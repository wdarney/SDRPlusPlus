# RX888 Android ARM64 FFTW

The RX888 Android R2IQ path uses the repository copy of
`libfftw3f.so` for `arm64-v8a`. This prevents Android builds from silently
falling back to the scalar `NO_SIMD` FFTW library in `/sdr-kit`.

The checked-in library is FFTW 3.3.10 built for Android API 28 with NDK
25.1.8937393 and these relevant options:

```text
--enable-single --enable-shared --disable-static --disable-fortran
--enable-neon --enable-threads --with-combined-threads
CFLAGS=-O3 -DNDEBUG -ffast-math -fomit-frame-pointer
```

Its expected SHA-256 is:

```text
2090834c7a41f71451de700e23176b1a91a3cc500819e7726d2ac0161cbf055d
```

Normal Android builds do not need to rebuild FFTW. CMake uses the ABI-compatible
SDR kit library while linking, then Gradle overlays the optimized library into
the APK and verifies the finished package. A missing, changed, or scalar ARM64
library fails the build.

To reproduce the prebuilt library, set `ANDROID_SDK_ROOT` or
`ANDROID_NDK_HOME`, then run:

```bash
./android/build_rx888_fftw_arm64.sh
```

FFTW is distributed under the GNU General Public License. Source and license
details are available from <https://fftw.org/>.
