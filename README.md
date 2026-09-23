# Profound Glow

A native Adobe After Effects effect plug-in (`.aex`) that renders a
high-dynamic-range bloom using multi-scale light diffusion, written in C++17.

The glow is generated in linear light from a soft-knee highlight extraction and
diffused through a weighted Gaussian pyramid, which gives a bright concentrated
core with a long, smooth falloff — the look of light spilling, rather than a
blurred copy of the layer composited with Add.

```
source ─► highlight extraction (soft knee, linear light)
       ─► downsample to pyramid level 0
       ─► blur / downsample cascade (one Gaussian per octave)
       ─► weighted collapse back up (fine core to wide halo)
       ─► exposure · intensity · saturation · tint
       ─► composite over the original as light, expanding the layer bounds
```

The default **Glow Model** is *Inverse Square*: the same light in every octave
from a fixed one-pixel core out to the radius, which is what a 1/r² veiling
glare is. The core hugging the source is always there and Radius sets how far
the light reaches, adding light as it grows — the look Deep Glow is known for.
Measured against a Deep Glow render of white text, the glow at each distance
from the letters lands within about a dozen 8-bit code values of it, where the
previous default fell short by up to a hundred (see
[Architecture](docs/architecture.md#the-inverse-square-model)). *Classic* keeps
the earlier energy-conserving bloom, where the whole kernel scales with the
radius and a bigger glow is a dimmer one.

* 8, 16 and 32 bits per channel, with all internal maths in 32-bit float
* HDR safe: values above 1.0 are never clamped in 32 bpc
* SmartFX, multi-frame rendering, host memory and the host thread pool
* Cost stays nearly flat as the radius grows, because a wide glow is built on a
  coarser pyramid — at 4K, Normal quality, radius 20 to 1000 costs 110–250 ms
  on four cores under either model

## Layout

| Path | What it is |
|------|------------|
| `src/core/` | Image processing. No After Effects dependency, so it builds and is tested on any platform. |
| `src/plugin/` | After Effects integration: entry point, parameters, SmartFX render, host adapters. |
| `tools/generate_pipl.py` | Builds the PiPL resource, the `.rc` that embeds it, and the shared out-flags header. |
| `tests/` | Core unit tests, an offline preview renderer, and a mock After Effects host that loads the built `.aex`. |
| `cmake/` | SDK discovery and the mingw-w64 cross-compilation toolchain. |
| `docs/` | [Architecture](docs/architecture.md), [testing](docs/testing.md), [GPU notes](docs/gpu.md). |

## Building

### Requirements

* Windows 10/11 and Visual Studio 2022 (17.4 or newer if you want an ARM64 build)
* CMake 3.20+
* Python 3.8+ (used at configure time to generate the PiPL resource)
* The Adobe After Effects SDK — see [`third_party/README.md`](third_party/README.md);
  it is not redistributable, so it is not in this repository

### Visual Studio 2022

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DAE_SDK_ROOT="C:/AfterEffectsSDK/Examples"
cmake --build build --config Release
```

The plug-in lands at `build/Release/ProfoundGlow.aex`.

To build again after changing sources, re-run the second command. Re-running
CMake is only needed when files are added or `tools/generate_pipl.py` changes.

### Installing

```bat
cmake --install build --config Release
```

installs to `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\AB Tools`,
the shared folder that every recent After Effects version scans (an
Administrator prompt is expected). Override it with
`-DABGLOW_INSTALL_DIR="D:/My Plug-ins"`, or simply copy `ProfoundGlow.aex` into

```
C:\Program Files\Adobe\Adobe After Effects <version>\Support Files\Plug-ins\
```

Restart After Effects; the effect appears under **Effect ▸ AB Tools ▸ Profound Glow**.

Pick **one** of those two folders. After Effects scans both, so a copy left in
the other one can be the copy it loads, and replacing the file you were
thinking of changes nothing. The effect's parameters end with a group named
after the build it came from (`v1.3.1 (abc1234)`), so the loaded build can be
read straight off the panel. To find every copy on the machine:

```powershell
Get-ChildItem C:\ -Filter ProfoundGlow.aex -Recurse -ErrorAction SilentlyContinue |
    Select-Object FullName, Length, LastWriteTime
```

### Cross-compiling from Linux (verification only)

The Windows binary can be built and smoke tested on Linux, which is how CI
checks this repository:

```sh
sudo apt-get install mingw-w64 wine64
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake
cmake --build build-win
ctest --test-dir build-win --output-on-failure   # runs both suites through wine
```

Ship release builds from Visual Studio; mingw is for verification.

## Parameters

| Parameter | Range (default) | What it does |
|-----------|-----------------|--------------|
| Threshold | 0 – 4 (0.5) | Brightness where the glow starts, in working-space units: 0.5 is mid grey, 1.0 is white, above 1.0 only HDR highlights glow. |
| Threshold Softness | 0 – 100 % (40 %) | Width of the knee below the threshold. 0 is a hard cut; higher values ramp the glow in smoothly and avoid edges appearing in the glow. |
| Radius | 0 – 4000 px (150) | Under Inverse Square, how far the light reaches: the octaves run from a fixed core out to the radius and are cut off past it, so a larger radius carries the glow further and adds light without dimming the core. Under Classic, the size of the whole bloom, which dims as it grows. |
| Intensity | 0 – 10000 % (100 %) | Strength of the generated light. 0 turns the effect into a pass-through. |
| Exposure | −10 – +10 stops (0) | Brightness of the glow in stops; +1 doubles it. |
| Saturation | 0 – 400 % (100 %) | Colour of the glow: 0 is white light, 100 % keeps the source colour, above that exaggerates it. |
| Tint | colour (white) | Colour multiplied into the glow. |
| Tint Amount | 0 – 100 % (0 %) | How much of the tint is mixed in. |
| Quality | Draft / Normal / High / Best | Trades the resolution of the diffusion pyramid, and the reconstruction filter used to scale it back up, against speed. It changes how finely the glow is resolved, not its size or shape — measured spread across the four settings is under 2.5%. |
| Composite | Add / Screen / Glow Only | How the glow is combined with the source. Glow Only is useful for inspecting the glow or building your own composite. |
| Working Space | Auto / Linear / sRGB | How to interpret the incoming pixels. Auto treats 32 bpc as linear and 8/16 bpc as sRGB, which matches the usual project setups. |
| Falloff | 1.0 – 3.0 (2.0) | The exponent *n* of the 1/rⁿ the glow follows. 2.0 is the Stiles–Holladay inverse-square law that the CIE disability-glare equations use for real veiling glare. Under Inverse Square it moves light between the core and the halo without changing how much there is: lower is a softer, hazier glow, higher a tighter, hotter one. Under Classic, lower gives Radius more say in the glow's apparent size. |
| Highlight Rolloff | Preserve Hue / Clip / Burn to White | What to do where the lit result leaves the output's range. Clipping each channel on its own reaches the ceiling at a different brightness per channel, so an over-driven saturated colour drifts to white; Preserve Hue rolls the whole triple off together and keeps the colour. Burn to White does on purpose and smoothly what Clip does by accident: the more a colour is overexposed, the whiter it gets, which is the white-hot core of a neon tube — while the glow around it, which the output can show, keeps its colour. Preserve Hue and Clip leave 32 bpc output alone; Burn to White whitens it too but keeps its HDR values. |
| Expand Bounds | on | Let the glow spread past the layer's edges by growing the layer's bounds. |
| Saturation Bias | −100 – 200 % (0 %) | Weights the extraction by how colourful a pixel is. Positive makes a saturated highlight glow harder than a white one of the same brightness; negative does the reverse. |
| Source Opacity | 0 – 100 % (100 %) | Fades the layer the glow sits on without touching the glow. At 0 % only the glow is left, but the source's alpha still shapes it — unlike Glow Only, which drops the source entirely. |
| Unmult | off | For footage delivered on black with no usable alpha. Coverage is read from the brightest channel instead of from an alpha that is 1 everywhere, so dark areas emit nothing and a dim area emits in proportion to its brightness. |
| Multiply Red / Green / Blue | 10 – 400 % (100 %) | Per-channel radius. A lens does not focus every wavelength at the same distance, so its veiling glare is a slightly different size per channel; pulling these apart gives the glow that chromatic fringe. The channel with the larger multiplier spreads wider and therefore peaks lower. |
| Glow Model | Inverse Square / Classic | Inverse Square: the same light in every octave from a fixed one-pixel core to the radius, so Radius is reach and the core stays hot. Classic: the energy-conserving bloom, which scales as a whole with the radius. |
| Core Radius | 0 – 1000 px (20) | Size of the core: the soft rim of light hugging the source, made by the finest octaves. It moves the rim's light to its own size without changing how much there is — smaller is a tight, hard rim, larger a wide, soft one — and works at any Core Intensity. |
| Core Intensity | 0 – 1000 % (100 %) | Strength of the core, apart from the halo that Radius sets. 0 % leaves the source crisp inside a halo; above 100 % the rim runs hotter. At 20 px and 100 % the glow is exactly what it was before the Core group existed, so older projects open unchanged. |
| Core Softness | 0 – 100 % (0 %) | How gently the core fades. It takes light off the hard line along the source's edge and lets the rim trail off into the halo instead of ending on a Gaussian edge — the same light, arriving as a gentler slope. 0 % is the core as it was. |
| Aspect Ratio | 0 – 20 (1.00) | Width over height of the glow. 1 is round; 2 is twice as wide as tall; 0.5 twice as tall as wide; 0 a vertical streak. The longer axis keeps the radius. The glow is stretched along the layer's axes; there is no angle control. |

Radius is in full-resolution pixels: it is scaled automatically for draft
resolutions and for non-square pixels, so a glow stays round and the same size
at Half or Quarter resolution — measured spread across Full / Half / Third /
Quarter is 0.8% in size and 0.1% in brightness. Comparing resolutions in the
viewer needs a fixed zoom, since After Effects upscales a reduced-resolution
render for display.

### Project compatibility

Parameter indices and ids are part of the saved project format. They are frozen
with literal numbers in `AbGlowParams.h` and `AbGlowParams.cpp` and pinned by
`static_assert`, so a new parameter can only be appended. A project saved by any
build from v1.0.0 onward opens correctly in any later build. The effect's match
name is `ABBZ ProfoundGlow`; the earlier `AB Glow` is a separate effect, so old
projects are untouched and both can be installed side by side.

Opening correctly is not the same as looking the same, though. These change
the look of a project saved by an earlier build:

* **Glow Model** is new, so an older project gets its default, Inverse Square.
  Set it to Classic to get the earlier look back; the project's own Radius and
  Falloff are kept either way.
* **8 and 16 bpc glows over transparency are brighter.** The glow is now
  written as light over black: a glow spilling into a transparent layer used to
  show a third of its light at the edge and a tenth of it in the tail. Opaque
  pixels, and everything in a linear working space, come out as before.
* **Anti-aliased edges glow as they should (v1.2.1).** After Effects stores
  pixels straight; every earlier build read and wrote them as premultiplied.
  Edges emitted far too much light — a ragged fringe that crawled as a layer
  moved, blobs on small text, a glow twice as strong at Third resolution — and
  came out stair-stepped with the glow on. Anything with soft or anti-aliased
  edges looks different, and right, now.
* **Near-invisible content no longer glows (v1.2.2).** A layer at a few 1/255
  of opacity - a soft light inside a precomp - passed the threshold on its
  stored colour in v1.2.1 and bloomed into a wide disc of posterised rings.
* **Non-square-pixel compositions glow round (v1.4.0).** The pyramid used to
  render a glow on 2:1 pixels about a fifth too wide; each axis now has its own
  decimation schedule. Square-pixel compositions are unchanged, and Core
  Softness and Aspect Ratio default to the earlier look.

### Anisotropy

Each axis of the diffusion pyramid is decimated on its own schedule: an axis is
halved going into a level only once its own blur there survives the coarser
pixel. A round glow halves both axes every level, exactly as before; a squeezed
axis stays finer for longer, so its resampling cannot smear it wider than asked.
That is what makes Aspect Ratio render the shape it names (2.00 measures 1.97:1
at every quality) and what keeps a glow round on non-square pixels:

| Pixel aspect | measured width ratio (should be 1.0) | before v1.4.0 |
|--------------|--------------------------------------|---------------|
| 1.00 | 1.00 | 1.000 |
| 1.46 | 0.97 | 1.049 |
| 2.00 | 1.02 | 1.188 |

(The residue is the measurement's whole-pixel steps.)

## Testing

```sh
cmake -S . -B build -DABGLOW_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

* `abglow_tests` — unit tests for the plan, filters, transfer functions,
  bit-depth handling, alpha handling, geometry and memory balance.
* `abglow_preview <dir>` — renders a reference scene (white text on black,
  saturated shapes, small bright points, an HDR disc, gradients, a
  semi-transparent block) through every parameter variation and writes PNGs,
  plus a radius calibration table and a banding measurement.
* `abglow_mock_host ProfoundGlow.aex <dir>` — loads the built plug-in, drives
  `PF_Cmd_GLOBAL_SETUP` → `PARAMS_SETUP` → `SMART_PRE_RENDER` → `SMART_RENDER`
  through stub host suites at 8/16/32 bpc, and checks the rectangles, the
  handle balance and repeated apply/remove cycles.

[docs/testing.md](docs/testing.md) has the checklist for testing inside After
Effects.

## Status

Working: everything described above, verified by the unit tests, the mock host,
and address/undefined/thread sanitizer runs of the core.

The plug-in loads and renders in After Effects. Development and the automated
verification happen on Linux, against the SDK headers, a cross-compiled Windows
binary and the mock host, so the checklist in [docs/testing.md](docs/testing.md)
is still the reference for what to confirm in the host itself.

Not implemented yet: GPU rendering (see [docs/gpu.md](docs/gpu.md)), a custom
UI, and Windows-on-Arm builds (which need the `CodeWinARM64` PiPL key from SDK
25.6 or newer — see `WIN_ARM64_FOURCC` in `tools/generate_pipl.py`). Windows on
Arm only matters on Arm hardware running the native After Effects build; on a
normal x64 machine the x64 binary is the right one.
