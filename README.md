# Mega Man Legends Modernization Project

A modernization project for the 2001 PC port of Mega Man Legends, for Windows and Android, with
an experimental Linux version. It
replaces the game's renderer, input and timing with modern versions and adds true widescreen,
60 fps, modern controls and an in-game settings menu.

The project works with your own copy of the game. It contains no game assets: the game's data,
music and voices always come from your copy. It does include a translation of the game's
program code (see [How it works](#how-it-works)), which only runs with the files of your own
copy.

**Current version: 0.8.1 beta.** The 0.1 releases are kept on the
[`legacy-0.1`](../../tree/legacy-0.1) branch and are no longer maintained.

## Contents

- [What's new in 0.8](#whats-new-in-08)
- [What's new in 0.7](#whats-new-in-07)
- [What's new in 0.6](#whats-new-in-06)
- [Features](#features)
- [Requirements](#requirements)
- [Installation on Windows](#installation-on-windows)
- [Installation on Android](#installation-on-android)
- [Installation on Linux (experimental)](#installation-on-linux-experimental)
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

## What's new in 0.8

- **Start menu on Windows.** The settings, the game import and the updates now live in the same
  start menu as on Android and Linux, before the game starts. Setup installs the app into a
  folder of its own, and the start menu imports your US version, Rockman DASH, or both.
- **New start menu layout** on every platform. It uses the whole widescreen: the settings on the
  left in labelled sections, and on the right the selected setting's full description, its
  choices, and the progress of imports, downloads and updates. Settings have clearer, more
  technical names, and the app page is now called **Game**, the first page.
- **Skip the start menu.** **Start menu at launch: Skip** goes straight into the game with your
  saved settings.
- **Back to the start menu** from the game: hold **Back** (or **F2**) for 1.5 seconds on the title
  screen. A ring fills while you hold it.
- **First launch:** when no game has been imported yet, a short prompt explains what the app needs
  and imports your copy.
- **Title screen hints** can be turned off. The shortcut card now also lists the way back to the
  start menu.
- **Linux** is part of the release (still experimental).
- Fix: Rockman DASH with the Original (32-bit) executable crashed when a debug option for the
  recompiled game was set in `mml.ini`.

0.8.1 adds:

- **Update notice.** With the start menu set to Skip, the game checks for a new release itself,
  and the title screen says when one is out. Holding Back / F2 then opens the start menu on its
  **Update** row.
- **Updates on Linux.** The start menu's **Update** downloads the new archive, checks it, installs
  it over the app and starts it again.
- Fix (Linux): holding Back / F2 on the title screen could crash instead of opening the start menu.

The separate launcher (`MML Launcher.exe`) is still installed in game folders that an earlier
Setup patched in place, and updating such a folder keeps it. New installs use the start menu.

## What's new in 0.7

- **Rockman DASH,** the Japanese PC version, is supported on Windows and Android, with both
  engines. Setup installs the patch in the US version, in Rockman DASH, or in both. On Android,
  both versions can be imported side by side, each with its own saves, and the start menu picks
  the one to play. A **Menu buttons** setting chooses the US or the Japanese confirm and back
  buttons in Rockman DASH.
- **English subtitles for the cutscenes** (experimental), which the original release never had.
  They are the work of the
  [Rockman DASH PSP English project](https://github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher)
  and are downloaded from its repository by Setup or by the Android start menu.
- **Japanese voices for the US version** (experimental), built from your own copy of Rockman
  DASH: the cutscenes, the in-game lines and the title screen.
- **Linux version** (experimental): a native x86-64 build with the start menu, the game import,
  Vulkan, controllers, mouse and keyboard, the subtitles and the Japanese voices. It is a separate
  download (see [Installation on Linux](#installation-on-linux-experimental)).

**Experimental.** The subtitles and the Japanese voices have not been checked through the whole
game yet. The subtitles are timed to the English voice acting. With the Japanese voices they
follow the same cutscene timing, so they stay close, but they are not always exactly in sync with
the Japanese line being spoken.

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
- **Renderers.** Direct3D 11 or Vulkan on Windows, Vulkan on Android and Linux.
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
- **Mouse and keyboard** (Windows and Linux). Mouse look, WASD movement and mouse aiming.
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
  shortcuts on the title screen (can be turned off).
- **Back to the start menu** from the title screen: hold Back (or F2).
- **Update notice.** With the start menu set to Skip, the game checks for a new release itself,
  and the title screen says when one is out.
- **New intro video,** with an option to skip it entirely.
- **Timing fix** for the cutscene audio drift on modern PCs.

### Subtitles and Japanese voices (optional, US version, experimental)

- **English subtitles for the cutscenes,** which were never subtitled in the original release.
  They show with the English voices and with the Japanese ones. They are the work of the
  [Rockman DASH PSP English project](https://github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher)
  and are downloaded from its repository by the start menu (see [Credits](#subtitles-and-japanese-voices)).
- **Japanese voices,** built from your own copy of Rockman DASH (the Japanese PC version): the
  cutscenes, the in-game lines and the title screen. The start menu (or the launcher, in older
  installs) switches between the English and the Japanese voices.

**Experimental.** The subtitles and the Japanese voices have not been checked through the whole
game yet. The subtitles are timed to the English voice acting. With the Japanese voices they
follow the same cutscene timing, so they stay close, but they are not always exactly in sync with
the Japanese line being spoken.

## Requirements

**Windows**

- The **Mega Man Legends PC port** (2001 Capcom CD-ROM release), or **Rockman DASH**, the
  Japanese PC version. The installer checks the game executable and only patches the supported
  builds. Rockman DASH is also what the optional Japanese voices for the US version are built from.
- **Windows 10 or 11** (64-bit) and a GPU with Direct3D 11. Vulkan 1.1 for the Vulkan renderer.
- A controller is optional. Mouse and keyboard are fully supported.

**Android**

- A copy of the **Mega Man Legends PC port** or of **Rockman DASH** (the same files as above),
  copied to the device.
- **Android 9 or newer** on a 64-bit (arm64) device with **Vulkan 1.1**.
- About **2.5 GB** of free space (the game is copied into the app).
- A **controller** or a handheld with built-in controls. Touch controls are not available yet.

**Linux (experimental)**

- A copy of the **Mega Man Legends PC port** or of **Rockman DASH** (the same files as above).
- A 64-bit (x86-64) distribution with **glibc 2.38 or newer** (for example SteamOS 3.5, Ubuntu
  24.04, Fedora 39, Debian 13 or a current Arch) and a GPU driver with **Vulkan 1.1**.
- About **2.5 GB** of free space (the game is copied into the app's folder).
- For the folder picker, the desktop's file chooser (the XDG portal) or `zenity`; for the
  subtitles download and updates, `curl` or `wget`.

## Installation on Windows

1. Download `MML-Setup.exe` from the [latest release](../../releases).
2. Run it, choose where to install the app (by default
   `C:\Games\Mega Man Legends Modernization`) and click **Install**.
3. Start **Mega Man Legends** from the desktop shortcut (or `mml_run64.exe --menu` in the app's
   folder). The start menu opens.
4. On first launch it asks for your game: choose **Import Mega Man Legends (US)** and pick the
   folder with `MEGAMAN.EXE`, or **Import Rockman DASH (JP)** and pick the folder with
   `ROCKMAN.EXE`.
5. Wait for the copy to finish, then press **Play** (Start, or Enter).

The import copies the game into the app's folder (Rockman DASH into its own `Japanese` folder,
with its own saves) and patches the copy. It verifies the game executable by hash and refuses
an unknown build, and it leaves the folder you picked unchanged. To bring your saves along, make
sure the `DASH_*.MCD` files are in that folder. The other version can be imported any time on the
**Game** page, which also downloads the English subtitles and builds the Japanese voices.

**Updating:** the start menu checks GitHub for a new release when it opens (**Check on launch**,
Game page) and installs it with **Update**. You can also run the new `MML-Setup.exe` on the same
folder. The imported games, settings and saves are kept.

**Earlier installs:** a game folder that an earlier Setup patched in place is updated where it
is, and keeps `MML Launcher.exe`. Setup moves conflicting wrapper DLLs from other fix packs to
`Modernization\quarantined-wrappers` instead of deleting them, and keeps the original executable
as `MEGAMAN.EXE.vanilla-backup` and the original intro as `CAPCOM.AVI.original`.

## Installation on Android

1. Copy your Mega Man Legends folder (the one with `MEGAMAN.EXE`, `DAT`, `BGM` and `SPH`) from
   your PC to the device, for example into `Download`. To bring your saves along, make sure
   the `DASH_*.MCD` files are in that folder too.
2. Download `MML-<version>.apk` from the [latest release](../../releases) and install it
   (allow installing apps from your browser or file manager when Android asks).
3. Open **Mega Man Legends**. On first launch the start menu asks for your game.
4. Choose **Import Mega Man Legends (US)**, pick the game folder in Android's folder picker, and
   allow access.
5. Wait for the copy to finish (a few minutes), then press **Play**.

For Rockman DASH, copy its folder (the one with `ROCKMAN.EXE`) and import it with **Rockman DASH
(JP)** on the **Game** page in the same way. It is kept apart from the US version, with its own
saves, and **Version to play** picks the one that **Play** starts.

On the same page, **English subtitles** downloads the subtitles for the cutscenes (press A), and
**Voice language** builds the Japanese voices from an imported Rockman DASH (about 260 MB). Once
there, the same rows turn them on and off.

The import checks and patches the executable the same way the Windows installer does, copies
only what the game uses, and leaves the picked folder unchanged. The folder can be deleted
afterwards. To update the app, install the new APK over the old one: the imported game and the
saves stay.

## Installation on Linux (experimental)

The Linux version has only been tested on Windows' Linux subsystem (WSL) so far, with a software
renderer. It has not been tested on real Linux hardware or on the Steam Deck yet. Reports are
welcome (see [Reporting bugs](#reporting-bugs)).

1. Download `mml-linux-<version>-x86_64.tar.gz` from the [latest release](../../releases) and
   extract it anywhere.
2. Copy your Mega Man Legends folder (the one with `MEGAMAN.EXE`, `DAT`, `BGM` and `SPH`) to the
   computer. To bring your saves along, make sure the `DASH_*.MCD` files are in that folder too.
3. Start `mml` in the extracted `mml-linux` folder. On first launch the start menu asks for
   your game.
4. Choose **Import Mega Man Legends (US)** (A, or Enter on the keyboard) and pick the game folder.
5. Wait for the copy to finish, then press **Play** (Start, or Enter).

The game is copied into `~/.local/share/mml-modernization` (or `$XDG_DATA_HOME/mml-modernization`),
which also holds `mml.ini`, the saves and `mml.log`. To use another folder instead, start the game
with `./mml --game-dir <folder>`. Rockman DASH, the subtitles and the Japanese voices work as on
Android (see [Installation on Android](#installation-on-android)).

**Updating:** the start menu checks GitHub for a new release when it opens, and **Update** (Game
page) downloads the new archive, checks it, copies its files over the app's and starts it again.
It needs write access to the `mml-linux` folder. You can also extract the new archive over the
old folder by hand. The game folder and the saves are not in it, so they stay.

In the start menu, the keyboard works as the controller does: the arrow keys choose, Space changes,
Enter plays, Esc quits, and Q and E switch pages.

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
pressed twice. In the start menu, **Start** plays and **B** quits. On the title screen, hold
**Back** for 1.5 seconds to go back to the start menu.

### Mouse and keyboard (Windows and Linux, modern controls)

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
| F2 (hold, title screen) | Back to the start menu |

All of these can be remapped in the start menu (Buttons). With modern controls
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

Settings are changed in the start menu before playing (on Windows installs that an earlier
Setup patched in place, in the launcher). The in-game settings menu changes most of them while
playing. All of them edit the same file (`mml.ini` in the app's or the game's folder). The start
menu shows each setting's full description and choices beside the list.

| Page | Settings |
|------|----------|
| Game | Version to play, importing each version, English subtitles, voice language, start menu at launch (show / skip), intro video, title screen hints, updates |
| Display | Graphics API (Direct3D 11 / Vulkan / DirectDraw legacy), display mode, render resolution, aspect ratio, frame rate, V-Sync, executable |
| Graphics | Anti-aliasing, texture filtering, upscaling filter, shader preset and its input resolution, fog distance, level of detail, texture replacement |
| Controls | Control scheme, modern controls on launch, input API, confirm button (Rockman DASH), camera sensitivity and inversion, stick dead zone, light stick tilt, dodge roll speed, aim mode, crosshair, reticle colour, mouse sensitivity |
| Buttons | Controller and keyboard remapping |

Settings that only exist on Windows (Graphics API, display mode, executable, input API, shaders)
are hidden on Android and Linux, and so are mouse and keyboard and V-Sync on Android.

**Executable** (Windows):

- **Original (32-bit)** (default): the game's own executable with `mml.dll`.
- **Recompiled (64-bit)**: `mml_run64.exe`, the recompiled game. It does not support shader
  presets yet.

**DirectDraw (legacy)** uses DDrawCompat instead of the project's own renderer. It has no 60 fps,
shaders, ReShade depth, in-game menu, notices or title screen shortcuts, and is only meant as a
fallback.

## Shaders, ReShade and texture packs

- **RetroArch shaders** (Windows, Classic engine, Direct3D 11): presets are not included.
  Download RetroArch's [slang-shaders](https://github.com/libretro/slang-shaders) into
  `Modernization\shaders`, then pick a preset in the start menu or the in-game menu. CRT presets
  work best with the shader input set to "Game resolution".
- **ReShade** (Windows, Direct3D 11): install ReShade for `MEGAMAN.EXE` with the DirectX
  10/11/12 API. Depth-based effects work with antialiasing off. The installer leaves an
  existing ReShade in place.
- **Texture packs:** see [docs/TEXTURE-PACKS.md](docs/TEXTURE-PACKS.md) for how to install a
  pack and how to dump and replace textures. On Android and Linux, packs go into the app's
  `Modernization\textures` folder (they are copied with the import when they are in the game
  folder).

## Uninstalling

**Windows:** delete the app's folder (by default `C:\Games\Mega Man Legends Modernization`) and
the desktop shortcut. The folder holds the imported games and the saves made with them: copy the
`DASH_*.MCD` files out of it (and out of its `Japanese` folder) first if you want to keep them.
The folder you imported from is not changed.

**Windows, a game folder patched in place by an earlier Setup:**

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

**Linux:** delete the `mml-linux` folder, then `~/.local/share/mml-modernization` (the imported
game and the saves: copy the `DASH_*.MCD` files out of it first if you want to keep them).

## Known issues

- Some in-engine cutscenes may show black or missing textures.
- Antialiasing disables depth-based ReShade effects.
- Shader presets and ReShade depth work only with the Classic engine on Direct3D 11.
- The in-game settings menu, notices, title-screen shortcuts and the way back to the start menu
  require the project's own renderers (not DirectDraw legacy).
- With the Legacy controller backend, the controller still reaches the game while the in-game
  menu is open.
- Android: no touch controls, and no mouse and keyboard support yet.
- The subtitles and the Japanese voices are experimental: they have not been checked through
  the whole game yet.
- With the Japanese voices, the subtitles (timed to the English voice acting) are not always
  exactly in sync with the Japanese line being spoken.
- Japanese voices: lines that only the Japanese version voices stay silent, and a long Japanese
  line can be cut off where the game moves on after the English line's length.
- Linux: tested only on Windows' Linux subsystem so far, not on real hardware or the Steam Deck.
  It needs glibc 2.38 or newer.
- Running the Windows version through Proton or Wine has not been tested.

## Roadmap

- **Linux:** testing on real hardware and the Steam Deck, and a build for older distributions.
- **Touch controls** on Android.
- **Retiring the separate launcher** in game folders patched in place by earlier versions.
- Shader presets for the Vulkan renderer and the standalone engine.
- Later candidates: bloom and ambient occlusion, HUD repositioning options, and further mod
  support.

## Reporting bugs

Open an [issue](../../issues) and include:

- `mml.log` from the app's folder on Windows (or the game folder, for installs patched in place;
  on Android: `Android/data/org.mml.legends/files`; on Linux: `~/.local/share/mml-modernization`). It is
  rewritten every time the game starts, so copy it right after the problem. Its first line
  identifies the build.
- On Windows, `Modernization\menu.log` (the start menu), or `Modernization\launcher.log` for
  installs that use the launcher.
- What you were doing, and a screenshot if the problem is visual.
- If the installer or the start menu's import rejected your game: the hash it reported (in
  `menu.log` on Windows, in `mml.log` on Android and Linux). Other builds of the PC port exist, and their hashes help add support for
  them.

## How it works

- **A small static patch** to `MEGAMAN.EXE` enlarges two renderer tables that overflow with
  the extended draw distance, fixes the culling around them, and adds `mml.dll` to the
  executable's imports so Windows loads it with the game. The patch is hash-verified and a
  backup is kept. The Android and Linux imports apply the same patch.
- **The recompiled game.** A static recompiler ([tools/recomp](tools/recomp), built on Ghidra)
  translates the game's x86 program into C. That code runs the game in the standalone engine,
  on Android and on Linux, with the original executable's code never running. The executable is still
  loaded for its data, so your own copy is always needed.
- **The runtime** (C) does everything else: it replaces the game's DirectDraw / Direct3D 3
  output with its own renderer (Direct3D 11 or Vulkan; widescreen placement, 60 fps
  interpolation, effects, the in-game UI), reads controllers through SDL3, implements the
  modern controls, and fixes the timing. On Windows Classic it is `mml.dll`, loaded by the game;
  in the standalone engine, on Android and on Linux it also provides the Windows functions the
  game calls.
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

**Linux.** Requirements: clang, Python 3, `glslangValidator` and `spirv-opt` (glslang and
SPIRV-Tools), the development files of FreeType and fontconfig, and SDL3 built for Linux
(installed into a folder with `include` and `lib`).

| Command | Output |
|---------|--------|
| `python3 linux/build_linux.py` | `linux/build/mml-linux` and `mml-linux-<version>-x86_64.tar.gz` |

The paths are set with environment variables (`SDL3_LINUX`, `SDL3_SRC`, `MML_LINUX_OUT`);
`MML_GAME_DIR` provides the intro video to bundle. See the top of `linux/build_linux.py`.

Icons are made from the artwork in `art/icon` with `tools/icons/make_icons.py`.

## Credits and licenses

The project's own code is MIT licensed (see [LICENSE](LICENSE)). It ships these open-source
components; full licenses are in [licenses/](licenses) and sources in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt):

- [SDL3](https://github.com/libsdl-org/SDL) (controller input; on Android and Linux also the
  window, audio and the app), zlib
- [librashader](https://github.com/SnowflakePowered/librashader) (RetroArch shader presets), MPL-2.0
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (intro video), MIT
- [DSOAL](https://github.com/kcat/dsoal) and [OpenAL Soft](https://github.com/kcat/openal-soft) (audio), LGPL-2.1
- [DDrawCompat](https://github.com/narzoul/DDrawCompat) (Compatibility renderer), 0BSD
- [Xidi](https://github.com/samuelgr/Xidi) (Legacy controller backend), BSD-3-Clause

On Linux, text is drawn with the system's own FreeType and fontconfig, which are not included.

The Legacy controller configuration builds on the Xidi setup from KuroRaiden's
*Mega Man Legends PC Fix* repack (@NC_KuroRaiden).

### Subtitles and Japanese voices

The cutscenes' English subtitles show with the English voices and with the Japanese ones. The
Japanese voices are built on the player's own computer or device from their copy of Rockman DASH
(the Japanese PC version). Nothing of either game comes with the patch.

- **Subtitles:** the English subtitles are the work of the
  [Rockman DASH PSP English project](https://github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher)
  (JamesAZ0 and its contributors), who transcribed the US voice acting and timed the lines to it.
  Setup (or the start menu on Android and Linux) downloads its `script/subtitles_en.csv` (commit
  `7d49ceb`) from that repository and converts it for the PC version; this repository and the
  patch's downloads never contain it.
  That project's work is under the MIT license (Copyright (c) 2026 The Rockman DASH PSP English
  project contributors). The lines belong to Capcom.
- **Through that project:** it credits OpenAI's Whisper, which it used to transcribe the English
  voice acting for the timings, Estil's GameFAQs script, which it used to check the transcripts,
  and OmbraRD's [DashEditor](https://github.com/OmbraRD/DashEditor), whose research into the
  game's file formats was the starting point of its tools.
- **The Japanese PSX English patch:** Gaiko's Mega Man Legends Japanese PSX English patch
  (v1.1.13, [Romhacking.net](https://www.romhacking.net/translations/7838/)) led us to the PSP
  project and its subtitle timing.

### Research

- [The Cutting Room Floor](https://tcrf.net/Mega_Man_Legends): its list of the regional
  differences between Mega Man Legends and Rockman DASH guides the optional Japanese-version
  features.

Mega Man Legends is a trademark of Capcom. This project is not affiliated with Capcom.
