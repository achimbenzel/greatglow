# Architecture

## Layers

```
AbGlowEntry.cpp      EffectMain, command dispatch, PluginDataEntryFunction2
AbGlowParams.cpp     parameter definitions, reading them into GlowSettings
AbGlowRender.cpp     SmartFX pre-render / render, rectangle maths
AeAdapters.cpp       host memory and host thread pool behind core interfaces
────────────────────────────────────────────────────────────────────────────
core/GlowPipeline    orchestration: extract, diffuse, colour, composite
core/GlowPlan        how many octaves, at what scale, with what weights
core/Blur            separable Gaussian
core/Resample        pyramid downsample and weighted upsample
core/Transfer        sRGB ⇄ linear, table driven
core/Color           exposure, intensity, saturation, tint
```

`src/core` knows nothing about After Effects. It takes plain buffer
descriptions (`HostImage`), an `Allocator` and a `TaskRunner`, which is what
makes the unit tests, the preview renderer and the sanitizer runs possible
without the host.

## The glow

### Highlight extraction

Pixels are read in whatever depth the host is rendering, converted to linear
light and un-premultiplied only where alpha is partial. The brightest channel
drives a soft-knee threshold:

```
above = level - threshold
soft  = clamp(above + knee, 0, 2·knee)² / (4·knee)
gain  = max(soft, above) / level
```

Using `max(r, g, b)` rather than luminance means a saturated blue shape glows as
readily as a white one, which is what people expect from a glow. Above the knee
the gain approaches 1, so an HDR highlight of 50.0 keeps almost all of its
energy instead of being clipped to white.

`level` is the pixel's own brightness — the premultiplied value divided by
alpha — not its brightness times its coverage. A half-covered pixel on the edge
of a bright glyph is bright; it just covers less area, and it should emit half
the light rather than fail the threshold for looking dim. That makes the
extracted light exactly linear in coverage, which is what the resolution
independence below rests on: After Effects renders a reduced-resolution preview
from a downsampled layer, so the same edge arrives as fewer, more partially
covered pixels. Thresholding the premultiplied value instead cost 8% of the
glow's light at Quarter resolution on anti-aliased text — a visible difference
that no amount of getting the pyramid right would have fixed.
`TestBrightnessIsResolutionIndependent` asserts 2%; measured spread is 0.1%.

Extraction and the first downsample are one pass: each pyramid-level-0 pixel
gathers the extracted light under it, so a single very bright pixel still
contributes its full energy rather than being thresholded away after averaging.

The gather is a **tent spanning two cells**, not a box over one. A box carries
the light but not its centre of mass: it places each sample at its cell's
centre, and the difference between that and where the light actually sits inside
the cell oscillates as content crosses the grid. A shape moving smoothly then
makes the glow lead and lag by up to a twelfth of a cell — a sawtooth with the
period of the pyramid step, ±0.6 px at a step of 15 and ±1.3 px at 30, which is
the shimmer seen on a moving layer. A tent reproduces linear functions, so first
moments survive the decimation: measured drift between the source's centroid and
the glow's, over sub-pixel motion, drops from that sawtooth to 0.0004 px at
every radius from 100 to 1600. At a step of 1 the tent degenerates to (0, 1, 0)
and costs nothing; the two passes are separable and each source row is filtered
once, so the whole change costs about 3%.

### Multi-scale diffusion

The kernel is a weighted sum of Gaussians, one per octave:

```
level 0 ── blur σ ──┬─────────────────────────── w₀
                    ↓ downsample ×½
level 1 ── blur σ ──┬─────────────────────────── w₁
                    ↓ downsample ×½
level 2 ── blur σ ──┬─────────────────────────── w₂ …
```

Because the blurs cascade, level *i* carries an effective σ of
`σ_level · sqrt((4ⁱ⁺¹−1)/3)` in level-0 pixels — asymptotically `1.155 · 2ⁱ`,
but noticeably smaller on the first few rungs, which is why the exact form is
used. The weights follow a log-normal envelope centred on the requested σ, so
the narrow octaves build the bright core and the wide ones the long tail. The
ladder is what keeps large radii cheap: a radius of 400 costs no more than a
radius of 40.

The collapse runs from the top down, each level being upsampled into the next
finer one and added with its weight, so only one buffer per level is ever live.

### Keeping the kernel the same shape everywhere

The same glow has to measure the same size whether the user is at Full, Half or
Quarter resolution, whichever Quality they picked, and it has to stay put while
the Radius is animated. Several things make that true, and each of them was a
visible defect before it was fixed.

**A fixed centre rung.** The requested σ is placed on rung 2 of the ladder by
solving for `σ_level`, and the pyramid step (`base_scale`) is any integer — not
a power of two — chosen to put it there. Its ceiling has to be derived from the
working buffer rather than fixed, for the same reason: a constant cap binds at
Full and not at Quarter, and binds for Draft and not for Best, so the step stops
tracking what was asked for and the ladder grows an extra rung to compensate.
That put 7.4% between the four Quality settings at radius 700, and left Full and
Quarter building different ladders for the same glow. Tied to the buffer, both
stay within 3% at every radius and the ladder is four rungs throughout. The mixture's weights depend only on
the ratios `CascadeFactor(i) / CascadeFactor(centre)`, so fixing the rung fixes
the shape of the kernel; only the grid it is sampled on changes. The rung moves
only when `base_scale` has hit a limit — a tiny radius, a huge one, or the
level-0 pixel budget.

**Quality changes the grid, not the glow.** `TargetLevelSigma` is the per-level
blur in level pixels: 1.3 / 1.8 / 2.3 / 2.8 for Draft / Normal / High / Best. A
larger value means a finer pyramid for the same glow, so Best resolves more
structure and costs more, while the rung — and therefore the kernel — is the
same. The resampling filters do add blur of their own, a fixed amount in level
pixels, so a coarse pyramid would come out wider; `kResamplingVariance` takes
that back off the requested σ. Measured 50% width of the same glow across the
four Quality settings:

| Radius | Draft | Normal | High | Best | spread |
|--------|-------|--------|------|------|--------|
| 100 | 44.6 | 44.4 | 44.7 | 44.2 | 1.0% |
| 352 | 141.4 | 144.0 | 144.2 | 145.2 | 2.7% |
| 700 | 289.1 | 292.8 | 297.2 | 291.9 | 2.8% |
| 1500 | 717.4 | 712.8 | 723.7 | 716.9 | 1.5% | It also makes the bounds expansion independent of
Quality, so switching it does not re-render the whole comp's geometry.

**Every source pixel reaches the pyramid.** Level-0 columns whose block only
partly overlapped the source used to be dropped, which threw away up to
`base_scale − 1` columns of the layer's right and bottom edge. That moved the
layer's centre of mass, so the glow jumped sideways by several pixels whenever
the radius changed `base_scale` — ±3 px over a radius animation from 200 to 240
— and made the same glow a different size at different resolutions. Partial
blocks are now summed over the samples that exist and divided by the full block
area, which is ordinary box filtering and preserves both energy and centroid.

**A grid anchored to the source.** Level 0 used to start at the corner of the
expanded output buffer, and that corner moves as the radius grows. Whenever
`expansion % base_scale` changed, the whole pyramid shifted by up to half a
level pixel. The grid is now anchored to source pixels (`GridStart`), so a given
source pixel always falls in the same place within its level-0 block.

With both of those fixed, the centroid moves 0.05 px over a radius sweep from
200 to 240 on a layer whose width is not a multiple of any pyramid step; before,
it jittered over ±3 px frame to frame. `TestGlowDoesNotSlideWithRadius` locks
it in.

**Kernels that are actually the σ they claim.** The Gaussian is truncated at 4σ
rather than 3σ: the discarded weight drops from 1.1% to 0.006%, and the realised
σ from up to 0.9% narrower than requested to 0.03%, so `ceil()` landing one tap
either way no longer matters. The expanded bounds reach out 3.6·σ_effective
(≈1.43·Radius), which leaves the 1% point of the profile at 73% of the reach —
at 3.0 it fell outside the buffer at Third resolution and was clipped flat.

Measured width of the same comp-space glow rendered at each AE resolution, in
comp pixels (`TestSizeIsResolutionIndependent` asserts 2%):

| | Full | Half | Third | Quarter |
|-|------|------|-------|---------|
| 50% | 394.6 | 394.6 | 392.1 | 395.3 |
| 10% | 795.7 | 795.8 | 791.9 | 798.5 |
| 1% | 1443.7 | 1443.7 | 1438.8 | 1450.8 |

0.8% spread, against 2.5% before these fixes. Size is only half of it, though
— see the extraction section above for why the same glow used to come out
*dimmer* at reduced resolution even when its size was right.

Measured falloff of a point source (from `abglow_preview`, half-width at each
fraction of the peak):

| Radius | 50 % | 25 % | 10 % | 1 % |
|--------|------|------|------|-----|
| 25 | 5 px | 8 px | 13 px | 26 px |
| 100 | 20 px | 34 px | 53 px | 105 px |
| 400 | 81 px | 134 px | 211 px | 419 px |

so the Radius control reads as "where the glow ends", and the profile keeps a
tight core (half brightness at a fifth of the radius) with a long tail. The
numbers are half-widths of a symmetric profile; measuring outward from the peak
instead makes them depend on where the source pixel sits inside its level-0
block, which is a property of the measurement and not of the glow.

### Energy

Every filter in the chain is normalised, so the glow conserves the light it
extracts: widening the radius spreads the same energy over more area and the
glow dims, exactly as a real light source would. Exposure and Intensity are the
controls for putting that brightness back.

### Reconstruction

The final upsample out of level 0 must have a continuous first derivative.
Bilinear does not: it produces straight segments joined by a kink every
`base_scale` pixels, and although the numerical error is tiny, the eye reads
those kinks as concentric rings in a wide, faint glow. Measuring the curvature
along a profile makes it obvious — per position within an 8-pixel cell:

```
bilinear         0.00001 0.00001 0.00001 0.37238 0.39404 0.00001 0.00001 0.00001
quadratic spline 0.08180 0.08123 0.08555 0.09015 0.09499 0.10000 0.10391 0.10443
```

Draft and Normal use a quadratic B-spline (3 taps per axis, C¹), High and Best
a cubic one (4 taps, C²). Cubic everywhere costs 50–75% more for no visible
gain at the smaller pyramid steps those settings already use.
`TestNoUpsampleCreases` keeps bilinear from creeping back in: it bins the
second difference along a profile by position within a level-0 cell and fails if
the worst bin exceeds the best by 5×. Bilinear scores in the thousands; the
B-splines stay between 1.0 and 1.3 even at a 24-pixel step.

The filter is separable, and output rows share most of their vertical taps, so
`GlowRowCache` filters each pyramid row horizontally once and keeps four of
them. Per output pixel that is `taps` multiply-adds plus `taps/base_scale`,
instead of `taps²`.

### Compositing

The glow is added in linear light and re-encoded for the output depth. Where
the glow is exactly zero the source pixel is copied through bit-exactly, so an
untouched area of the frame is never altered by an encode/decode round trip.
Alpha grows as `a + glow_a·(1 − a)`, clamped to 1, so the glow is visible where
the layer was transparent without breaking premultiplication.

For 8 and 16 bpc output the quantisation is stochastic: a sample lands on one
of the two code values it sits between, with probability given by where it
falls. The mean is exact, and the stair-stepping a wide shallow gradient would
otherwise show is broken up — longest identical run along a gradient drops from
48 to 11 pixels in 8 bpc.

Two properties matter more than the noise shaping:

* **A representable value cannot move.** The usual ±1 LSB triangular dither
  rounds an exact zero up a quarter of the time. With expanded bounds most of
  the output buffer is untouched black, so that lit 12% of it to code value 1 —
  and because one dither value is shared by all three channels, the pixels that
  fired came out neutral: white speckle scattered around the layer on an
  otherwise black frame. Stochastic rounding leaves zero at zero.
* **It is keyed to the layer, not to the buffer.** The expanded rect moves as a
  layer's Position animates. A buffer-keyed pattern therefore reshuffles every
  frame: moving the layer one pixel changed 60% of the output pixels, which
  reads as a shimmer over the whole glow. Keyed to source pixels it is 1.3%, all
  of it ±1 LSB at the buffer edges.

`TestDitherIsQuietAndStill` covers both.

## After Effects integration

* **SmartFX** (`PF_OutFlag2_SUPPORTS_SMART_RENDER`) is required to see 32-bit
  float pixels at all, so it is the primary path. `PF_Cmd_RENDER` is still
  implemented for hosts that do not drive SmartFX.
* **Pre-render** asks for the layer's extent with an empty request, computes the
  glow's reach from the parameters at that time (3.6·σ_effective), and declares
  `result_rect = max_result_rect = layer ⊕ reach` together with
  `PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS`. `max_result_rect` never depends on
  the requested region, which is what After Effects requires.
* **The pyramid spans the glow, not the request.** After Effects renders only
  what it needs — the visible part of a zoomed viewer, a region of interest, a
  tile — so the same glow is asked for through windows of every size and
  position. The pyramid is built over the layer inflated by the glow's reach,
  in layer coordinates, and the composite then reads whatever window was asked
  for out of it. Sizing level 0 to the requested rectangle instead both
  discarded source pixels outside it and made the octave blurs clamp against
  its edge: a window covering the layer plus 40 px came out 4.4% brighter
  overall and up to 121% different on individual pixels, so the glow changed
  with the viewer's zoom, scroll and resolution, and flickered as a layer
  moved. `TestRegionOfInterestMatchesFullFrame` renders four windows and
  requires each to match the same pixels of the full-frame render.
* **Geometry.** The offset between the input and output buffers is derived from
  the layer-space rectangles the effect itself declared, cross-checked against
  the sizes the host actually handed back, falling back to the worlds'
  `origin_x`/`origin_y`.
* **Multi-frame rendering** (`PF_OutFlag2_SUPPORTS_THREADED_RENDERING`) is safe
  here because the effect keeps no sequence data and writes no globals during
  render. Parallelism inside a frame goes through
  `PF_Iterate8Suite1::iterate_generic`, which is the host's own pool, so
  concurrent frames do not oversubscribe the machine.
* **Memory** comes from `PF_HandleSuite1`, so After Effects can account for it.
  The level-0 buffer is capped by a per-quality pixel budget (4 MP at Normal),
  which keeps a 4K frame near 80 MB instead of 300 MB.
* **PiPL.** After Effects still reads the PiPL resource, and its flags must
  agree with `PF_Cmd_GLOBAL_SETUP`. `tools/generate_pipl.py` writes the resource
  and a header with the same flag values, which `AbGlowEntry.cpp`
  `static_assert`s against the SDK enums — the two cannot drift apart.

## Performance

4K (3840×2160), 4 cores, 8 bpc, milliseconds — best of five renders:

| Radius | Draft | Normal | High | Best |
|--------|-------|--------|------|------|
| 20 | 279 | 287 | 733 | 681 |
| 100 | 228 | 228 | 258 | 328 |
| 400 | 219 | 214 | 222 | 222 |
| 1000 | 210 | 207 | 208 | 210 |

Cost is flat in the radius, which is the whole point of the octave ladder, and
now rises with Quality rather than falling — the Quality control used to be
wired backwards, so Best built a two-octave pyramid on a coarse grid: cheaper
than Normal and a visibly different glow. Small radii are the expensive case,
not large ones: `base_scale` shrinks with the radius, so level 0 is at or near
full resolution and the level-0 blur dominates. The level-0 pixel budget is what
stops that from also costing hundreds of megabytes (4 MP at Normal keeps a 4K
frame near 80 MB).

What mattered, in order: restructuring the blur so the tap loops vectorise
(taps in the outer loop, pixels in the inner loop), giving level 0 a pixel
budget, precomputing the upsample taps per column instead of clamping inside the
composite loop, and caching the horizontally filtered pyramid rows. Denormals
are flushed for the duration of each pass, because the tail of a glow decays
straight into the denormal range.

The composite is the bottleneck — it touches every output pixel of an expanded
buffer with the reconstruction filter and the transfer functions — so the next
thing worth trying is hand-written SIMD over four pixels at a time there, or the
GPU path in `docs/gpu.md`.
