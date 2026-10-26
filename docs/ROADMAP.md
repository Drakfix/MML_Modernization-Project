# Roadmap (2.0 and beyond)

0.1 was a pile of runtime patches injected with Frida. It worked, but it was fragile: the
widescreen was a 4:3 picture stretched sideways, so every HUD piece had to be found and
squeezed back by hand, and anything new on screen could break it. 2.0 is about doing it
properly this time, and eventually getting the game running natively everywhere, Android
included, without emulation.

## Where things stand

**Done (in the repo, version 0.2.0-dev):**

- A proper Ghidra project for the exe (`tools/ghidra/`), with the Watcom calling convention
  set up and the loader's mistakes fixed, so the decompiler output is actually readable.
- `mml.dll`: everything the Frida runner did, rewritten in C and loaded by the game itself
  (one extra import in the exe). No Python, no Frida, no launcher process.
- Real 16:9. The game draws on a wide surface and every primitive gets placed on its way to
  the GPU: more world on the sides, HUD at the screen edges, backgrounds stretched to fill,
  menus and text at their real proportions. Nothing squeezed.
- An installer and launcher without Frida, half the size, that upgrade 0.1 installs in place.
- Our own renderer (now the default, `renderer=mml`): the game's DirectDraw/Direct3D calls go to our
  own small implementation on top of Direct3D 11. Proper borderless window, screenshots and
  alt-tab work, it picks the fast GPU on laptops. The intro video plays centred on it, and
  the launcher has Renderer and Display (fullscreen/windowed) options.
- The floor holes are fixed for real: terrain polygons that reach past the camera now get
  clipped at the near plane like the game already does for objects, instead of thrown away.
- 60 fps: the game still runs at 30, and every frame gets an in-between frame with
  everything halfway (characters, animation, camera, grass, the sky). Launcher: Frame rate.
- The sky scrolls smoothly: the game kept its position with sub-pixel precision but drew it
  in whole pixels, and its walking parallax stalled every few ticks. Both fixed.
- The game no longer spins a CPU core waiting for the next frame.
- Exclusive fullscreen as an option for weaker hardware (Display: Exclusive).

**Next up:** the 2.0 wishlist.

**The 2.0 wishlist:**

- Distance fog options: original, light or off (done)
- No more low-detail model swaps (LOD) popping in, without characters losing parts far
  away (done)
- Better modern controls (in progress)
  - Native controller support through SDL3: Xbox, PlayStation, Switch and most other
    gamepads, no Xidi needed (done)
  - Faster, snappier roll that starts right away, chains, and cancels into a run
    (done)
  - Analog movement: tilt the stick a little to walk, more to run, with the speed
    and animation following the tilt (done)
  - Orbit camera hands control back to the game for conversations and cutscenes,
    stops at walls instead of going through them, and can look up further
    without sinking into the floor (done)
- Mouse + keyboard with the orbit camera
- Fix the known issues
- Better presentation overall

## The long road (native port)

The PC port is basically the PS1 game sitting on a thin layer that fakes the PS1 hardware
(the 3D math, the drawing lists, sound, files). The plan is to replace that layer piece by
piece with our own portable code, then rewrite the game logic itself in C one function at a
time, checking each one against the original while the game keeps running. When nothing of
the original exe is left, it can be built for Linux, Mac and Android. 60 fps also becomes
realistic at that point since we'd own both the game logic and the drawing.

This project will never ship any of the game's files. You'll always need your own copy.
