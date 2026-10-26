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

**Next up:** the 2.0 wishlist.

**The 2.0 wishlist:**

- Distance fog options: original, light or off (done)
- No more low-detail model swaps (LOD) popping in, without characters losing parts far
  away (done)
- Better modern controls
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
