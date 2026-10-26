# mml.dll (native runtime)

This is the Frida-free version of the mod runtime. `MEGAMAN.EXE` gets one extra
import (`mml.dll`), so Windows loads the DLL before the game starts and it
patches the game in place. No Python, no Frida, no launcher process, and it
should run under Wine/Proton too.

Ported so far: widescreen projection, the GetTickCount/frame-limiter timing fix,
intro skip (keyboard, gamepad or bypass) and all the modern controls with the
orbit indicator. Not ported yet: the HUD un-stretch. That one is getting
replaced by a proper renderer backend instead of being copied over.

## Build

Needs Visual Studio Build Tools with the C++ workload (x86 tools).

```
native\build.bat
```

Output is `native\build\mml.dll` (32-bit, static runtime, no redistributables).

## Try it

1. `python tools\patch_add_import.py MEGAMAN.EXE MEGAMAN_NATIVE.EXE` in the game
   folder (works on the already-patched exe; your original stays untouched).
2. Copy `mml.dll` next to it and run `MEGAMAN_NATIVE.EXE`.
3. Settings live in `mml.ini` next to the exe:

```
[video]
aspect=16:9          ; or 4:3
closer_factor=0.859375

[controls]
mode=modern          ; or vanilla

[general]
intro=play           ; or skip
```

What it did gets written to `mml.log`.

## Layout

- `src/mml.c` entry point, config, widescreen, timing, intro bypass
- `src/controls.c` orbit camera, camera-relative movement, aim, dodge, button remap
- `src/toast.c` the ORBIT ON/OFF indicator, drawn into the game's own HUD list
- `src/hook.c` mid-function hooks (jump to a stub that saves registers, calls C, replays the stolen bytes)
- `src/iat.c` import table patching (used to catch Xidi's XInput lookup)
- `src/game.h` every game address we touch, named like `tools/ghidra/symbols.csv`
