# Mega Man Legends Modernization Project

For years I wanted to play this game with a more "modern" feel, so using IDA Pro and a
bunch of Python and Frida patches I hacked some systems of the 2001 PC port to build this.
It adds real widescreen, a modern third person camera, a modern controller layout, an
audio fix and a few other things, on top of the original game. No emulator, no recompile,
no source port. It's the original engine with its arm twisted until it behaves.

The project patches your own copy of the game. It doesn't contain any Capcom code or
assets, and it will never give you the game.

## Table of contents

- [Features](#features)
- [What you need](#what-you-need)
- [Installing](#installing)
- [Playing](#playing)
- [Controls](#controls)
- [Uninstalling](#uninstalling)
- [Known issues](#known-issues)
- [Planned additions](#planned-additions)
- [Reporting bugs](#reporting-bugs)
- [How it works](#how-it-works)
- [Building from source](#building-from-source)
- [Credits and third-party components](#credits-and-third-party-components)
- [License](#license)

## Features

### Camera and movement

- **A modern third person camera.** The right stick orbits the camera freely and it stays
  where you leave it, like any game made this century. Toggle it any time with F5 or by
  clicking L3/R3, even mid-cutscene (that wasn't planned, but it's fun, so it stays).
- **Camera-relative movement.** With the modern layout the left stick moves Mega Man
  relative to where the camera points, so goodbye tank controls. Push up, he goes where
  you're looking.
- **Directional dodge rolls, a la Dark Souls.** The vanilla roll could only go left or
  right with a weird button combo; this one rolls in whatever direction you're moving,
  just by pressing X.

### Controls

- **A modern controller layout** closer to a modern third person shooter: LT to aim, RT
  to shoot, X to dodge, right stick for the camera. XInput controllers only for now.
- **The D-pad works everywhere.** It mirrors the left stick, so you can navigate menus
  and walk with it like the PS1 original.
- **Vanilla mode.** Pick "Vanilla (1997)" in the launcher and every control hook stays
  off. You get the stock game with just the widescreen and timing fixes, and you can
  still toggle the orbit camera on top if you want.

### Display

- **True 16:9 widescreen.** Not a stretch and not a hack that only widens the camera: the
  projection is anamorphic and the render area is extended, so you get a wider field of
  view and farther draw distances. A 4:3 Classic mode is also there if you want the
  original framing.
- **HUD and UI un-stretching.** The 2D overlay (health bars, dialogue boxes, text, map)
  is drawn at correct proportions in widescreen instead of getting fat, and the HP and
  special-weapon bars sit out toward the screen edges where a modern game would put them.
- **Higher resolutions** through DDrawCompat, from x1 (640x480) up to x5 (3200x2400),
  plus a "Display" option that renders at your monitor's resolution.
- **Filtering and antialiasing options:** bilinear or nearest-neighbor upscaling,
  trilinear or 16x anisotropic texture filtering, and up to 8x MSAA.

### Audio and extras

- **An audio desync fix** for the cutscene drift that happens on modern hardware. The
  game's timing code loses precision the longer your PC has been on; this fixes the
  timer at the source. It's not frame-perfect yet but it's a lot better than vanilla.
- **A new intro video.** The original `CAPCOM.AVI` is kept next to it, and the launcher
  can skip the intro entirely if you prefer.

Everything except a small, hash-verified executable patch is applied at runtime when you
hit Play, so the game files stay close to stock and it's easy to undo.

## What you need

- A legit copy of the **Mega Man Legends PC port** (the 2001 Capcom CD-ROM release).
  Again, this project does not include the game.
- **Windows 10 or 11** (64-bit).
- An **XInput controller** (Xbox-style) if you want the modern control layout. Keyboard
  still works like it always did for the vanilla layout.

## Installing

1. Download `MML-Setup.exe` from the [latest release](../../releases/latest).
2. Run it and point it at your game folder (it tries to find it on its own first).
3. Click Install. That's it. To play, use `MML Launcher.exe` in the game folder or the
   optional desktop shortcut.

What the installer actually does, in order:

- **Checks your game version by hash.** If your `MEGAMAN.EXE` isn't the exact build the
  patch was made for, it refuses and changes nothing (see
  [Reporting bugs](#reporting-bugs) if that happens to you).
- **Backs up the executable** as `MEGAMAN.EXE.vanilla-backup` before patching, and
  verifies the patched result by hash too. If verification fails it restores the backup.
- **Sets aside conflicting mods.** If it finds wrapper DLLs from other fix packs
  (dgVoodoo, dxwrapper, ReShade and friends), it moves them to
  `Modernization\quarantined-wrappers` instead of deleting them.
- **Copies in the runtime files:** the launcher, the mod runtime, the display/input/audio
  wrappers and their licenses.
- **Installs the new intro,** keeping your original as `CAPCOM.AVI.original`.

The installer is self contained. You don't need Python, Frida or anything else installed.

## Playing

Run `MML Launcher.exe`. Options are remembered between sessions:

| Option | Choices | Notes |
|--------|---------|-------|
| Resolution | x1 (640x480) to x5 (3200x2400), Display | Display = your monitor's resolution, recommended |
| Aspect ratio | 16:9 Widescreen, 4:3 Classic | 4:3 is the original projection, pillarboxed |
| Controls | Modern twin-stick, Vanilla (1997) | Vanilla leaves the game's controls completely stock |
| Intro video | Play (any button skips), Skip entirely | |
| Upscaling | Bilinear, Nearest | Nearest = sharp pixels |
| Texture filter | Game default, Trilinear, Anisotropic 16x | |
| Antialiasing | Off, MSAA 4x, MSAA 8x | |

There's also a **"Make a one-click Play shortcut"** button that creates a desktop
shortcut which boots the game directly with your saved settings, no launcher window.

Each session writes a log to `Modernization\launcher.log`. If something misbehaves,
that's the file I'll ask you for.

## Controls

Modern layout, with an XInput controller:

| Input | Action |
|-------|--------|
| Left stick | Move (camera-relative) |
| Right stick | Orbit the camera |
| D-pad | Same as the left stick (menus and movement) |
| LT | Aim (the camera and aim stay together) |
| RT | Shoot |
| X | Dodge roll in your movement direction |
| A | Jump |
| F5 / L3 / R3 click | Toggle the orbit camera on or off |

With the orbit camera off (or in Vanilla mode) the game plays exactly like the original
release, weird 1997 control scheme and all.

## Uninstalling

There's no automated uninstaller yet, but everything is reversible by hand:

1. Rename `MEGAMAN.EXE.vanilla-backup` back to `MEGAMAN.EXE`.
2. Rename `CAPCOM.AVI.original` back to `CAPCOM.AVI`.
3. Delete `MML Launcher.exe`, the `Modernization` folder, `THIRD-PARTY-NOTICES.txt` and
   the `licenses` folder.
4. Delete the wrapper DLLs the project added: `dinput.dll`, `ddraw.dll`, `dsound.dll`
   and `dsoal-aldrv.dll`. If you had your own wrappers before, they're waiting in
   `Modernization\quarantined-wrappers`, so move them back first.

## Known issues

Things that are broken right now and that I know about:

- **Holes in the ground at certain camera angles.** When the camera gets close to the
  floor (looking down with the orbit camera, or talking to characters at certain angles),
  some terrain triangles drop out and you can see through the level. It's a culling bug
  in the extended draw distance and it's the top of the fix list.
- **Some cutscenes show black or missing textures.** A few in-engine cutscenes have
  elements that render black or don't render at all in widescreen.
- **The pause/item menu isn't fully un-stretched.** It renders through a separate path
  the HUD fix doesn't reach yet, so some bars and grids in there still look wide.
- **Some dialogue box right borders get cropped** in widescreen.
- **Camera collision is rough in tight spaces** with the extended widescreen view.
- **The orbit camera works during menus and cutscenes.** Technically a bug, kept as a
  feature for now. If a cutscene looks weird, you probably left the orbit on; F5 turns
  it off.
- **Audio sync isn't frame-perfect** in long cutscenes yet, just much closer than vanilla.

## Planned additions

Roughly in the order I want to tackle them:

- Fixing the floor holes and the cutscene texture dropouts (the two big known issues).
- Finishing the audio sync work.
- Un-stretching the remaining menus.
- DualShock/DirectInput controller support.
- Mouse and keyboard for the modern layout (the mouse-look part is genuinely hard with
  how this engine handles input, but I want it).
- Android support through Windows emulation layers (GameHub, Winlator, GameNative).

And one non-plan: **60 fps isn't happening.** Every approach I tried ran the game at 2x
speed; the engine ties everything to its frame tick and it would basically need a physics
rewrite. Even interpolation wasn't feasible with its limitations.

## Reporting bugs

Open an [issue](../../issues) and include:

- `Modernization\launcher.log` from the session where it happened (it includes a build
  tag line that tells me exactly what you were running).
- What you were doing, and a screenshot if it's visual.
- If the installer refused your game version: the hash it printed. There are other CD
  builds of this port out there (US/EU/JP variants) and I can only verify the ones I
  know about, so those hashes genuinely help.

## How it works

Short version, for the curious:

- **One small static patch** to `MEGAMAN.EXE` relocates two renderer tables that overflow
  when the draw distance is extended, and fixes the culling math around them. It's a few
  dozen byte patches, hash-verified against the exact game build, with a backup kept.
- **Everything else is runtime.** The launcher starts the game and injects the rest with
  Frida: the widescreen projection, the HUD un-stretch, the camera, the movement and
  input remapping, and the timing fix. Close the game and nothing persists; the mod
  re-arms on every launch.
- **The compatibility layer** is standard, well-tested wrappers: DDrawCompat for
  DirectDraw and resolution handling, Xidi for controller input, DSOAL + OpenAL Soft for
  the game's DirectSound3D audio.

The upside of this design is that your install stays essentially stock and every piece
can be turned off. The downside is that it's Windows-only by nature.

## Building from source

The launcher and installer are built with PyInstaller:

```
pip install pyinstaller pillow frida
set "MML_GAME_DIR=C:\path\to\your\game\folder"
build\build_release.bat
```

That builds `dist\MML-Setup.exe`. `MML_GAME_DIR` only points at the wrapper DLLs and the
intro so they can go in the package; nothing from the game is stored in the repo. See
[docs/RELEASING.md](docs/RELEASING.md) for how updates and releases work.

## Credits and third-party components

This project ships these open-source components as-is. Full licenses are in
[licenses/](licenses); sources and attribution are in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

- **[Xidi](https://github.com/samuelgr/Xidi)** (XInput to DirectInput), BSD-3-Clause
- **[DDrawCompat](https://github.com/narzoul/DDrawCompat)** (DirectDraw compatibility), 0BSD
- **[DSOAL](https://github.com/kcat/dsoal)** and **[OpenAL Soft](https://github.com/kcat/openal-soft)** (DirectSound3D audio), LGPL-2.1

The controller setup builds on the Xidi config from KuroRaiden's *Mega Man Legends PC
Fix* repack (@NC_KuroRaiden). Thanks!

## License

The project's own code is MIT (see [LICENSE](LICENSE)). The bundled components and the
game itself are under their own licenses.
