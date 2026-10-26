# Making an update

The mod is split in two parts, and that decides what you have to rebuild:

- **The runtime** (`native/`, builds `mml.dll`) holds the widescreen, HUD layout, camera,
  controls and timing logic. Rebuild it with `native\build.bat` and copy `mml.dll` next to
  a patched `MEGAMAN.EXE` to test; no launcher rebuild needed. `native/README.md` has the
  details, and `tools\dev\drive.ps1` can play the game by script for screenshot tests.
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

That rebuilds `mml.dll` and the launcher, re-packages, and rebuilds `dist\MML-Setup.exe`. `MML_GAME_DIR`
is only needed to pull the wrapper DLLs and the intro into the package; the copyrighted
game files never go into the repo.

## Regenerating the exe patch

If you change the executable byte-patches themselves, regenerate the patch data:

```
python tools\make_patcher.py
```

This needs a vanilla `MEGAMAN.EXE.orig` and the patched `MEGAMAN.EXE` in `MML_GAME_DIR`, and
it rewrites `src/patch_manifest.py` (which is what the installer patches with). The import
that loads `mml.dll` gets added on top automatically. When a release changes the patch,
add the previous release's patched hash to `PREVIOUS_PATCHED_SHA256` in `make_patcher.py`
so existing installs can upgrade in place.

## Self-testing the installer

Always self-test a rebuilt installer before publishing it. The exe installs headlessly into
any folder you point it at, so make a sandbox with a vanilla `MEGAMAN.EXE` in it and run:

```
MML-Setup.exe --selftest C:\path\to\sandbox
```

Exit code 0 means the install ran; then verify the result actually is the new build:

- the patched `MEGAMAN.EXE` in the sandbox hashes to `PATCHED_SHA256` from
  `src/patch_manifest.py`, and
- the installed `mml.dll` is the one you just built (same hash as `native\build\mml.dll`).

Also self-test an upgrade: a sandbox with the previous release's patched exe should land on
the same `PATCHED_SHA256`. Bump `MML_NATIVE_BUILD` in `native/src/mml.c` on every runtime
change; it's the first line of `mml.log`, so a log from any machine tells you what ran.

## Committing

Keep updates as their own commits so the history reads normally, for example:

```
git add -A
git commit -m "Fix boss health bar drifting in widescreen"
```

## Publishing a release

1. Bump `VERSION` in `src/mml_gui.py` and `src/mml_installer.py` (keep them in sync) and
   `MML_NATIVE_BUILD` in `native/src/mml.c`.
2. Rebuild and self-test as above.
3. Tag and push:

   ```
   git tag -a v0.x.y -m "v0.x.y - short summary"
   git push origin main --tags
   ```

4. On GitHub: Releases, "Draft a new release", pick the tag, write the notes and attach
   `dist\MML-Setup.exe`. With the `gh` CLI it's one command instead:

   ```
   gh release create v0.x.y dist\MML-Setup.exe --title "..." --notes-file notes.md
   ```

Release notes that work: what changed in one or two sentences, the three-step install, a
reminder that a fresh unsigned exe can trip browser/Defender warnings, and the current top
known issues so they don't get re-reported.
