# Mega Man Legends — Modernization Project

A compatibility and quality-of-life patch for the 2001 PC port of *Mega Man Legends*.
It adds proper widescreen, a modern twin-stick control scheme, an audio-timing fix, and a
number of rendering fixes on top of the original executable — without replacing the game's
own renderer or requiring any emulator.

The patch operates on **your own copy** of the game. It contains no Capcom code or assets.

## Features

- **Widescreen (16:9)** with a correct anamorphic field of view — not a stretched 4:3 image.
- **Modern controls** (optional): right-stick orbit camera, camera-relative movement, a
  dodge roll, and a twin-stick shooter layout (LT aim / RT shoot / X dodge). The original
  tank controls are still available.
- **HUD/UI un-stretch** so the 2D overlay isn't horizontally distorted in widescreen.
- **Audio-timing fix** for the cutscene desync caused by the game's frame limiter losing
  float precision at high system uptime.
- **Stability fixes** to the ordering-table and render queues that cause crashes and
  dropped geometry at wider fields of view.
- **Higher internal resolution** via DDrawCompat, with selectable texture filtering and MSAA.
- Optional custom intro, or skip the intro entirely.

Everything except the executable byte-patches is applied at runtime by a launcher, so the
game files stay close to stock and the changes are easy to reverse.

## Requirements

- A legitimate copy of the **Mega Man Legends PC port** (the 2001 Capcom CD-ROM release).
  This project does not distribute the game.
- Windows 10 or 11 (64-bit).

## Installing

Run **`MML-Setup.exe`** from the latest release, point it at your game folder, and click
Install. It verifies the executable, backs it up, applies the patch, and installs the
runtime files. There is nothing else to install — the launcher is self-contained.

To play afterwards, use **`MML Launcher.exe`** in the game folder (or the optional Desktop
shortcut). The launcher lets you pick resolution, aspect ratio, control scheme, filtering,
and the intro behaviour each time; it can also create a one-click "play with these settings"
shortcut.

## Controls (modern scheme)

| Input | Action |
|-------|--------|
| Left stick | Move (camera-relative) |
| Right stick | Orbit camera |
| D-pad | Same as the left stick (menus + movement) |
| LT | Aim | RT | Shoot |
| X | Dodge roll | A | Jump |
| F5 / stick-click | Toggle the orbit camera on/off |

## How it works

The static portion is a small set of byte-patches to `MEGAMAN.EXE` (widening the visibility
windows, enlarging the render queues into new PE sections, and fixing the ordering-table
overflow). The runtime portion is a launcher that injects the widescreen projection, the
control scheme, the HUD scaling and the audio-timing fix into the running process, and drives
the controller through Xidi. See [`docs/`](docs) for the technical write-up.

## Building from source

The launcher and installer are built with PyInstaller:

```
pip install pyinstaller pillow frida
build\build_gui.bat        # builds MML Launcher.exe
build\build_installer.bat  # builds MML-Setup.exe
```

Regenerating the executable patch data (`src/patch_manifest.py`) additionally requires a
vanilla and a patched copy of `MEGAMAN.EXE`; see `src/make_patcher.py`. The redistributable
wrapper DLLs (Xidi, DDrawCompat, DSOAL) are obtained separately — see
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

## Credits and third-party components

This project redistributes the following open-source components, unmodified. Full licenses
are in [`licenses/`](licenses); attribution and sources are in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

- **Xidi** (XInput→DirectInput) — BSD-3-Clause
- **DDrawCompat** (DirectDraw compatibility) — 0BSD
- **DSOAL** + **OpenAL Soft** (DirectSound3D audio) — LGPL-2.1

The controller setup builds on the Xidi configuration from the community *Mega Man Legends
PC Fix* repack by **KuroRaiden** (@NC_KuroRaiden).

## License

The Modernization Project's own code is released under the [MIT License](LICENSE). The
bundled third-party components and the game itself are under their respective licenses.

## Known issues

- Some floor geometry can disappear when the camera is very close to the ground.
- A few in-game menu elements and dialogue borders are not perfectly un-stretched.
- The game is fixed-timestep, so it runs at its native 30 fps; higher frame rates are not
  supported.
