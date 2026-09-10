# Third-party dependencies

## Adobe After Effects SDK

The plug-in builds against the Adobe After Effects SDK, which is **not** part of
this repository: Adobe's licence does not allow redistributing it.

Get it from the Adobe Developer Console (free, requires an Adobe ID):
<https://developer.adobe.com/after-effects/> → *Download the SDK*.

Then either

* unpack it and point the build at it:

  ```
  cmake -S . -B build -DAE_SDK_ROOT="C:/AfterEffectsSDK/Examples"
  ```

* set `AESDK_ROOT` in the environment (the variable Adobe's own samples use), or
* copy the SDK's `Examples` folder to `third_party/AfterEffectsSDK`, which is
  where CMake looks by default. That folder is git-ignored.

Only these parts of the SDK are needed:

```
Headers/            all headers, including Headers/SP and Headers/adobesdk
Util/entry.h
Util/Param_Utils.h
```

The build was developed against the SDK that reports
`PF_PLUG_IN_VERSION 13`, `PF_PLUG_IN_SUBVERS 28` (After Effects 23.4 era) and
works with newer SDKs; `tools/generate_pipl.py` reads the spec version out of
`Headers/AE_EffectVers.h`, so the plug-in always advertises the SDK it was
compiled with.
