# AB Glow

A native Adobe After Effects effect plug-in (`.aex`) that renders a
high-dynamic-range glow using multi-scale light diffusion, written in C++17.

The glow is generated in linear light from a soft-knee highlight extraction and
diffused through a weighted Gaussian pyramid, which gives a bright concentrated
core with a long, smooth falloff — the look of light spilling, rather than a
blurred copy of the layer composited with Add.

```
source ─► highlight extraction (soft knee, linear light)
       ─► downsample to pyramid level 0
       ─► blur / downsample cascade (one Gaussian per octave)
       ─► weighted collapse back up (log-normal octave envelope)
       ─► exposure · intensity · saturation · tint
       ─► composite over the original, expanding the layer bounds
```

* 8, 16 and 32 bits per channel, with all internal maths in 32-bit float
* HDR safe: values above 1.0 are never clamped in 32 bpc
* SmartFX, multi-frame rendering, host memory and the host thread pool
* Cost stays nearly flat as the radius grows (radius 400 is not slower than
  radius 20 at 4K)

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

The plug-in lands at `build/Release/AbGlow.aex`.

To build again after changing sources, re-run the second command. Re-running
CMake is only needed when files are added or `tools/generate_pipl.py` changes.

### Installing

```bat
cmake --install build --config Release
```

installs to `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\AB Tools`,
the shared folder that every recent After Effects version scans (an
Administrator prompt is expected). Override it with
`-DABGLOW_INSTALL_DIR="D:/My Plug-ins"`, or simply copy `AbGlow.aex` into

```
C:\Program Files\Adobe\Adobe After Effects <version>\Support Files\Plug-ins\
```

Restart After Effects; the effect appears under **Effect ▸ AB Tools ▸ AB Glow**.

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
| Radius | 0 – 4000 px (40) | Size of the glow. The value is roughly the distance, in full-resolution pixels, at which the glow has faded out. |
| Intensity | 0 – 10000 % (100 %) | Strength of the generated light. 0 turns the effect into a pass-through. |
| Exposure | −10 – +10 stops (0) | Brightness of the glow in stops; +1 doubles it. Useful for large radii, where the same light is spread over more area. |
| Saturation | 0 – 400 % (100 %) | Colour of the glow: 0 is white light, 100 % keeps the source colour, above that exaggerates it. |
| Tint | colour (white) | Colour multiplied into the glow. |
| Tint Amount | 0 – 100 % (0 %) | How much of the tint is mixed in. |
| Quality | Draft / Normal / High / Best | Trades resolution of the diffusion pyramid and upsample filter against speed. |
| Composite | Add / Screen / Glow Only | How the glow is combined with the source. Glow Only is useful for inspecting the glow or building your own composite. |
| Working Space | Auto / Linear / sRGB | How to interpret the incoming pixels. Auto treats 32 bpc as linear and 8/16 bpc as sRGB, which matches the usual project setups. |
| Expand Bounds | on | Let the glow spread past the layer's edges by growing the layer's bounds. |

Radius is in full-resolution pixels: it is scaled automatically for draft
resolutions and for non-square pixels, so a glow stays round and the same size
at Half or Quarter resolution.

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
* `abglow_mock_host AbGlow.aex <dir>` — loads the built plug-in, drives
  `PF_Cmd_GLOBAL_SETUP` → `PARAMS_SETUP` → `SMART_PRE_RENDER` → `SMART_RENDER`
  through stub host suites at 8/16/32 bpc, and checks the rectangles, the
  handle balance and repeated apply/remove cycles.

[docs/testing.md](docs/testing.md) has the checklist for testing inside After
Effects.

## Status

Working: everything described above, verified by the unit tests, the mock host,
and address/undefined/thread sanitizer runs of the core.

Not implemented yet: GPU rendering (see [docs/gpu.md](docs/gpu.md)), a custom
UI, and Windows-on-Arm builds (which need the `CodeWinARM64` PiPL key from SDK
25.6 or newer — see `WIN_ARM64_FOURCC` in `tools/generate_pipl.py`).

The plug-in has not yet been run inside After Effects itself; it was developed
and verified on Linux against the SDK headers, a cross-compiled Windows binary
and a mock host.
