# mml.dll (native runtime)

This is the Frida-free version of the mod runtime. `MEGAMAN.EXE` gets one extra
import (`mml.dll`), so Windows loads the DLL before the game starts and it
patches the game in place. No Python, no Frida, no launcher process, and it
should run under Wine/Proton too.

Ported so far: widescreen, the GetTickCount/frame-limiter timing fix, intro
skip (keyboard, gamepad or bypass) and all the modern controls with the orbit
indicator.

Widescreen has two modes:

- `native` renders on a real wide surface (854x480 on 16:9). Every draw goes
  through one Direct3D call, so each primitive gets placed on its way to the
  GPU: the 3D world just gets more width, the HUD gauges go to the screen edges,
  backgrounds made for 4:3 get stretched, menus and dialogue stay centered at
  their real proportions. Nothing gets squeezed. Needs DDrawCompat to allow the
  mode (`SupportedResolutions` has to list it).
- `anamorphic` is the old way: render 4:3 with a narrower projection and let
  DDrawCompat stretch it. The HUD stays stretched in this mode.

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
mode=native          ; or anamorphic
hud_edges=1          ; native: push the life/weapon gauges to the screen edges
closer_factor=0.859375

[controls]
mode=modern          ; or vanilla

[general]
intro=play           ; or skip
```

What it did gets written to `mml.log`.

Dev settings (`[debug]`): `remote_pad=1` lets tools press pad buttons through
shared memory (`tools\dev\drive.ps1` uses it to play the game for screenshot
tests), `census=1` logs what every frame draws, per ordering-table bucket.

## Layout

- `src/mml.c` entry point, config, anamorphic widescreen, timing, intro bypass
- `src/display.c` native widescreen: wide mode/viewport/clear rect + per-primitive placement in DrawPrimitive
- `src/packets.c` which display-list packet and bucket is being drawn (walker hooks, HUD sprite tags, deferred draws) + the census
- `src/controls.c` orbit camera, camera-relative movement, aim, dodge, button remap
- `src/toast.c` the ORBIT ON/OFF indicator, drawn into the game's own HUD list
- `src/hook.c` mid-function hooks (jump to a stub that saves registers, calls C, replays the stolen bytes)
- `src/iat.c` import table patching (used to catch Xidi's XInput lookup)
- `src/game.h` every game address we touch, named like `tools/ghidra/symbols.csv`
