# Fresh-machine test checklist

What to run through when testing a release on a computer that has never seen the project.
The goal is to catch anything that only works on the dev machine.

## Before you start

- Use the real release: download `MML-Setup.exe` from the GitHub release page on that
  machine, don't copy a local build over. The download itself is part of the test
  (SmartScreen/Defender behavior included).
- Note the machine's specs: Windows version, GPU, monitor resolution and aspect ratio,
  controller model if any.

## Install

- [ ] Browser and Windows let you download and run the exe (note any SmartScreen or
      Defender warnings and what it took to get past them).
- [ ] The wizard finds the game folder on its own, or browsing to it works.
- [ ] Install completes; `MEGAMAN.EXE.vanilla-backup` exists afterwards.
- [ ] Pointing it at a wrong folder (no game) is refused cleanly.
- [ ] Re-running the installer over an existing install works (idempotent, no errors).
- [ ] If the machine had another fix pack in the game folder: its wrapper DLLs got moved to
      `Modernization\quarantined-wrappers`, not deleted.

## Launcher

- [ ] `MML Launcher.exe` opens (first launch can take a few seconds, it self-extracts).
- [ ] Status card shows the game as patched and detects the controller when one is plugged.
- [ ] Settings persist after closing and reopening the launcher.
- [ ] "Make a one-click Play shortcut" creates a desktop shortcut that boots the game
      directly.
- [ ] `Modernization\launcher.log` is written and its `RUNNER_BUILD` line matches the
      release you're testing.

## In game, 16:9 + modern controls

- [ ] Game boots to the intro; any button skips it; "Skip entirely" in the launcher skips it
      without input.
- [ ] Widescreen fills the monitor with no stretching (HUD proportions look right).
- [ ] HP and special-weapon bars sit out toward the screen edges.
- [ ] Orbit camera: F5 and L3/R3 toggle it, right stick orbits and holds position.
- [ ] Movement is camera-relative, dodge roll goes in the movement direction (X).
- [ ] LT aims, RT shoots, aim and camera stay together.
- [ ] D-pad navigates menus.
- [ ] A boss fight: the boss bar stays in one piece, fill inside its frame, and drains
      normally.
- [ ] A long cutscene: audio stays in sync (or note how far it drifts).

## In game, alternates

- [ ] 4:3 Classic mode: pillarboxed, original framing, no widescreen artifacts.
- [ ] Vanilla controls mode: plays like the stock game, orbit toggle still available.
- [ ] A couple of resolution options (x1 and Display at least) and Nearest upscaling.
- [ ] Keyboard-only play works for the vanilla layout if no controller is around.

## If something breaks

Grab `Modernization\launcher.log` from that session, screenshot the problem if it's
visual, and note the machine specs. That's everything needed to debug it.
