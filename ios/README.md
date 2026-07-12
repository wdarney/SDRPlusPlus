# SDR++ iOS (client-only)

The iOS / iPadOS client is maintained alongside the desktop applications on
the unified `integration/main` branch. Platform-specific CMake entry points
and source files isolate the static iOS build without deleting or replacing
the GLFW, Windows, Linux, USB hardware, plugin, or packaging code used by the
other platforms.

The app is **client only**: it connects to remote SDR servers (sdrpp_server,
SpyServer, rtl_tcp, raw network IQ, Spectran HTTP) and plays back local WAV
files. Local USB SDR hardware is not supported (iOS sandbox forbids the
libusb access path).

## What's here

| Path | Notes |
| --- | --- |
| [core/backends/ios/](../core/backends/ios) | Metal + UIKit backend |
| [sink_modules/coreaudio_sink/](../sink_modules/coreaudio_sink) | AudioUnit-based sink (replaces the desktop RtAudio sink) |
| [ios/AppShell/](AppShell) | Obj-C++ UIApplication shell, MTKView host, touch routing |
| [ios/CMakeLists.txt](CMakeLists.txt) | Xcode iOS app target wiring |
| [ios/build.sh](build.sh) | Convenience configure script |

## Status — what still needs doing

1. **Cross-compiled deps.** Build `fftw3f`, `libzstd`, and (optionally) `volk`
   for `iphoneos` and `iphonesimulator`. Set `SDRPP_IOS_DEPS_ROOT` to a dir
   containing `include/` and `lib/`. VOLK is the awkward one — its generic
   kernels are the safe path on iOS, or swap the FFT/dot-product calls to
   Apple's Accelerate framework.
2. **Touch UX.** The current backend forwards a single touch as the left
   mouse button. A real iOS port wants pinch-to-zoom on the waterfall and
   long-press → right-click for context menus.
3. **Soft keyboard.** `iosWantsKeyboard()` is plumbed but the host shell
   doesn't yet present a UITextField to capture text input.
4. **App lifecycle.** `applicationWillResignActive` should call
   `gui::mainWindow.setPlayState(false)` so streaming stops when backgrounded.

## Build

```sh
export SDRPP_IOS_DEPS_ROOT=/abs/path/to/ios-deps    # see status #2
./ios/build.sh device    # or `sim` for the simulator
```

That generates an Xcode project under `build-ios-device/`. Open it, select
the `SDRPP_iOS` scheme, set your signing team, and run.

If codesigning fails because the checkout under `Documents` has macOS File
Provider extended attributes, configure and build in `/private/tmp` instead.

## How the static-link refactor works

iOS forbids dynamic loading, so SDR++'s usual `dlopen` plugin model is
replaced with link-time module registration:

- Every module is built as a CMake `OBJECT` library and linked into the main
  binary.
- Each module's `_INIT_` / `_INFO_` / etc. extern "C" symbols are uniquified
  via `-DSDRPP_MODULE_TOKEN=<name>_` (see `core/src/module.h`).
- The static-modules registry is generated at configure time from the
  `SDRPP_STATIC_MODULE_TARGETS` global property
  (`core/src/static_modules.cpp.in` -> `static_modules.cpp`).
- File-scope globals in module sources (e.g. `ConfigManager config;`) carry
  the `static` keyword so they don't collide across modules. This is the only
  per-module source change the fork required.

## Adding a module from another task branch

When a module is finished in a separate task branch:

1. Merge or cherry-pick the reviewed module commit into a clean branch based
   on `origin/integration/main`.
2. If the module declares any file-scope external globals, mark them `static`
   (the link error is loud and points to the offending symbol).
3. Add the module subdir to the top-level [CMakeLists.txt](../CMakeLists.txt)
   (look for the `add_subdirectory("source_modules/...")` block).
4. Configure + build.
