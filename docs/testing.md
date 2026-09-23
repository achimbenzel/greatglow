# Testing

## Automated

```sh
cmake -S . -B build -DABGLOW_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`core` runs the unit tests, `plugin` runs the mock host against the built
`.aex` (Windows builds only).

### Sanitizers

The core has no host dependency, so it can be checked directly:

```sh
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I src -I tests \
    -o /tmp/ct tests/CoreTests.cpp tests/TestSupport.cpp src/core/*.cpp -pthread && /tmp/ct
g++ -std=c++17 -O1 -g -fsanitize=thread -I src -I tests \
    -o /tmp/ct tests/CoreTests.cpp tests/TestSupport.cpp src/core/*.cpp -pthread && /tmp/ct
```

The address sanitizer is what caught the one real memory bug during
development, a source read that used the destination's column index when the
buffers were offset.

### Looking at the output

```sh
build/abglow_preview /tmp/glow
```

writes `scene_*.png` for each parameter variation, `crop_*.png` for banding
inspection, a radius calibration table and the dither measurement. The mock
host writes `mockhost_*.png` for each bit depth and geometry case.

## In After Effects

The automated tests cover the pixels and the call sequence; these are the
things only the real host can tell you.

1. Build Release, install, restart After Effects.
2. **It loads.** `Effect ▸ AB Tools ▸ AB Glow` exists and applies without a
   warning triangle in the Effect Controls panel (the triangle would mean
   After Effects does not consider the effect thread safe).
3. **Parameters.** Every control changes the render, and the groups expand and
   collapse. Scrub each one to its extremes.
4. **Bit depths.** Switch the project between 8, 16 and 32 bpc
   (*File ▸ Project Settings ▸ Colour ▸ Depth*) and confirm the result matches
   and that nothing clips in 32 bpc.
5. **HDR.** With a 32 bpc project, apply to a layer with values above 1.0
   (an EXR, or *Exposure* pushed up). The glow should grow with the overbright
   values, not clip.
6. **Radii.** Radius 0, 1, 40, 400, 4000. Large radii should stay smooth and
   should not become dramatically slower.
7. **Transparency.** Apply to text and to a shape layer with holes. Transparent
   areas must not generate light, and the glow should spread into them.
8. **Bounds.** With *Expand Bounds* on, the glow spills past the layer's edges;
   with it off, it stops at them.
9. **Resolutions.** Full / Half / Quarter, and a region of interest. The glow
   must stay the same size in comp space at every resolution. Compare at a fixed
   viewer zoom, or render the frames to disk — After Effects upscales a
   reduced-resolution render for display, so a different zoom per screenshot
   makes any effect look different.
10. **Non-square pixels.** A comp with a 1.46 pixel aspect: the glow must stay
    round.
11. **Animation.** Keyframe Radius, Threshold and Intensity; render the range
    and check for popping between frames, and that the glow does not drift
    sideways as the radius grows.
12. **Motion blur and 3D.** Enable motion blur on a moving layer and confirm the
    glow is blurred with it (After Effects renders the layer per sample; the
    effect needs no special handling).
13. **Multi-frame rendering.** Render a range from the render queue and confirm
    several frames render concurrently without artefacts.
14. **Stability.** Apply and remove the effect 20 times, undo/redo, duplicate
    the layer, save and reopen the project. Watch memory in Task Manager for
    growth that does not come back.
## When a report cannot be reproduced

The mock host is written from the same assumptions as the plug-in, so it cannot
catch a misunderstanding of what After Effects actually asks for. Build with
diagnostics to record that from inside a real session:

```bat
cmake -S . -B build-diag -G "Visual Studio 17 2022" -A x64 -DABGLOW_DIAGNOSTICS=ON
cmake --build build-diag --config Release
```

Every pre-render and render then appends to `%TEMP%\AbGlow-diag.log`: the layer
size, downsample factor and bit depth; the rectangle the host requested; the
rectangle the effect declared; the rectangle the host granted for the input;
the size, origin and row pitch of both worlds; and the plan the pipeline chose.
The file is rewritten on each launch and capped at 20000 lines. Diagnostics are
off in a normal build and cost nothing there.

## Manual checks

15. **The right build.** The parameter list ends with a group named after the
    build (`v1.2.2 (abc1234)`). Check it matches what was just installed before
    trusting any of the above — After Effects scans both the application's
    `Plug-ins` folder and `Common\Plug-ins\7.0\MediaCore`, and a stale copy in
    the other one will be loaded instead.
16. **Colour.** Compare *Working Space: Auto* against *Linear* and *sRGB* on the
    same footage; Auto should match Linear in a 32 bpc linearised project and
    sRGB in an 8 bpc one.
17. **Glow Model.** On white text over a transparent background in an 8 bpc
    comp, Inverse Square at radius 400 should read like Deep Glow's default
    look: a bright line hugging the letters and a haze that carries a couple of
    hundred pixels. Keyframe Radius from 100 to 600 and scrub it: the line next
    to the letters must not jump at any frame. Classic should look like
    v1.1.0, apart from being brighter over transparency.
18. **Edges and motion.** Anti-aliased text at 100% intensity: the letters'
    edges stay smooth, with the effect on and off. Animate the text's position
    slowly: the glow moves with it and does not shimmer or crawl. Switch the
    viewer between Full and Third at a fixed zoom: the glow is the same
    strength.
19. **Burn to White.** On saturated text with Intensity pushed up, the letters
    burn towards white while the glow around them keeps its colour, in 8 bpc
    and in 32 bpc.
