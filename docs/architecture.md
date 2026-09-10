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

Extraction and the first downsample are one pass: each pyramid-level-0 pixel is
the average of the extracted light in the block of source pixels under it, so a
single very bright pixel still contributes its full energy rather than being
thresholded away after averaging.

### Multi-scale diffusion

The kernel is a weighted sum of Gaussians, one per octave:

```
level 0 ── blur σ ──┬─────────────────────────── w₀
                    ↓ downsample ×½
level 1 ── blur σ ──┬─────────────────────────── w₁
                    ↓ downsample ×½
level 2 ── blur σ ──┬─────────────────────────── w₂ …
```

Because the blurs cascade, level *i* carries an effective σ of about
`σ_level · 1.155 · 2ⁱ`. The weights follow a log-normal envelope centred on the
requested σ, one octave wide either side, so the result is a smooth mixture:
the narrow octaves build the bright core and the wide ones the long tail. The
octave ladder is what keeps large radii cheap — a radius of 400 costs no more
than a radius of 40.

The collapse runs from the top down, each level being upsampled into the next
finer one and added with its weight, so only one buffer per level is ever live.

Measured falloff of a point source (from `abglow_preview`):

| Radius | 50 % | 25 % | 10 % | 1 % |
|--------|------|------|------|-----|
| 25 | 6 px | 9 px | 14 px | 28 px |
| 100 | 21 px | 35 px | 53 px | 109 px |
| 400 | 85 px | 141 px | 214 px | 438 px |

so the Radius control reads as "where the glow ends", and the profile keeps a
tight core (half brightness at a fifth of the radius) with a long tail.

### Energy

Every filter in the chain is normalised, so the glow conserves the light it
extracts: widening the radius spreads the same energy over more area and the
glow dims, exactly as a real light source would. Exposure and Intensity are the
controls for putting that brightness back.

### Compositing

The glow is added in linear light and re-encoded for the output depth. Where
the glow is exactly zero the source pixel is copied through bit-exactly, so an
untouched area of the frame is never altered by an encode/decode round trip.
Alpha grows as `a + glow_a·(1 − a)`, clamped to 1, so the glow is visible where
the layer was transparent without breaking premultiplication.

For 8 and 16 bpc output a ±1 LSB triangular dither is applied to the pixels the
glow touches, which removes the stair-stepping a wide, shallow gradient would
otherwise show (measured: longest identical run along a gradient drops from 33
to 12 pixels in 8 bpc).

## After Effects integration

* **SmartFX** (`PF_OutFlag2_SUPPORTS_SMART_RENDER`) is required to see 32-bit
  float pixels at all, so it is the primary path. `PF_Cmd_RENDER` is still
  implemented for hosts that do not drive SmartFX.
* **Pre-render** asks for the layer's extent with an empty request, computes the
  glow's reach from the parameters at that time, and declares
  `result_rect = max_result_rect = layer ⊕ reach` together with
  `PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS`. `max_result_rect` never depends on
  the requested region, which is what After Effects requires.
* **Whole-frame rendering.** The glow at one pixel depends on a wide
  neighbourhood, so the effect always renders its full result rather than
  sub-regions. After Effects caches the frame, so a tiled request would only
  recompute the same pixels with a wider halo.
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

4K (3840×2160), 4 cores, Normal quality, 8 bpc:

| Radius | Before optimisation | Now |
|--------|--------------------|-----|
| 20 | 746 ms | 78 ms |
| 100 | 113 ms | 72 ms |
| 400 | 157 ms | 108 ms |

What mattered, in order: restructuring the blur so the tap loops vectorise
(taps in the outer loop, pixels in the inner loop), giving level 0 a pixel
budget so large frames start the pyramid lower, and precomputing the upsample
taps per column instead of clamping inside the composite loop. Denormals are
flushed for the duration of each pass, because the tail of a glow decays
straight into the denormal range.
