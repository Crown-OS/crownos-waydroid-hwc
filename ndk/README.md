# Out-of-tree build of `hwcomposer.waydroid.so`

This directory builds the Waydroid hwcomposer HAL in `../hwcomposer` without a
LineageOS tree. It uses the Android NDK compiler, the libraries in the pinned
Waydroid `vendor.img` and `system.img`, and only the headers and host tools that
the build needs from Android 13. The result ships as a Waydroid overlay file:
`/var/lib/waydroid/overlay/vendor/lib64/hw/hwcomposer.waydroid.so`.

## Quick start

```sh
ndk/build.sh x86_64        # about 2 minutes on a cold cache, 6 s to rebuild
```

The output is `~/.cache/crownos-android/hwc/out/x86_64/vendor/lib64/hw/hwcomposer.waydroid.so`.
Two builds from different cache directories are bit-identical.

Requirements: NDK 27.2.12479018, SDK build-tools 34.0.0 (for `aidl`), cmake ≥ 3.27,
ninja, clang, bison, flex, OpenSSL, jsoncpp, expat, debugfs and python3. Environment
overrides: `ANDROID_NDK`, `AIDL`, `HWC_CACHE`, `WAYDROID_IMAGES`, `JOBS` (default 4),
`HOST_CC` and `HOST_CXX`.

## What `build.sh` does

| Step | Script | Output in `$HWC_CACHE` |
|---|---|---|
| Fetch android-13.0.0_r75 / LineageOS 20 sources and apply the Waydroid patches that change headers or generated code | `fetch-sources.sh` | `aosp/` (69 MB) |
| Extract the HWC's `DT_NEEDED` closure from the images, in vendor linker-namespace order | `make-sysroot.sh` | `sysroot/<abi>/lib64` (55 libraries, 8 MB) |
| Build the host tools `hidl-gen` (with the Waydroid patch) and `wayland-scanner` (AOSP wayland 1.19) | `host/CMakeLists.txt` | `build/host/` |
| Generate the HIDL headers (`vendor.waydroid.*`, `android.hidl.*`, graphics) and the AIDL NDK headers that libui exports | `generate-interfaces.sh` | `build/<abi>/gen-interfaces/` |
| Cross-compile libffi, libwayland-client, libxkbcommon, the protocol code, the HWC and, for checking only, the `vendor.waydroid.*` HIDL libraries | `CMakeLists.txt`, `cmake/*.cmake` | `build/<abi>/`, `out/<abi>/` |
| Resolve every import against the sysroot | `check-symbols.sh` | `*.imports.tsv` next to the .so |
| Compare the rebuilt HIDL libraries with `vendor.img`: strong exports and every vtable slot | `check-hidl-abi.sh` | |
| Check the `cros_gralloc_handle` layout against the image's minigbm and the stock HWC (x86_64 only) | `check-gralloc-handle.sh` | `build/gralloc-probe/` |

## ABI rules this build follows

- **C++ library.** C++ is compiled against the platform libc++ headers (LineageOS 20
  `external/libcxx`, `std::__1`) with `-nostdinc++`, and linked against the image's
  `libc++.so`. The NDK's `std::__ndk1` is never used, and the checker fails the
  build if the symbol `__ndk1` appears.
- **C library and kernel headers.** These come from Android 13 bionic (`-nostdlibinc`).
  NDK r27's `<ctype.h>` defines the `is*_l` functions inline, which clashes with
  this libc++.
- **Vendor-variant flags.** Like Soong for a vendor module, the build passes
  `-D__ANDROID_VNDK__ -D__ANDROID_VENDOR__ -fno-exceptions -fno-rtti
  -fvisibility-inlines-hidden`, `-z now`, `--gc-sections` and Android packed
  relocations.
- **Include paths.** Each library's headers come from the directories its vendor
  variant exports: `liblog/include_vndk`, `libcutils/include_outside_system`,
  libui's export list, and so on.
- **Link order.** Shared libraries are linked in the order `Android.bp` lists them,
  so `DT_NEEDED` matches the stock module exactly.

## Differences from the stock module

These were measured against the stock module and against a build of commit
`ce61fbc`, the last HWC change before the vendor image was built:

- **Unversioned EGL, GLES and libsync imports.** `egl*`, `gl*`, `sync_wait` and
  `sw_sync_*` are imported without a version tag. Soong links against LLNDK stubs
  that carry `LIBEGL`, `LIBGLESV2` and `LIBSYNC`. The real libraries in the image
  define no versions, so bionic resolves both forms the same way.
- **Extra `__register_atfork@LIBC` import.** It comes from the NDK's
  `crtbegin_so.o`, and libc has exported it since API 23.
- **A few different weak `std::__1` template exports.** Clang 18 makes different
  inlining choices than the platform's clang 14.
- **No `configureRpcThreadpool` import at HEAD.** This is a source change: upstream
  commit `4aee4c8` dropped the call.

## arm64

`build.sh arm64` selects `arm64-v8a`, the aarch64 libffi sources and the arm64
kernel headers. Every compile step has been tried for arm64. Linking needs arm64
images: set `WAYDROID_IMAGES` to a directory that holds them.
`make-sysroot.sh` refuses images whose ELF machine does not match the ABI.

## Installing

Use `crownos-android/spikes/s2-hwc-build/root-steps.sh install`. It stops if the
container is running, saves the stock module and any earlier overlay file, and
installs the new module with mode 0644, owned by root.
