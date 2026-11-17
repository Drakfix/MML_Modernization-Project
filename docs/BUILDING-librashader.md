# Building librashader

The optional Shader setting (RetroArch `.slangp` presets: CRT, scanlines, NTSC and so on)
runs through [librashader](https://github.com/SnowflakePowered/librashader), loaded at
runtime by mml.dll, the 64-bit runner and the Android app. librashader only publishes
64-bit desktop builds, and the game is 32-bit, so we build every copy ourselves from the
unmodified tagged source.

License: MPL-2.0 (`licenses/librashader-LICENSE.txt`). The C header in
`native/vendor/librashader/` is MIT.

## What is built

| Build | Target | Runtimes | Goes to |
|---|---|---|---|
| mml.dll (32-bit) | `i686-pc-windows-msvc` | Direct3D 11, Vulkan | `librashader.dll` in the game folder |
| mml_run64.exe | `x86_64-pc-windows-msvc` | Direct3D 11, Vulkan | `x64\librashader.dll` in the game folder |
| Android app | `aarch64-linux-android` | Vulkan | `lib/arm64-v8a/librashader.so` in the APK |

## Steps

1. Install Rust with [rustup](https://rustup.rs) (stable, minimal profile is enough) and
   add the targets:

   ```
   rustup target add i686-pc-windows-msvc aarch64-linux-android
   ```

   The MSVC Build Tools used for `native\build.bat` provide the Windows linker. The
   Android build uses the NDK that `android\build_android.py` uses.

2. Get the source at the tag the runtime was built against:

   ```
   git clone --depth 1 --branch librashader-v0.12.1 https://github.com/SnowflakePowered/librashader.git
   ```

3. Windows, from the librashader folder (32-bit, then 64-bit):

   ```
   cargo run -p librashader-build-script -- --profile optimized --target i686-pc-windows-msvc -- --no-default-features --features runtime-d3d11,runtime-vulkan
   cargo run -p librashader-build-script -- --profile optimized --target x86_64-pc-windows-msvc -- --no-default-features --features runtime-d3d11,runtime-vulkan
   ```

   The results are `target\<target>\optimized\librashader.dll` (about 7 and 8 MB). Copy
   the 32-bit one next to `mml.dll` and the 64-bit one into `x64\`.
   `tools\make_patcher.py` packages both from there.

4. Android, from the librashader folder, with the NDK's compilers named for Cargo and the
   C++ runtime linked in statically (so the app needs no `libc++_shared.so`). Rust links
   with `-nodefaultlibs`, so `libc++abi` has to be named explicitly, and `--no-undefined`
   makes a missing symbol fail the build instead of the app's `dlopen`.
   `NDK` is the NDK's `toolchains\llvm\prebuilt\windows-x86_64\bin` folder:

   ```
   set CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=%NDK%\aarch64-linux-android28-clang.cmd
   set CC_aarch64_linux_android=%NDK%\aarch64-linux-android28-clang.cmd
   set CXX_aarch64_linux_android=%NDK%\aarch64-linux-android28-clang++.cmd
   set AR_aarch64_linux_android=%NDK%\llvm-ar.exe
   set CXXSTDLIB_aarch64_linux_android=c++_static
   cargo rustc -p librashader-capi --lib --crate-type cdylib --profile optimized --target aarch64-linux-android --no-default-features --features runtime-vulkan -- -C link-arg=-lc++abi -C link-arg=-Wl,--no-undefined
   ```

   The result is `target\aarch64-linux-android\optimized\liblibrashader_capi.so`.
   `android\build_android.py` finds it there (or at `MML_LIBRASHADER_ANDROID`), strips it
   and packages it as `librashader.so`.

If the header changes on a librashader update, copy the new `include/librashader.h` into
`native/vendor/librashader/` too: the runtime refuses a library whose ABI version doesn't
match.

## Notes

- The game's main thread has a 128 KB stack, too small for librashader's shader
  compilers, so `rshader.c` builds the filter chain on its own thread with a 16 MB stack.
- Compiled shaders are cached by librashader, so a preset's second start is faster (the
  first start of a preset can also stutter briefly while the driver compiles). On Android
  the app has no home folder, so librashader keeps its cache in the current folder, the
  app's files folder (`librashader\librashader.db.1`).
- Vulkan uses render-pass mode (no dynamic rendering), so it needs nothing beyond the
  backend's Vulkan 1.1 device.
- Presets aren't shipped (most are GPL). Users point the launcher at their own copy of
  libretro's [slang-shaders](https://github.com/libretro/slang-shaders), or put them in
  `Modernization\shaders`.
