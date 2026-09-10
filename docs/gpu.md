# GPU rendering

AB Glow renders on the CPU today. This is what the current SDK offers and how
the code is arranged so a GPU path can be added without disturbing the rest.

## What After Effects provides

GPU effects arrived in After Effects 16.0 (CC 2019) and the shape of the API
has not changed since:

* `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` during `PF_Cmd_GLOBAL_SETUP` declares
  the capability. GPU rendering is **32-bit float only**; 8 and 16 bpc renders
  always take the CPU path.
* `PF_Cmd_GPU_DEVICE_SETUP` / `PF_Cmd_GPU_DEVICE_SETDOWN` create and release
  per-device state. `PF_GPUDeviceInfo` carries the native device and context
  handles.
* `PF_Cmd_SMART_PRE_RENDER` must set `PF_RenderOutputFlag_GPU_RENDER_POSSIBLE`
  for the frames the GPU can handle; without it After Effects stays on the CPU.
* `PF_Cmd_SMART_RENDER_GPU` then renders the frame, with
  `PF_SmartRenderInput::what_gpu` naming the framework and `device_index`
  identifying the device for the Premiere GPU device suite.
* Frameworks: CUDA, OpenCL and DirectX on Windows, Metal on macOS. DirectX
  additionally needs `PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING` and ships its
  compiled shaders in a `DirectXAssets` folder next to the binary.

Build dependencies for the GPU path, from Adobe's own instructions: Boost (used
by the SDK sample's kernel build rules), the CUDA SDK matching the After Effects
build (AE 25.4 uses CUDA 12.8), and the DirectX shader compiler. Adobe
recommends the CUDA *driver* API, or statically linking `cudart_static.lib`, for
forward compatibility with future drivers.

## Why it is not implemented here

The GPU path is a second, independent implementation of the whole pipeline —
kernels per framework, per-device resource management, and a fallback that has
to match the CPU output closely enough that switching does not shift the image.
That is a larger piece of work than the CPU effect itself, and it needs real
hardware and After Effects to validate. The CPU implementation is fast enough
to be useful (40–110 ms for a 4K frame on four cores), so it ships first.

## How the code is arranged for it

* All the image maths lives in `src/core` behind plain data descriptions. The
  algorithm — extraction, octave weights, tap counts, colour transform,
  composite — is expressed in `GlowPlan` and `GlowSettings` and is deliberately
  free of CPU-specific structure, so a kernel can be written against the same
  plan and produce the same result.
* `AbGlowRender.cpp` already separates *deciding what to render*
  (`SmartPreRender`, which computes rectangles and stores a `PreRenderData`)
  from *rendering it* (`RunPipeline`). A GPU path adds a `SmartRenderGPU` beside
  `SmartRender` and reuses the same pre-render data.
* Adding it means: set the two out-flags in `tools/generate_pipl.py` (the
  generated header keeps `GlobalSetup` in sync automatically), handle
  `PF_Cmd_GPU_DEVICE_SETUP`/`SETDOWN` and `PF_Cmd_SMART_RENDER_GPU` in
  `EffectMain`, set `PF_RenderOutputFlag_GPU_RENDER_POSSIBLE` in pre-render, and
  implement the pyramid as a kernel chain. The CPU path stays as the fallback
  the host uses whenever the flag is absent.

## Sources

* [Building GPU Effects](https://ae-plugins.docsforadobe.dev/intro/gpu-build-instructions/)
* [SmartFX](https://ae-plugins.docsforadobe.dev/smartfx/smartfx/)
* `Wunkolo/Vulkanator` — an out-of-tree example of driving a non-Adobe GPU API
  (Vulkan) from a SmartFX plug-in by copying through host buffers
