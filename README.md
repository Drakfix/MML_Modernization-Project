# Mega Man Legends Modernization Project

A modernization project for the 2001 PC port of Mega Man Legends, for Windows and Android. It
replaces the game's renderer, input and timing with modern versions and adds true widescreen,
60 fps, modern controls and an in-game settings menu.

The project works with your own copy of the game. It contains no game assets: the game's data,
music and voices always come from your copy. It does include a translation of the game's
program code (see [How it works](#how-it-works)), which only runs with the files of your own
copy.

**Current version: 0.6.3 beta.** The 0.1 releases are kept on the
[`legacy-0.1`](../../tree/legacy-0.1) branch and are no longer maintained.

## Contents

- [What's new in 0.6](#whats-new-in-06)
- [Features](#features)
- [Requirements](#requirements)
- [Installation on Windows](#installation-on-windows)
- [Installation on Android](#installation-on-android)
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

## What's new in 0.6

- **Android version.** A native arm64 build of the game, not an emulator, rendered with Vulkan.
  It imports your own copy of the game from a folder on the device and runs at 60 fps on current
  handhelds.
- **Start menu** on Android: every setting before the game starts, button remapping, and the
  import of your game files.
- **Vulkan renderer** on Windows, next to Direct3D 11 (Renderer setting).
- **Standalone engine** on Windows (Advanced setting): the recompiled game in a 64-bit program,
  without the original executable's code.
- **Render thread:** the renderer's work runs beside the game, which gives smoother frame pacing
  and more headroom on slower CPUs.
- Fades now cover the whole screen at high resolutions.
- New icon.

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
- **Renderers.** Direct3D 11 or Vulkan on Windows, Vulkan on Android.
- **Display modes** (Windows). Borderless (the default, with normal Alt+Tab, screenshots and
  capture), exclusive fullscreen or windowed.
- **Post-processing** (Windows, Direct3D 11). RetroArch shader presets (CRT and others)
  through librashader, and ReShade with a working depth buffer.
- **HD texture packs.** Replace any game texture with a PNG of any size, or dump the
  originals to make your own.

### Controls

- **Modern controls.** Camera-relative movement, a free camera on the right stick, aim on
  the triggers, and dodge rolls in any direction. Can be switched off at any time to play
  with the original controls and camera.
- **Lock-on targeting.** Hold aim to lock on to the nearest enemy in view. Mega Man keeps
  facing the target while moving freely; flick the right stick (or use the mouse wheel) to
  switch targets. The reticle colour is configurable.
- **Camera aim** (optional). Shots go where the camera looks, up and down included, with an
  optional crosshair. Mega Man turns to the camera's direction only while firing, so movement
  stays free in every direction.
- **Analog movement.** A light stick tilt walks, a full tilt runs.
- **Any controller.** SDL3 supports Xbox, PlayStation, Switch and most other pads, and the
  built-in controls of Android handhelds. The D-pad works as a second movement input, as in
  the PS1 version.
- **Mouse and keyboard** (Windows). Mouse look, WASD movement and mouse aiming.
- **Remapping.** Every controller button and keyboard key used by the modern controls can be
  rebound.
- **Vanilla mode.** Leaves the game's controls completely stock.

### Quality of life

- **In-game settings menu** (F1, or Back + Start). Changes apply instantly, which makes it easy
  to compare graphics options side by side. Settings are saved and shared with the launcher
  and the start menu.
- **Quick save** (F6, or Back + RB) to a dedicated slot, from anywhere you have control of
  Mega Man.
- **On-screen notices** for quick saves and the modern controls toggle, and a short list of
  shortcuts on the title screen.
- **New intro video,** with an option to skip it entirely.
- **Timing fix** for the cutscene audio drift on modern PCs.

## Requirements

**Windows**

- The **Mega Man Legends PC port** (2001 Capcom CD-ROM release). The installer checks the
  game executable and only patches the supported build.
- **Windows 10 or 11** (64-bit) and a GPU with Direct3D 11. Vulkan 1.1 for the Vulkan renderer.
- A controller is optional. Mouse and keyboard are fully supported.

**Android**

- A copy of the **Mega Man Legends PC port** (the same files as above), copied to the device.
- **Android 9 or newer** on a 64-bit (arm64) device with **Vulkan 1.1**.
- About **2.5 GB** of free space (the game is copied into the app).
- A **controller** or a handheld with built-in controls. Touch controls are not available yet.

## Installation on Windows

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
- Installs the runtime, the standalone engine, the launcher, the compatibility wrappers and
  their licenses.
- Installs the new intro and keeps the original as `CAPCOM.AVI.original`.

**Upgrading:** run the new installer on the same folder. Your saves and settings are kept.

## Installation on Android

1. Copy your Mega Man Legends folder (the one with `MEGAMAN.EXE`, `DAT`, `BGM` and `SPH`) from
   your PC to the device, for example into `Download`. To bring your saves along, make sure
   the `DASH_*.MCD` files are in that folder too.
2. Download `MML-<version>.apk` from the [latest release](../../releases) and install it
   (allow installing apps from your browser or file manager when Android asks).
3. Open **Mega Man Legends**. The start menu opens on the **Files** page.
4. Choose **Import**, pick the game folder in Android's folder picker, and allow access.
5. Wait for the copy to finish (a few minutes), then press **Play**.

The import checks and patches the executable the same way the Windows installer does, copies
only what the game uses, and leaves the picked folder unchanged. The folder can be deleted
afterwards. To update the app, install the new APK over the old one: the imported game and the
saves stay.

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

On Android, the system's **Back** button closes the settings menu, or closes the game when
pressed twice. In the start menu, **Start** plays and **B** quits.

### Mouse and keyboard (Windows, modern controls)

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

All of these can be remapped in the launcher or the start menu (Buttons). With modern controls
off, or in Vanilla mode, the game uses its original controls and camera.

### Camera aim

With **Shots aim: Camera** (Controls settings), shooting works like a third-person shooter
while modern controls are on:

| Situation | What happens |
|-----------|--------------|
| Fire (shoot or special weapon) | Mega Man turns to where the camera looks, and the shot goes there, up and down included |
| Move while firing | He keeps facing the camera's direction and runs or strafes around it, firing continuously |
| Move toward the camera while firing | He turns and runs toward the camera, shooting where he runs (the game has no way to shoot while backing away) |
| Stop firing | He keeps facing that way for a moment, then moves freely again |
| Fire while jumping | He turns to shoot where the camera looks; the jump keeps drifting where the stick points |
| Talk, check or kick | Camera aim does not apply: he faces what he is interacting with |
| Aim / lock-on held | Lock-on works as usual; camera aim does not apply |

**Crosshair:** shows where camera-aimed shots go. It appears with the first shot, fades out a
few seconds after the last one, and uses the lock-on reticle colour. It needs the project's
renderer (Direct3D 11 or Vulkan).

With **Shots aim: Character** (the default), shots go where Mega Man faces, as in the original
game.

| `mml.ini` `[controls]` | Values |
|------------------------|--------|
| `aim` | `character` (default), `camera` |
| `crosshair` | `1` (default), `0` |

### Quick save

Quick save writes to save slot 10, which appears as "Quick save" on the load screen. It never
overwrites a slot-10 save made from the game's own save menu, and it only works while you are
in control of Mega Man (not in menus or cutscenes).

## Settings

On Windows, settings are changed in the launcher before playing; on Android, in the start
menu. The in-game settings menu changes most of them while playing. All of them edit the same
file (`mml.ini` in the game folder).

| Page | Settings |
|------|----------|
| Display | Renderer (Direct3D 11 / Vulkan / Compatibility), display mode, resolution, aspect ratio, frame rate, intro video; Advanced: game engine |
| Graphics | Texture filter, upscaling, antialiasing, distance fog, model detail, shader preset, HD textures |
| Controls | Control scheme, modern controls at start, controller backend, camera speed and inversion, stick deadzone, walk tilt, roll speed, mouse speed, lock-on reticle colour, shots aim (character / camera), crosshair |
| Buttons | Controller and keyboard remapping |
| Files (Android) | The game files' status, and importing them |

Settings that only exist on Windows (renderer, display mode, game engine, controller backend,
shaders, mouse and keyboard) are hidden on Android.

**Game engine** (Windows, Advanced):

- **Classic** (default): the game's own executable with `mml.dll`.
- **Standalone (64-bit)**: `mml_run64.exe`, the recompiled game. It does not support shader
  presets yet.

The **Compatibility** renderer uses DDrawCompat instead of the project's own renderer. It has
no 60 fps, shaders, ReShade depth, in-game menu or notices, and is only meant as a fallback.

## Shaders, ReShade and texture packs

- **RetroArch shaders** (Windows, Classic engine, Direct3D 11): presets are not included.
  Download RetroArch's [slang-shaders](https://github.com/libretro/slang-shaders) into
  `Modernization\shaders`, then pick a preset in the launcher or the in-game menu. CRT presets
  work best with the shader input set to "Game resolution".
- **ReShade** (Windows, Direct3D 11): install ReShade for `MEGAMAN.EXE` with the DirectX
  10/11/12 API. Depth-based effects work with antialiasing off. The installer leaves an
  existing ReShade in place.
- **Texture packs:** see [docs/TEXTURE-PACKS.md](docs/TEXTURE-PACKS.md) for how to install a
  pack and how to dump and replace textures. On Android, packs go into the app's
  `Modernization\textures` folder (they are copied with the import when they are in the game
  folder).

## Uninstalling

**Windows**

1. Rename `MEGAMAN.EXE.vanilla-backup` back to `MEGAMAN.EXE`.
2. Rename `CAPCOM.AVI.original` back to `CAPCOM.AVI`.
3. Delete `MML Launcher.exe`, `mml.dll`, `mml_run64.exe`, `mml.ini`, `DDrawCompat.ini`, the
   `Modernization` and `x64` folders, `THIRD-PARTY-NOTICES.txt` and the `licenses` folder.
4. Delete the added DLLs: `SDL3.dll`, `librashader.dll`, `ddraw.dll`, `dinput.dll`,
   `dsound.dll` and `dsoal-aldrv.dll`. If you had your own wrappers before, restore them from
   `Modernization\quarantined-wrappers` first.

**Android:** uninstall the app. This also removes the imported game and the saves made on the
device, so copy the `DASH_*.MCD` files out of `Android/data/org.mml.legends/files` first if you
want to keep them.

## Known issues

- Some in-engine cutscenes may show black or missing textures.
- Antialiasing disables depth-based ReShade effects.
- Shader presets and ReShade depth work only with the Classic engine on Direct3D 11.
- The in-game settings menu, notices and title-screen shortcuts require the project's own
  renderers (not Compatibility).
- With the Legacy controller backend, the controller still reaches the game while the in-game
  menu is open.
- Android: no touch controls, and no mouse and keyboard support yet.
- Linux (Proton/Wine) and the Steam Deck have not been tested yet.

## Roadmap

- **Linux version,** sharing most of the Android work.
- **Touch controls** on Android.
- **The start menu on Windows,** replacing the separate launcher.
- Shader presets for the Vulkan renderer and the standalone engine.
- Later candidates: bloom and ambient occlusion, HUD repositioning options, and further mod
  support.

## Reporting bugs

Open an [issue](../../issues) and include:

- `mml.log` from the game folder (on Android: `Android/data/org.mml.legends/files`). It is
  rewritten every time the game starts, so copy it right after the problem. Its first line
  identifies the build.
- On Windows, `Modernization\launcher.log`.
- What you were doing, and a screenshot if the problem is visual.
- If the installer or the Android import rejected your game: the hash it reported (on Android
  it is in `mml.log`). Other builds of the PC port exist, and their hashes help add support for
  them.

## How it works

- **A small static patch** to `MEGAMAN.EXE` enlarges two renderer tables that overflow with
  the extended draw distance, fixes the culling around them, and adds `mml.dll` to the
  executable's imports so Windows loads it with the game. The patch is hash-verified and a
  backup is kept. The Android import applies the same patch.
- **The recompiled game.** A static recompiler ([tools/recomp](tools/recomp), built on Ghidra)
  translates the game's x86 program into C. That code runs the game in the standalone engine
  and on Android, with the original executable's code never running. The executable is still
  loaded for its data, so your own copy is always needed.
- **The runtime** (C) does everything else: it replaces the game's DirectDraw / Direct3D 3
  output with its own renderer (Direct3D 11 or Vulkan; widescreen placement, 60 fps
  interpolation, effects, the in-game UI), reads controllers through SDL3, implements the
  modern controls, and fixes the timing. On Windows Classic it is `mml.dll`, loaded by the game;
  in the standalone engine and on Android it also provides the Windows functions the game
  calls.
- **Wrappers** (Windows Classic) handle the rest: DSOAL and OpenAL Soft for the game's
  DirectSound3D audio, DDrawCompat for the Compatibility renderer, and Xidi for the Legacy
  controller backend.
- The launcher and the start menu only edit `mml.ini`. Every part can be switched off.

The reverse-engineering tooling (a Ghidra project setup and symbol map) is in
[tools/ghidra](tools/ghidra).

## Building from source

The recompiled game code is generated from your own `MEGAMAN.EXE` with
[tools/recomp](tools/recomp) and is never stored in the repository; without it the runtime
builds without the standalone engine.

**Windows.** Requirements: Visual Studio Build Tools (C++ workload, x86 and x64 tools), the
Android NDK's `clang-cl` (set `RECOMP_CLANG` to it) and Python 3 with
`pip install pyinstaller pillow`.

| Command | Output |
|---------|--------|
| `native\build.bat` | `native\build\mml.dll` |
| `native\build_runner.bat x64` | `native\build\mml_run64.exe` |
| `build\build_release.bat` | `dist\MML-Setup.exe` (also rebuilds the runtime and the launcher) |

`build_release.bat` needs `MML_GAME_DIR` set to a game folder that holds the redistributable
DLLs (`SDL3.dll`, `x64\SDL3.dll`, `librashader.dll` and the wrappers) and the intro video, so
they can be packaged. librashader is built from source; see
[docs/BUILDING-librashader.md](docs/BUILDING-librashader.md).

**Android.** Requirements: the Android SDK with build-tools, platform 35 and NDK 27, the JDK
that comes with Android Studio, SDL3's source and SDL3 built for Android (arm64-v8a).

| Command | Output |
|---------|--------|
| `python android/build_android.py` | `android\build\mml.apk` |

The paths are set with environment variables (`ANDROID_SDK`, `JAVA_HOME`, `SDL3_SRC`,
`SDL3_ANDROID`); `MML_GAME_DIR` provides the intro video to bundle. See the top of
`android/build_android.py`.

Icons are made from the artwork in `art/icon` with `tools/icons/make_icons.py`.

## Credits and licenses

The project's own code is MIT licensed (see [LICENSE](LICENSE)). It ships these open-source
components; full licenses are in [licenses/](licenses) and sources in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt):

- [SDL3](https://github.com/libsdl-org/SDL) (controller input; on Android also the window,
  audio and the app), zlib
- [librashader](https://github.com/SnowflakePowered/librashader) (RetroArch shader presets), MPL-2.0
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (intro video), MIT
- [DSOAL](https://github.com/kcat/dsoal) and [OpenAL Soft](https://github.com/kcat/openal-soft) (audio), LGPL-2.1
- [DDrawCompat](https://github.com/narzoul/DDrawCompat) (Compatibility renderer), 0BSD
- [Xidi](https://github.com/samuelgr/Xidi) (Legacy controller backend), BSD-3-Clause

The Legacy controller configuration builds on the Xidi setup from KuroRaiden's
*Mega Man Legends PC Fix* repack (@NC_KuroRaiden).

Mega Man Legends is a trademark of Capcom. This project is not affiliated with Capcom.
