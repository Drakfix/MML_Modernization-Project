# Mega Man Legends Modernization Project

A modernization patch for the 2001 PC port of Mega Man Legends. It replaces the game's
renderer, input and timing with modern versions and adds true widescreen, 60 fps, modern
controls and an in-game settings menu, while running the original game code.

The project patches your own copy of the game. It contains no Capcom code or assets.

**Current version: 0.5.0 beta.** This is a full rewrite of the 0.1 series. The 0.1 releases
are kept on the [`legacy-0.1`](../../tree/legacy-0.1) branch and are no longer maintained.

## Contents

- [What's new in 0.5](#whats-new-in-05)
- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Controls](#controls)
- [Settings](#settings)
- [Shaders, ReShade and texture packs](#shaders-reshade-and-texture-packs)
- [Uninstalling](#uninstalling)
- [Known issues](#known-issues)
- [Roadmap](#roadmap)
- [Reporting bugs](#reporting-bugs)
- [How it works](#how-it-works)
- [Building from source](#building-from-source)
- [Credits and licenses](#credits-and-licenses)

## What's new in 0.5

Version 0.1 patched the game from a separate Python/Frida process. Version 0.5 is a native
runtime (`mml.dll`) that the game loads itself, and it goes much further:

- Its own Direct3D 11 renderer instead of DirectDraw, with true 16:9 and 60 fps
- Controller support through SDL3 (Xbox, PlayStation, Switch and other pads)
- Lock-on targeting, directional dodge rolls and analog walking
- Mouse and keyboard controls, with full button and key remapping
- In-game settings menu with live preview, and quick save
- RetroArch shader presets, ReShade support with depth, HD texture replacement
- Redesigned launcher, and a new intro video player

## Features

### Graphics

- **True 16:9 widescreen.** The game renders to a wide surface: the 3D view gets the extra
  width, HUD elements are placed at correct proportions, and 4:3 backgrounds are stretched to
  fit. A 4:3 mode is available.
- **60 fps.** The game logic still runs at 30 fps; the renderer draws an interpolated frame in
  between. Can be switched back to 30 fps.
- **Resolution and image quality.** Render at your display's resolution or at 1x to 5x the
  game's resolution, with up to 8x MSAA, 16x anisotropic filtering and a choice of bilinear or
  nearest-neighbour upscaling.
- **Draw distance options.** Lighter or no distance fog, and full-detail models at any
  distance instead of the original low-poly swaps.
- **Display modes.** Borderless (the default, with normal Alt+Tab, screenshots and capture),
  exclusive fullscreen or windowed.
- **Post-processing.** RetroArch shader presets (CRT and others) through librashader, and
  ReShade with a working depth buffer.
- **HD texture packs.** Replace any game texture with a PNG of any size, or dump the
  originals to make your own.

### Controls

- **Modern controls.** Camera-relative movement, a free camera on the right stick, aim on
  the triggers, and dodge rolls in any direction. Can be switched off at any time to play
  with the original controls and camera.
- **Lock-on targeting.** Hold aim to lock on to the nearest enemy in view. Mega Man keeps
  facing the target while moving freely; flick the right stick (or use the mouse wheel) to
  switch targets. The reticle colour is configurable.
- **Analog movement.** A light stick tilt walks, a full tilt runs.
- **Any controller.** SDL3 supports Xbox, PlayStation, Switch and most other pads. The D-pad
  works as a second movement input, as in the PS1 version.
- **Mouse and keyboard.** Mouse look, WASD movement and mouse aiming.
- **Remapping.** Every controller button and keyboard key used by the modern controls can be
  rebound in the launcher.
- **Vanilla mode.** Leaves the game's controls completely stock.

### Quality of life

- **In-game settings menu** (F1, or Back + Start). Changes apply instantly, which makes it easy
  to compare graphics options side by side. Settings are saved and shared with the launcher.
- **Quick save** (F6, or Back + RB) to a dedicated slot, from anywhere you have control of
  Mega Man.
- **On-screen notices** for quick saves and the modern controls toggle, and a short list of
  shortcuts on the title screen.
- **New intro video,** with an option to skip it entirely.
- **Timing fix** for the cutscene audio drift on modern PCs.

## Requirements

- The **Mega Man Legends PC port** (2001 Capcom CD-ROM release). The installer checks the
  game executable and only patches the supported build.
- **Windows 10 or 11** (64-bit) and a GPU with Direct3D 11.
- A controller is optional. Mouse and keyboard are fully supported.

## Installation

1. Download `MML-Setup.exe` from the [latest release](../../releases).
2. Run it and select your game folder (it tries to find it automatically).
3. Click **Install**.
4. Start the game with `MML Launcher.exe` in the game folder, or with the optional desktop
   shortcut.

The installer:

- Verifies the game executable by hash, and refuses to patch an unknown build.
- Backs up the executable as `MEGAMAN.EXE.vanilla-backup`, then verifies the patched file.
- Moves conflicting wrapper DLLs from other fix packs to `Modernization\quarantined-wrappers`
  instead of deleting them.
- Installs the runtime, the launcher, the compatibility wrappers and their licenses.
- Installs the new intro and keeps the original as `CAPCOM.AVI.original`.

**Upgrading from 0.1.x:** run the new installer on the same folder. The old 0.1 runtime files
are moved to `Modernization\legacy-0.1`. Your saves are not touched.

## Controls

### Controller (modern controls)

| Input | Action |
|-------|--------|
| Left stick / D-pad | Move (light tilt walks) |
| Right stick | Camera; while locked on, flick to switch targets |
| A | Jump |
| B | Special weapon |
| X | Dodge roll |
| Y | Talk / check |
| LT or LB | Aim / lock-on |
| RT | Shoot |
| L3 or R3 | Modern controls on/off |
| Start | Game menu |
| Back | Select |
| Back + Start | Settings menu |
| Back + RB | Quick save |

### Mouse and keyboard (modern controls)

| Input | Action |
|-------|--------|
| W A S D / arrow keys | Move |
| Mouse | Camera |
| Left Ctrl (hold) | Walk |
| Space | Jump |
| Left click | Shoot |
| Right click | Aim / lock-on |
| Mouse wheel | Next target while locked on |
| Left Shift | Dodge roll |
| E | Talk / check |
| Q | Special weapon |
| Tab | Game menu |
| M | Select |
| F5 | Modern controls on/off |
| F1 | Settings menu |
| F6 | Quick save |

All of these can be remapped in the launcher (Buttons tab). With modern controls off, or in
Vanilla mode, the game uses its original controls and camera.

### Quick save

Quick save writes to save slot 10, which appears as "Quick save" on the load screen. It never
overwrites a slot-10 save made from the game's own save menu, and it only works while you are
in control of Mega Man (not in menus or cutscenes).

## Settings

Settings can be changed in the launcher before playing, or in the in-game settings menu
while playing. Both edit the same file (`mml.ini` in the game folder).

| Tab | Settings |
|-----|----------|
| Display | Renderer (Modern / Compatibility), display mode, resolution, aspect ratio, frame rate, intro video |
| Graphics | Texture filter, upscaling, antialiasing, distance fog, model detail, shader preset, HD textures |
| Controls | Control scheme, modern controls at start, controller backend, camera speed and inversion, stick deadzone, walk tilt, roll speed, mouse speed, lock-on reticle colour |
| Buttons | Controller and keyboard remapping |

The in-game menu covers frame rate, vsync, resolution, antialiasing, texture filtering,
upscaling, shaders, fog, model detail, HD textures, camera and mouse speed, inversion and the
reticle colour. The renderer, display mode, aspect ratio, control scheme, controller backend
and remapping are set in the launcher.

The **Compatibility** renderer uses DDrawCompat instead of the project's own renderer. It has
no 60 fps, shaders, ReShade depth, in-game menu or notices, and is only meant as a fallback.

## Shaders, ReShade and texture packs

- **RetroArch shaders:** presets are not included. Download RetroArch's
  [slang-shaders](https://github.com/libretro/slang-shaders) into `Modernization\shaders`,
  then pick a preset in the launcher or the in-game menu. CRT presets work best with the
  shader input set to "Game resolution".
- **ReShade:** install ReShade for `MEGAMAN.EXE` with the DirectX 10/11/12 API. Depth-based
  effects work with antialiasing off. The installer leaves an existing ReShade in place.
- **Texture packs:** see [docs/TEXTURE-PACKS.md](docs/TEXTURE-PACKS.md) for how to install a
  pack and how to dump and replace textures.

## Uninstalling

1. Rename `MEGAMAN.EXE.vanilla-backup` back to `MEGAMAN.EXE`.
2. Rename `CAPCOM.AVI.original` back to `CAPCOM.AVI`.
3. Delete `MML Launcher.exe`, `mml.dll`, `mml.ini`, `DDrawCompat.ini`, the `Modernization`
   folder, `THIRD-PARTY-NOTICES.txt` and the `licenses` folder.
4. Delete the added DLLs: `SDL3.dll`, `librashader.dll`, `ddraw.dll`, `dinput.dll`,
   `dsound.dll` and `dsoal-aldrv.dll`. If you had your own wrappers before, restore them from
   `Modernization\quarantined-wrappers` first.

## Known issues

- Some in-engine cutscenes may show black or missing textures.
- Antialiasing disables depth-based ReShade effects.
- The in-game settings menu, notices and title-screen shortcuts require the Modern renderer.
- With the Legacy controller backend, the controller still reaches the game while the in-game
  menu is open.
- Linux (Proton/Wine) and the Steam Deck have not been tested yet.

## Roadmap

- **Android port.** A native build, not an emulator. The app will build itself from the game
  data folder of your own copy and will not contain any game assets.
- **Linux port,** sharing most of the Android work.
- Later candidates: bloom and ambient occlusion, HUD repositioning options, and further mod
  support.

## Reporting bugs

Open an [issue](../../issues) and include:

- `mml.log` from the game folder. It is rewritten every time the game starts, so copy it right
  after the problem. Its first line identifies the build.
- `Modernization\launcher.log`.
- What you were doing, and a screenshot if the problem is visual.
- If the installer rejected your game: the hash it reported. Other builds of the PC port
  exist, and their hashes help add support for them.

## How it works

- **A small static patch** to `MEGAMAN.EXE` enlarges two renderer tables that overflow with
  the extended draw distance, fixes the culling around them, and adds `mml.dll` to the
  executable's imports so Windows loads it with the game. The patch is hash-verified and a
  backup is kept.
- **`mml.dll`** (C) does everything else at runtime: it replaces the game's DirectDraw /
  Direct3D 3 output with a Direct3D 11 renderer (widescreen placement, 60 fps interpolation,
  effects, the in-game UI), reads controllers through SDL3, implements the modern controls,
  and fixes the timing.
- **Wrappers** handle the rest: DSOAL and OpenAL Soft for the game's DirectSound3D audio,
  DDrawCompat for the Compatibility renderer, and Xidi for the Legacy controller backend.
- The launcher only edits `mml.ini`. The game files stay close to stock and every part can be
  switched off.

The reverse-engineering tooling (a Ghidra project setup and symbol map) is in
[tools/ghidra](tools/ghidra).

## Building from source

Requirements: Visual Studio Build Tools (C++ workload, x86 tools) and Python 3 with
`pip install pyinstaller pillow`.

| Command | Output |
|---------|--------|
| `native\build.bat` | `native\build\mml.dll` |
| `build\build_release.bat` | `dist\MML-Setup.exe` (also rebuilds `mml.dll` and the launcher) |

`build_release.bat` needs `MML_GAME_DIR` set to a game folder that holds the redistributable
DLLs (`SDL3.dll`, `librashader.dll` and the wrappers) and the intro video, so they can be
packaged. Nothing from the game is stored in the repository. librashader is built from
source; see [docs/BUILDING-librashader.md](docs/BUILDING-librashader.md).

## Credits and licenses

The project's own code is MIT licensed (see [LICENSE](LICENSE)). It ships these open-source
components; full licenses are in [licenses/](licenses) and sources in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt):

- [SDL3](https://github.com/libsdl-org/SDL) (controller input), zlib
- [librashader](https://github.com/SnowflakePowered/librashader) (RetroArch shader presets), MPL-2.0
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (intro video), MIT
- [DSOAL](https://github.com/kcat/dsoal) and [OpenAL Soft](https://github.com/kcat/openal-soft) (audio), LGPL-2.1
- [DDrawCompat](https://github.com/narzoul/DDrawCompat) (Compatibility renderer), 0BSD
- [Xidi](https://github.com/samuelgr/Xidi) (Legacy controller backend), BSD-3-Clause

The Legacy controller configuration builds on the Xidi setup from KuroRaiden's
*Mega Man Legends PC Fix* repack (@NC_KuroRaiden).

Mega Man Legends is a trademark of Capcom. This project is not affiliated with Capcom.
