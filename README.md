# Mega Man Legends Modernization Project

For years I wanted to play this game with a more "modern" feel, so using IDA Pro and a
bunch of Python and Frida patches I hacked some systems of the 2001 PC port to build this.
It adds real widescreen, a modern third person camera, a modern controller layout, an audio
fix and a few other things, on top of the original game. No emulator, no recompile.

It patches your own copy of the game. It doesn't contain any Capcom code or assets.

## What it does

- **A modern third person camera** decoupled from player movement, so goodbye to tank
  controls. It also lets you orbit the camera during cutscenes (that wasn't planned, but you
  can toggle it on and off whenever you want).

- **Directional dodge rolls, a la Dark Souls.** The vanilla roll could only go left or right
  with a weird button combo; this one rolls in whatever direction you're facing or walking,
  just by pressing X.

- **True 16:9 widescreen.** It doesn't only widen the camera, it also extends the render area
  so you get wider and farther draw distances. It's still a little buggy and there are some
  camera collision issues in tight spaces.

- **More resolutions** through DDrawCompat, from x1 (640x480) up to x5 (3200x2400), plus an
  option to render at your monitor's resolution, with selectable filtering and antialiasing.

- **A modern controller layout** closer to a modern third person shooter (LT to aim, RT to
  shoot, X to dodge, right stick for the camera). You can toggle it in-game with L3/R3 to go
  back to the vanilla layout on the fly. XInput controllers only for now.

- **An audio desync fix** for the cutscene drift that happens on modern hardware. It's not
  perfect but it's a lot better than vanilla.

- **HUD and UI un-stretching** so the 2D overlay isn't fat in widescreen.

Everything except a small executable patch is applied at runtime, so the game files stay
close to stock and it's easy to undo.

## What you need

- A legit copy of the Mega Man Legends PC port (the 2001 Capcom CD-ROM release). This
  project does not give you the game.
- Windows 10 or 11 (64-bit).

## Installing

Grab `MML-Setup.exe` from the latest release, run it, point it at your game folder and click
Install. It checks the exe, backs it up, patches it and drops in the runtime files. There's
nothing else to install; the launcher is self contained.

To play, use `MML Launcher.exe` in the game folder (or the optional desktop shortcut). The
launcher lets you pick resolution, aspect ratio, controls, filtering and the intro each time,
and it can make a one-click "play with these settings" shortcut.

## Controls (modern layout)

| Input | Action |
|-------|--------|
| Left stick | Move (relative to the camera) |
| Right stick | Orbit the camera |
| D-pad | Same as the left stick (menus and movement) |
| LT / RT | Aim / Shoot |
| X / A | Dodge roll / Jump |
| F5 or stick click | Toggle the orbit camera |

## Building from source

The launcher and installer are built with PyInstaller:

```
pip install pyinstaller pillow frida
set "MML_GAME_DIR=C:\path\to\your\game\folder"
build\build_release.bat
```

That builds `dist\MML-Setup.exe`. `MML_GAME_DIR` only points at the wrapper DLLs and the
intro so they can go in the package; nothing from the game is stored in the repo. See
[docs/RELEASING.md](docs/RELEASING.md) for how updates work.

## Credits and third-party stuff

This project ships these open-source components as-is. Full licenses are in
[licenses/](licenses); sources and attribution are in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

- **Xidi** (XInput to DirectInput), BSD-3-Clause
- **DDrawCompat** (DirectDraw compatibility), 0BSD
- **DSOAL** and **OpenAL Soft** (DirectSound3D audio), LGPL-2.1

The controller setup builds on the Xidi config from KuroRaiden's *Mega Man Legends PC Fix*
repack (@NC_KuroRaiden). Thanks!

## License

The project's own code is MIT (see [LICENSE](LICENSE)). The bundled components and the game
itself are under their own licenses.

## Known issues and plans

Still on the list:

- Widescreen draw distance still has some terrain holes when the camera gets close to the
  floor (like talking to Data at certain angles).
- Some in-game menu bits and dialogue borders aren't perfectly un-stretched.
- Better audio latency and sync.
- Dualshock support, and mouse and keyboard (that one's a lot harder).
- Android through Windows emulators (GameHub, Winlator, GameNative).

60 fps isn't happening; every way I tried it ran at 2x speed, and the engine would basically
need a physics rewrite. Even interpolation wasn't really feasible with its limitations.
