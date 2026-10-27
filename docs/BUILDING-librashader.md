# Building librashader.dll

The optional Shader setting (RetroArch `.slangp` presets: CRT, scanlines, NTSC and so on)
runs through [librashader](https://github.com/SnowflakePowered/librashader), loaded by
mml.dll at runtime. librashader only publishes 64-bit Windows builds, and the game is
32-bit, so we build it ourselves from the unmodified tagged source.

License: MPL-2.0 (`licenses/librashader-LICENSE.txt`). The C header in
`native/vendor/librashader/` is MIT.

## Steps

1. Install Rust with [rustup](https://rustup.rs) (stable, minimal profile is enough) and
   add the 32-bit target:

   ```
   rustup target add i686-pc-windows-msvc
   ```

   The MSVC Build Tools used for `native\build.bat` provide the linker.

2. Get the source at the tag mml.dll was built against:

   ```
   git clone --depth 1 --branch librashader-v0.12.1 https://github.com/SnowflakePowered/librashader.git
   ```

3. Build only the Direct3D 11 runtime, from the librashader folder:

   ```
   cargo run -p librashader-build-script -- --profile optimized --target i686-pc-windows-msvc -- --no-default-features --features runtime-d3d11
   ```

   The result is `target\i686-pc-windows-msvc\optimized\librashader.dll` (about 6 MB).

4. Copy it into the game folder next to `mml.dll`. `tools\make_patcher.py` packages it
   from there.

If the header changes on a librashader update, copy the new `include/librashader.h` into
`native/vendor/librashader/` too: mml.dll refuses a DLL whose ABI version doesn't match.

## Notes

- The game's main thread has a 128 KB stack, too small for librashader's shader
  compilers, so `rshader.c` builds the filter chain on its own thread with a 16 MB stack.
- Compiled shaders are cached by librashader, so a preset's second start is faster.
- Presets aren't shipped (most are GPL). Users point the launcher at their own copy of
  libretro's [slang-shaders](https://github.com/libretro/slang-shaders), or put them in
  `Modernization\shaders`.
