# Making an update

The mod is split in two parts, and that decides what you have to rebuild:

- **The runtime script** (`src/launch_widescreen_freecam.py`) holds the widescreen,
  camera, controls, HUD and audio logic. It's a plain file the launcher runs from disk,
  so changing it does NOT need the launcher rebuilt. To test a change, just copy it into
  your game folder's `Modernization` folder (or your working copy) and play.
- **The launcher and installer** (`src/mml_gui.py`, `src/mml_installer.py`) are the
  programs. Changing those needs a rebuild.

The installer bundles a copy of everything, so any change that should ship to users means
rebuilding the installer.

## The short version

After any change:

```
set "MML_GAME_DIR=C:\path\to\your\game\folder"
build\build_release.bat
```

That rebuilds the launcher, re-packages, and rebuilds `dist\MML-Setup.exe`. `MML_GAME_DIR`
is only needed to pull the wrapper DLLs and the intro into the package; the copyrighted
game files never go into the repo.

## Regenerating the exe patch

If you change the executable byte-patches themselves, regenerate the patch data:

```
python tools\make_patcher.py
```

This needs a vanilla `MEGAMAN.EXE.orig` and the patched `MEGAMAN.EXE` in `MML_GAME_DIR`, and
it rewrites `src/patch_manifest.py` (which is what the installer patches with).

## Committing

Keep updates as their own commits so the history reads normally, for example:

```
git add -A
git commit -m "Fix boss health bar drifting in widescreen"
```
