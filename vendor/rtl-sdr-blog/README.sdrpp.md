# RTL-SDR Blog driver used by SDR++

Source: https://github.com/rtlsdrblog/rtl-sdr-blog
Pinned release: V1.4.0
Commit: aed0ea19f3a273370a13c9009b96313c75d54c7b
License: GPL-2.0-or-later; see COPYING and individual source headers.

The RTL source and macOS ADS-B module link this driver statically on every
platform where those modules are enabled. System librtlsdr and prebuilt SDK
RTL libraries are not used. This includes Blog V4L/R828S detection and retains
the upstream supported older RTL-SDR tuner implementations.

Local changes: a library-only CMake build, and Android rtlsdr_open_sys_dev
support using libusb_wrap_sys_device. Android retains ownership of the USB
file descriptor. Both open paths share the upstream tuner initialization.
No tuner or frequency calculations are changed.

libusb remains a shared platform dependency. Existing packaging must include
it. RTL-TCP clients use the server's driver, so remote servers need a separate
driver update. iOS builds that do not enable a local RTL source are unaffected.

Validate the built RTL source module contains `Blog V4L` and does not import
an external librtlsdr/rtlsdr DLL. A successful build is not an RF tuning test.
