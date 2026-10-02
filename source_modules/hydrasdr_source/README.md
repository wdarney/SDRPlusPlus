# HydraSDR integration and build handoff

This module is imported from https://github.com/hydrasdr/SDRPlusPlus at
`bc209f3720a71e6cbba2333c3dba224fbc3d61cf` (module version 0.1.1), on top of
this fork's `integration/main` baseline `cc5e9755`. Vendor attribution is
retained. Local changes initialize the device pointer and initial frequency.

The driver used by the vendor build and this integration is
https://github.com/hydrasdr/hydrasdr-host tag `v1.1.3`, commit
`16942cbcbde47198abc6b7968c700ed0cbb8cc87`. The module requires the 1.1 API;
the SDK pkg-config version is `1.1` even though the library is 1.1.3.
The desktop CI and Linux build scripts now fetch this driver tag.

## Building current and future desktop apps

Build from the updated `integration/main`, or merge that branch into an
existing app's source branch while preserving its app-specific changes.
Do not merge the entire vendor fork or replace this fork's core and modules.

`OPT_BUILD_HYDRASDR_SOURCE` now defaults ON. An existing CMake cache can retain
OFF, so explicitly pass `-DOPT_BUILD_HYDRASDR_SOURCE=ON` when refreshing an old
build. Install/build the pinned SDK for the target architecture first. For a
private SDK prefix, expose its `lib/pkgconfig` through `PKG_CONFIG_PATH`.
Windows builds use the SDK in `C:/Program Files/hydrasdr-host`.

Build the core, module, and each app's other modules from the same revision.
Package `hydrasdr_source` together with the matching HydraSDR driver and USB
dependencies. The macOS bundler discovers the enabled module and follows its
linked dependencies. Do not copy an arbitrary plugin into apps built against
different core versions. Re-sign and audit each final macOS bundle.

The module ID and config file remain `hydrasdr_source` and
`hydrasdr_config.json`. Existing profiles may need the Module Manager entry
`HydraSDR Source` -> `hydrasdr_source` enabled. The vendor source dropdown name
now includes the driver version (`HydraSDR lib v1.1.3`), so reselect it if the
profile previously selected `HydraSDR` or the older `hydrasdr_rfone_source`.
Do not load both old and new Hydra modules for the same radio.

Android's existing build explicitly disables HydraSDR and is unchanged.
Enabling it requires a matching new SDK/USB integration for every Android ABI;
macOS artifacts cannot be reused on Android, Windows, or iOS. No new native
iOS USB support is claimed by this desktop module import.

## Validation and remaining work

Before the user assigned app builds to a separate session, the imported module,
shared core, and driver 1.1.3 compiled on macOS arm64. An isolated minimal app
loaded the HydraSDR module and logged driver 1.1.3. The local app dependencies
require macOS 26; this is not evidence of older-macOS compatibility. No installed
user app was replaced.

The reported HydraSDR crash is not diagnosed or proven fixed. RF reception,
start/stop, retuning, disconnect/reconnect, long runs, and other platform builds
remain for the build/testing session. This import still contains the vendor's
stream callback and error handling; successful compilation is not proof that
those paths are crash-free.
