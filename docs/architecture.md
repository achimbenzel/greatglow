# Architecture

The plug-in is **Profound Glow**, match name `ABBZ ProfoundGlow`.

## Layers

```
AbGlowEntry.cpp      EffectMain, command dispatch, PluginDataEntryFunction2
AbGlowParams.cpp     parameter definitions, reading them into GlowSettings
AbGlowRender.cpp     SmartFX pre-render / render, rectangle maths
AeAdapters.cpp       host memory and host thread pool behind core interfaces
────────────────────────────────────────────────────────────────────────────
core/GlowPipeline    orchestration: extract, diffuse, colour, composite
core/GlowPlan        how many octaves, at what scale, with what weights, per model
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

### Straight alpha

After Effects hands an effect **straight** pixels at every depth: an
anti-aliased edge keeps its full colour, and alpha says how much of the pixel
it covers. The SDK's own sampling macros ask for `PF_MF_Alpha_STRAIGHT`, and
`preserve_rgb_of_zero_alpha` only means anything if colour survives under zero
alpha. The glow is computed premultiplied, so `HostImage` carries an
`AlphaMode`, the plug-in marks After Effects' worlds `kStraight`, and the
pipeline converts on the way in (colour decoded as it stands, then weighted by
coverage) and on the way out.

Until v1.2.1 every pixel was read and written as if it were premultiplied,
and the mock host was written from the same assumption, so nothing caught it.
In After Effects that meant:

* **Edges emitted far too much light.** The colour was divided by the coverage
  a second time, so a pixel a tenth covered glowed as if it were ten times as
  bright. Every anti-aliased edge grew a ragged fringe of light, dense edges
  such as small text turned into blobs, and the fringe changed with the
  coverage, so it crawled whenever the layer moved: between quarter-pixel
  steps of a moving ring the glow changed by up to 55% and its total by 21%.
* **The glow was stronger at reduced resolution**, where more of a layer is
  edge: 1.91× at Third against Full on text. Now 1.005×.
* **Edges went hard with the glow on.** The composite read the edge's full
  colour as light over black, which needed full coverage to show, so every
  anti-aliased edge came out opaque and stair-stepped.

`TestStraightAlphaMatchesPremultiplied` holds the straight path to the
premultiplied one within 2.5/255 at every pixel, as the host will show it;
`TestStraightEdgesStayAntiAliased`, `TestGlowDoesNotFlicker` and the
straight-pixel run of `TestBrightnessIsResolutionIndependent` fail on the old
behaviour, and the mock host checks an anti-aliasing ramp through the built
plug-in. The core's own tests default to premultiplied images so that a glow's
light can be read straight off a pixel.

### Highlight extraction

Pixels are read in whatever depth the host is rendering, converted to
premultiplied linear light, and un-premultiplied again only to judge a pixel's
own brightness. The brightest channel drives a soft-knee threshold:

```
above = level - threshold
soft  = clamp(above + knee, 0, 2·knee)² / (4·knee)
gain  = max(soft, above) / level
```

Using `max(r, g, b)` rather than luminance means a saturated blue shape glows as
readily as a white one, which is what people expect from a glow. Above the knee
the gain approaches 1, so an HDR highlight of 50.0 keeps almost all of its
energy instead of being clipped to white.

`level` is the pixel's own brightness — its straight colour — not its
brightness times its coverage. A half-covered pixel on the edge
of a bright glyph is bright; it just covers less area, and it should emit half
the light rather than fail the threshold for looking dim. That makes the
extracted light exactly linear in coverage, which is what the resolution
independence below rests on: After Effects renders a reduced-resolution preview
from a downsampled layer, so the same edge arrives as fewer, more partially
covered pixels. Thresholding the premultiplied value instead cost 8% of the
glow's light at Quarter resolution on anti-aliased text — a visible difference
that no amount of getting the pyramid right would have fixed.
`TestBrightnessIsResolutionIndependent` asserts 2%; measured spread is 0.1%.

A pixel cannot say on its own whether it is the edge of a bright shape or a
bright colour at almost no opacity — a soft light or a fade inside a precomp,
stored straight at 1/255. Judged on its own colour, such a near-invisible layer
passed the threshold, and since 8 bpc alpha comes in steps of 1/255, each step
doubled or tripled its light: a disc of posterised rings far past the radius,
in whatever colour the invisible pixels stored (v1.2.1, fixed in v1.2.2). The
neighbours tell the two apart. An edge sits next to a covered pixel; a faint
layer does not. The pixel is judged on its own brightness scaled by the most
coverage in its 3×3 neighbourhood, up to a half, so edges keep the linearity
above and a faint layer is judged on roughly the light it shows.
`TestFaintLayerDoesNotGlow` covers it. Alpha is also dithered with the colour
now: stored straight, a glow over transparency carries its brightness in alpha,
and undithered alpha banded the tail.

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

Two models share the pyramid; the **Glow Model** parameter picks one. This
section and the next describe *Classic*; [the inverse-square
model](#the-inverse-square-model), the default, reuses the same pyramid and
reconstruction but weights it differently.

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
used. The widest octave carries the requested σ and the ladder runs down from it
in halves, six octaves at Normal quality, so the finest is a fixed fraction of
the radius.

**The octave weights come from the glare law, not from taste.** Veiling glare
follows a power of the angle: the Stiles–Holladay form the CIE disability-glare
equations use is 1/θ², steepening towards 1/θ³ close in. A sum of Gaussians
whose σ double reproduces 1/rⁿ when the octave weights go as `σ_k^(2−n)` —
octave *k* contributes a peak density of `w_k / σ_k²`, and setting that
proportional to `σ_k^−n` is what puts the curve on the law. The **Falloff**
control is *n* directly, defaulting to the physical 2.0.

Measured by fitting log intensity against log radius across the octave range of
a rendered point source:

| Falloff asked for | measured exponent | fit R² |
|-------------------|-------------------|--------|
| 2.0 | 2.047 | 0.9991 |
| 2.5 | 2.508 | 0.9995 |
| 3.0 | 2.985 | 0.9996 |

**The ladder carries one octave past the radius.** Without it the widest octave
*is* the radius, so the profile stops being a power law there and becomes that
octave's Gaussian shoulder — the glow was gone by 1.5× the radius and exactly
zero by 2×, which reads as a blur that stops rather than light trailing away.
Measured against the value a quarter of the radius out, with and without it:

| Distance | widest octave = radius | one octave past |
|----------|------------------------|-----------------|
| 1.0 × radius | 0.0065 | **0.0339** |
| 1.5 × radius | 0.00006 | **0.0088** |
| 2.0 × radius | 0 | **0.0014** |
| 3.0 × radius | 0 | **0.00001** |

The other half has to hold too: a large bright region must not lift the whole
frame. Its glow in an empty corner measures 0.000% of its own core, because the
far octaves are spread over so much area that their peak density is negligible
even carrying equal energy. `TestGlowHasALongTailWithoutHaze` asserts both ends.
A second tail octave was measured too — it reaches to 5× the radius and stays
haze-free, but it puts the bounds at 4.1× the radius, which is a 92 MP buffer on
a 4K comp and past the expansion cap anyway. One octave puts them at 2.05× and
costs about 1.5× the render time, all of it in the larger composite.

**The law is cut off at the radius.** A power law is scale-free — 1/rⁿ has no
characteristic size — so weighting the octaves by it alone left Radius changing
the glow's brightness and barely its size. The weights are
`(σ_k/σ_R)^a · e^(−σ_k/σ_R)`: a gamma spectrum, power law below the radius and
exponential above it. `a` is fitted so the rendered skirt lands on the Falloff
the user asked for — the pure `2−n` would be right without the cutoff, but the
cutoff steepens the profile below it too.

Falloff then trades two things against each other, measured:

| Falloff | Radius authority | Speck contrast | 80 px vs 600 px shape |
|---------|------------------|----------------|------------------------|
| 1.0 | 2.06× | 0.051 | 35% |
| **1.4** (default) | **1.81×** | **0.083** | **20%** |
| 2.0 (inverse-square) | 1.53× | 0.162 | 7% |

Lower puts more light at the wide scales, so Radius has more say in the glow's
apparent size; higher concentrates it and makes individual highlights glow on
their own. The default sits where the glow is about the size the octave-per-
scale predecessor produced while keeping most of the bloom character.

`TestFalloffFollowsThePowerLaw` holds the fit to ±0.15 below the cutoff. An earlier hand-tuned weight
tilt measured 1.61 — far shallower than real glare, which is why the glow read
as haze rather than as light.

Concentrating the weight on a single scale, which is what a log-normal envelope
does, is a band-limited blur rather than a glow. Measured against this ladder:

| | one scale (0.01/0.23/0.52/0.24) | six octaves on the law |
|-|-------------------|-------------|
| Local contrast left in a field of bright specks | 0.025 | **0.135** |
| Peak glow, 80 px shape vs 600 px shape | 76% apart | **10.8% apart** |

The first is why a noise field came out blurred instead of glowing; the second
is why a short word looked dimmer than a long one at the same settings.

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
| 25 | 1 px | 2 px | 4 px | 13 px |
| 100 | 1 px | 2 px | 5 px | 22 px |
| 400 | 3 px | 5 px | 9 px | 51 px |

These are far tighter than a Gaussian's because a bloom's peak is dominated by
its finest octave — a point source produces a spike, which is exactly what
glare does. They are not a measure of how far the glow reaches: **Radius sets
the widest octave's σ**, so the halo around an extended shape carries out to
roughly the radius, and the expanded bounds follow the widest octave rather
than the mixture's RMS for the same reason.

### The inverse-square model

This is the look Deep Glow is known for, and it was built by measuring it. A
Deep Glow render of white text was fitted as a sum of Gaussians, one per
half-octave, with non-negative weights: the light it spreads is **0.3 to 0.4 of
the source per octave, the same in every octave from about a pixel out to
about 100 px, and gone by about 180 px**. Equal light per octave is exactly
what a 1/r² glare is — the Classic ladder at Falloff 2.0 has that shape too —
but three things set it apart, and each one is visible:

1. **Nothing is normalised.** Every octave carries the same light however many
   there are, so the total is several times the source's light and grows with
   the log of the radius. Classic divides the same light between its octaves,
   so its glow next to a thin stroke is a fraction of Deep Glow's.
2. **The core is a fixed size.** The finest octave is about one pixel in the
   composition whatever the radius. Classic's finest octave is a fixed fraction
   of the radius, so at a large radius the bright line hugging the shape is
   gone.
3. **Radius is reach.** A larger radius adds octaves at the wide end and leaves
   the core alone — the glow reaches further and gets brighter. In Classic the
   whole kernel scales, and a larger glow is a dimmer one.

The weights come from a density over log σ, in units of the radius sigma
`s = σ / σ_R`:

```
ρ(s) = gain · s^(2−n) · exp(−s²/2)
```

`s^(2−n)` is the power law — flat at the default *n* = 2 — and the Gaussian
factor is the cutoff: untouched below the radius, an eighth at twice it, gone
by three times it. Falloff redistributes the light between the core and the
halo but the total is always that of *n* = 2, so it changes the glow's shape
and not its brightness. `gain` is `kInverseSquareOctaveGain`, 0.45 per octave,
fitted so the default threshold of 0.5 lands on the reference.

Each rung of the ladder carries the integral of ρ over the band of scales
around it, from the geometric midpoint with the rung below to the one with the
rung above. The first rung also takes everything down to the half-pixel core,
and the last everything past the radius. Integrating instead of sampling is what
makes the total independent of where the grid put the rungs, so it holds
across resolutions and qualities. Measured, rendered white text against the
reference, as the median 8 bpc code value at each distance from the letters:

| Distance (px) | 1–3 | 3–6 | 6–10 | 10–20 | 20–40 | 40–80 | 80–150 | 150–200 |
|---------------|-----|-----|------|-------|-------|-------|--------|---------|
| Deep Glow | 181 | 151 | 127 | 104 | 73 | 43 | 21 | 9 |
| Inverse Square, radius 400 | 180 | 157 | 137 | 110 | 77 | 49 | 26 | 11 |
| Previous default (Classic, radius 40) | 56 | 40 | 28 | 15 | 4 | 1 | 0 | 0 |

Below the cutoff the rendered profile of a point source follows the law it was
asked for — fitted exponent 1.595, 2.057 and 3.028 for Falloff 1.5, 2.0 and 3.0.
`TestInverseSquareFollowsThePowerLaw` holds it to ±0.12, and
`TestRadiusExtendsTheReachNotTheCore` checks the third point above: going from
radius 100 to 400 moves the glow 2 px from a bar by 18% and 150 px from it by a
factor of 28.

**The rungs sit still; the weights move.** Classic solves the per-level blur so
a rung lands on the radius, which means the rungs slide as the radius animates.
That is harmless when the whole kernel scales, but here it would move the core.
The inverse-square ladder uses a fixed per-level blur of 1.8 level pixels, so
the rungs are fixed sizes — 1.9, 4.2, 8.5 px and on up at a step of 1 — and only
their weights follow the radius. A new rung only ever appears past twice the
radius, where the density is an eighth of its plateau, so the radius can be
animated without a step.

The per-level blur has to be wide enough that halving the level does not alias.
It was 1.0 at first, which kept the finest rung at 1.1 px, but a thin edge
moving across the grid rippled by 1.3% along its length and the halo carried
kinks that read as rings. At 1.8 the ripple is 0.55%; see
[Reconstruction](#reconstruction) for the rings.

**Two tiers, so the budget never touches the core.** The level-0 pixel budget
has to hold the glow's whole reach, which is several times the radius. Fitting
that into the budget by coarsening the pyramid step, the way Classic does,
coarsens the core with it: when a radius animation pushed the step from 1 to 2,
the finest rung went from 1.1 px to 2.3 px and the glow 1 px from the edge
jumped by 21%. Instead the step is chosen from the layer's own size and the
quality, never the radius, and the pyramid splits in two:

```
core tier   levels 0 … k−1   the layer plus what their blurs spill (a few px)
halo tier   levels k … top   the whole reach, anchored to multiples of its pixel
```

`k` is the first level whose full span fits the budget, and it is zero — one
tier, as in Classic — whenever the reach is small. `DownsampleHalfInto` feeds
the first halo level from the last core level at an offset, reading zeros past
the core tier's edge, and each tier collapses on its own. The composite
reconstructs both and adds them: the core at the pyramid step, over the layer
only, and the halo at `step · 2^k` everywhere. Where `k` changes nothing moves,
because the extents change and the content does not. Sweeping the radius from
180 to 260 in steps of 4, across a change of tier, the largest step anywhere
within 16 px of a bar is 0.9%, all of it the light the larger radius adds;
`TestInverseSquareDoesNotPop` fails on the old behaviour. The same split also
keeps memory flat: a 1080p layer at radius 4000 builds a 12-level ladder whose
halo starts four levels up.

Everything the Classic model guarantees, the inverse-square one is tested for
too — each of these runs for both models:

| | Classic | Inverse Square |
|-|---------|----------------|
| Glow slides as the radius animates (`TestGlowDoesNotSlideWithRadius`) | 0.002 px | 0.001 px |
| Size spread across Full / Half / Third / Quarter | 0.68% | 0.71% |
| Brightness spread across Full / Half / Quarter, straight or premultiplied | 0.13% | < 0.001% |
| Centroid drift over sub-pixel motion | < 0.0001 px | 0.0001 px |
| Glow change between eighth-pixel steps, beyond the motion (`TestGlowDoesNotFlicker`) | 0.36% | 0.40% |
| A requested window against the full render | < 0.1% | < 0.1%, two tiers |

On a 4K layer with the settings from a user report (radius 4000, intensity
39%), the glow over the transparent area comes out within 1.3% across the four
Quality settings and within 2.5% between Full and Third.

### The core controls

The core is the light of the finest octaves, the soft rim hugging the source.
What counts as core is fixed: each rung's share of it is
`exp(−(σ / σ₂₀)² / 2)`, with σ₂₀ the sigma of the default Core Radius, 20 px.
Core Radius moves that light to the rungs around its own size, in the
proportions the plan already gives them, so the rim gets wider and softer or
tighter and harder while carrying the same light; Core Intensity scales it.
The rest of each rung is halo and stays where Radius and Falloff put it, and
the reach, the pyramid and the bounds do not change. At 20 px and 100% the plan
is returned untouched, so a project saved before the group existed renders
bit for bit as before.

The first version only scaled the octaves below Core Radius by Core
Intensity, which left Core Radius doing nothing at 100% — the one setting that
mattered. `TestCoreIsSetApartFromTheHalo` now checks the identity at the
defaults, the rim 6 px from a shape going from 0.179 to 0.222 at 80 px and
0.153 at 5 px at 100%, the total light staying within 2%, and the halo 200 px
out staying within 5% when the core is turned off. Classic has little to move
at large radii, because its finest octave is a fraction of the radius.

Core Softness changes where the moved light lands, not how much of it there is.
The placement past the core radius flattens from `exp(−s² / 2)` towards
`exp(−s^0.6 / 2)`, with `s = σ / σ_core`, and the rungs finer than the core
radius are weighted by `s^softness`, which takes light off the hard line the
finest octaves draw along the edge. At 0% both factors are 1 and the plan is
unchanged. `TestCoreSoftness` checks that at 100% the rim 40 px out doubles,
the glow 100 px out rises by more than 30%, the shape's own face gets darker
and the total light stays within 2%.

### Per-axis decimation and Aspect Ratio

Aspect Ratio scales one axis of every octave: `f_x = min(1, a)`,
`f_y = min(1, 1 / a)`, multiplied by the ratio of the two radii (pixel aspect,
per-axis downsampling). Each axis then gets its own schedule through the
pyramid. Level `i` of a round glow adds a blur of `level_sigma · scale · 2^i`
render pixels; a squeezed axis adds `f` times that, and it is halved going
into level `i` only if that blur is still at least a full level blur at the
coarser pixel. So an axis at `f = 1` halves every level, as before, and a
squeezed one stays at a finer pixel until its blur has caught up. The blur
within the level is the wanted blur in that level's pixels. Where both axes
halve together the original half-size filters run, which is what keeps a
round glow bit for bit what it was; otherwise the same tent (down) and
quadratic B-spline (up) are applied to the halved axis only and the other one
is passed through. The halo tier's step, the tiers' origins and the core tier's
margin are per axis too, and a squeezed axis only spans its own reach.

`TestAspectRatio` checks both models at Draft and Best: 1 is round, 2 and 0.5
are 2:1 within 10%, 0 spreads no further sideways than the source and the
extraction, and the total light stays within 3%. The window test runs at 0.4
and 2.5 as well, so the per-axis grid is anchored as firmly as the round one.

### Energy

In the Classic model every filter in the chain is normalised, so the glow
conserves the light it extracts: widening the radius spreads the same energy
over more area and the glow dims, exactly as a real light source would.
Exposure and Intensity are the controls for putting that brightness back.

Inverse Square does not conserve it, deliberately: a larger radius adds the
light a wider glare would catch. The total grows with the log of the radius —
about 2.1× the extracted light at radius 40, 3.6× at 400 and 5.1× at 4000.

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
a cubic one (4 taps, C²).

The collapse that feeds it has to be as smooth. Each rung is upsampled into the
one below before the final reconstruction, and that upsample used to be
bilinear: a kink at every coarse sample, and in a strong, wide glow the kinks of
successive levels lined up into faint concentric rings. It is a quadratic
B-spline now — at the two phases a 2× upsample needs, three taps of
(0.28125, 0.6875, 0.03125) and their mirror. `TestNoRingsInTheHalo` measures the
curvature of the log profile out of a small disc, where a power law is a smooth
*n*/r²: the 99th percentile over the median was 4.7 for Inverse Square with
the bilinear collapse and its first per-level blur, and is 2.0 now (2.2 for
Classic). Cubic everywhere costs 50–75% more for no visible
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

**The glow is written as light over black.** After Effects blends 8 and 16 bpc
layers in their encoded space, so what a pixel shows over black is its
premultiplied value as it stands. The glow used to be written the usual way for
a coloured pixel — its colour unpremultiplied by the glow's coverage, encoded,
and premultiplied again. Over black that shows `Encode(light / a) · a`, and the
concave curve makes that darker the less coverage there is: a glow spilling
into a transparent layer showed about a third of its light at the edge and a
tenth of it in the tail, which is why the old glow on text looked thin however
it was set. In an encoded working space the result is now built as the source's
own light over black plus the glow, encoded, with alpha the largest encoded
channel — the least coverage that can carry that colour, which over anything
brighter than black composites like Screen. An opaque pixel comes out exactly
as before. In a linear working space premultiplied values already are light
over black, and alpha grows as `a + glow_a·(1 − a)`, clamped to 1.
`TestGlowIsLightOverBlack` compares an 8 bpc render with the float light it
should show: within 2.8% for both models. The result is then written in the
destination's own convention — straight, for After Effects.

Where the lit result leaves the output's range, **Highlight Rolloff** decides
what happens. Clipping each channel independently reaches the ceiling at a
different brightness per channel, so a saturated colour driven hard enough drags
to white — a light red of (1, 0.08, 0.08) at 60x gain clips to (1, 1, 1). The
default rolls the whole triple off together through a smooth shoulder above
0.75, which leaves the ratios between channels — the hue — untouched: the same
pixel comes out (1, 0.078, 0.078), the source's colour at full brightness. Float
output keeps its HDR values and is never touched.

**Burn to White** does on purpose, and smoothly, what clipping does by
accident. The shoulder that preserves the hue says how much light the brightest
channel has to lose to fit, and that much overexposure pulls the other channels
up towards it: `white = 1 − exp(−(m − shoulder(m)))`. Below the knee nothing
changes and the join is C¹, so no contour is drawn where burning starts. A
saturated cyan driven to three times its level comes out (0.99, 1, 1) at its
core while its glow, which the output can show, keeps its colour. Float output
is burned too but keeps the brightest channel's HDR value.

All three work on light over black, not on the unpremultiplied colour: the
faint tail of a glow over a transparent layer has the source's full colour at
low coverage, and reading that as a bright colour dimmed it by a tenth under
Preserve Hue though it was nowhere near the ceiling.

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
* **Parameter layout is a file format.** Every project that uses the effect
  stores its parameter values by index, and their ids alongside. Both enums are
  therefore spelled out with literal numbers and pinned by `static_assert`: a new
  parameter takes the next free number and is appended, and anything else is a
  compile error. Inserting one in the middle once shifted three ids and made
  projects saved by the previous build open with their groups mismatched.
* **PiPL.** After Effects still reads the PiPL resource, and its flags must
  agree with `PF_Cmd_GLOBAL_SETUP`. `tools/generate_pipl.py` writes the resource
  and a header with the same flag values, which `AbGlowEntry.cpp`
  `static_assert`s against the SDK enums — the two cannot drift apart.

## Performance

Classic, 4K (3840×2160), 4 cores, 8 bpc, milliseconds — best of five renders:

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

Inverse Square, measured the same way on text-like content, with the output
the size of the layer:

| Radius | Draft | Normal | High | Best |
|--------|-------|--------|------|------|
| 20 | 136 | 139 | 291 | 284 |
| 100 | 188 | 199 | 331 | 379 |
| 400 | 225 | 230 | 416 | 652 |
| 1000 | 226 | 274 | 440 | 621 |

Classic on that content and machine measured 120 / 185 / 283 / 200 ms at
Normal for the same radii, so the two cost about the same: the core tier is at
most the layer, and the halo tier starts as far up the ladder as the budget
needs. A 4K layer is over the Normal budget on its own, so its core is built at
a step of 2 — a 3.8 px finest octave, which at 4K is the same size in the frame
as 1.9 px at 1080p.

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
