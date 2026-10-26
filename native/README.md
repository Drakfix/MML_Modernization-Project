# mml.dll (native runtime)

This is the Frida-free version of the mod runtime. `MEGAMAN.EXE` gets one extra
import (`mml.dll`), so Windows loads the DLL before the game starts and it
patches the game in place. No Python, no Frida, no launcher process, and it
should run under Wine/Proton too.

Ported so far: widescreen, the GetTickCount/frame-limiter timing fix, intro
skip (keyboard, gamepad or bypass) and all the modern controls with the orbit
indicator.

Widescreen has two modes:

- `native` renders on a real wide surface (854x480 on 16:9). Every draw goes
  through one Direct3D call, so each primitive gets placed on its way to the
  GPU: the 3D world just gets more width, the HUD gauges go to the screen edges,
  backgrounds made for 4:3 get stretched, menus and dialogue stay centered at
  their real proportions. Nothing gets squeezed. Needs DDrawCompat to allow the
  mode (`SupportedResolutions` has to list it).
- `anamorphic` is the old way: render 4:3 with a narrower projection and let
  DDrawCompat stretch it. The HUD stays stretched in this mode.

## Build

Needs Visual Studio Build Tools with the C++ workload (x86 tools).

```
native\build.bat
```

Output is `native\build\mml.dll` (32-bit, static runtime, no redistributables).

## Try it

1. `python tools\patch_add_import.py MEGAMAN.EXE MEGAMAN_NATIVE.EXE` in the game
   folder (works on the already-patched exe; your original stays untouched).
2. Copy `mml.dll` next to it and run `MEGAMAN_NATIVE.EXE`.
3. Settings live in `mml.ini` next to the exe:

```
[video]
aspect=16:9          ; or 4:3
mode=native          ; or anamorphic
hud_edges=1          ; native: push the life/weapon gauges to the screen edges
closer_factor=0.859375

[controls]
mode=modern          ; or vanilla

[general]
intro=play           ; or skip
```

What it did gets written to `mml.log`.

### Our own renderer

`renderer=mml` (the default) replaces DirectDraw entirely: the game's DirectDraw/Direct3D calls
land in `ddshim.c`, our implementation of exactly the parts it uses (worked out
with an API census, `[debug] api_census=1`), which draws through `gfx_d3d11.c`.
It picks the high-performance GPU on laptops, runs as a normal borderless window
(alt-tab and screenshots behave), and renders at your window's resolution.
Extra settings in `[video]`: `windowed=1`, `render_scale=N` (N times 480 lines,
0 = window resolution), `nearest_upscale=1`, `msaa=1/2/4/8`, `anisotropy=16`,
`vsync=0/1`, `no_fog=1`. The intro video is played by Windows (MCI) straight into
the game window, so the shim centres it at 4:3 on the black window.
`renderer=ddrawcompat` hands DirectDraw back to the DDrawCompat wrapper like 0.1 did. `gfx.h` is the backend interface, so another backend
(GLES/Vulkan for Android) can sit behind the same code.

### 60 fps (frame interpolation)

`[video] interp=1` (the launcher's Frame rate option, Modern renderer only). The
game still thinks at 30 frames a second; for every game frame we also show one
in between, with everything halfway between the last two frames. A frame
starts reacting to your input at the same moment as before, it just gets an
extra frame of motion.

- `gte_capture.c` records every vertex the game's software GTE projects, with an
  identity (which code drew it, its model coordinates, how many times that pair
  came up earlier in the frame), its camera-space position and where it landed
  on screen. It also grabs the world-to-camera transform from the terrain.
- `interp.c` (portable) matches those identities with the previous frame.
  Geometry the game builds in screen space around a projected point (grass,
  sprite cards) or that the near clippers rebuilt gets its 3D position back
  from screen position + depth and moves with the camera, plus whatever the
  nearest matched vertex did on top of that.
- `frame_rec.c` (portable, only uses `gfx.h`) records the frame's draw calls while
  they're drawn, replays them into a second target with every vertex blended
  halfway, shows that frame, and shows the game's own frame 16.7 ms later.
- Background layers (the sky) are handled by `bglayer.c`: the game keeps their
  scroll with sub-pixel precision but drew the tiles at whole pixels, so the sky
  moved in steps. We add the dropped fraction back (smooth even at 30 fps,
  `smooth_sky=1`) and blend with the real scroll, which stays right when the
  tile grid wraps around.

The game's frame limiter used to spin a CPU core waiting for the next frame on a
timer that only ticks every 15.6 ms; it now gets a 1 ms clock and sleeps.

`[video] gpu=display` renders on the GPU the monitor is connected to instead of
the fastest one (laptops with two GPUs). On some machines (the test laptop: an
Intel GPU drives the panel, an NVIDIA renders) Windows composites every frame of
any borderless fullscreen window, which shows up as "Desktop Window Manager" GPU
use. `[video] exclusive=1` (the launcher's Display: Exclusive) skips that: the
game takes the screen exclusively while it's in front and goes back to its
borderless window when you alt-tab away. It renders on the monitor's GPU, since
exclusive mode only works there. On the test laptop the desktop compositor went
from 22-38% of the Intel GPU to 0%. The trade-offs are the usual ones for
exclusive fullscreen: switching away (Alt+Tab, the Start menu) makes the game
give the display back, and screen capture and overlay tools can't see it.

Debug: `[debug] interp_census=1`, `interp_mid_only=1` (show only the in-between
frames), `interp_timing=1` (log frame pacing).

### Distance fog

`[video] fog=original|light|off` (the launcher's Distance fog option). The game
uses plain table fog: a colour plus a linear start/end in screen depth per
scene, switched on per draw. The world is drawn well past where the fog maxes
out, so there's real scenery behind it. `light` multiplies start and end by
`fog_distance` (2 by default), `off` keeps fog disabled. `fog.c` does it on the
game's SetRenderState calls, so it works with either renderer. The old
`no_fog=1` still means off.

### Model detail (LOD)

Characters and props carry three versions of their model (full, medium, low)
and six draw paths pick one by screen depth, with the same two thresholds
(700 and 1800). `lod.c` raises both out of reach, so everything always uses the
full model and nothing swaps as it moves away. `[video] lod=original` brings the
swaps back.

Full models far away hit a second limit: their small triangles (heads, hands)
cover less than one PS1 pixel, and the GTE's facing test (NCLIP) works in
320x240 space and truncates to an integer, so those come out as 0, count as
back-facing and get dropped. Characters lost limbs at a distance. With
`lod=full` the truncation keeps the sign (+-1) for any nonzero result, so
tiny front-facing triangles survive; exactly degenerate ones are still culled.

### Terrain near clipping

The game never clips terrain against the camera. Its software GTE flattens any
vertex closer than the near distance onto the near plane, and one behind the
camera also gets depth 0, so the whole polygon ends up with junk depth. The old
driver threw those polygons away, which is where the holes in the floor at low
camera angles came from. `terrain.c` fixes that for both renderers: right after
the game builds a terrain polygon it rebuilds the polygon's vertices in camera
space from the cell's model data, and if one is too close it tags the packet
(a NaN in the depth field pointing at the saved vertices). When the tagged
polygon is drawn it gets clipped at the near plane, with float UVs, and drawn
as triangles. Vertices that didn't need clipping keep the game's own screen
position, so clipped polygons meet their neighbours exactly. `[video]
terrain_clip=0` turns it off.

Dev settings (`[debug]`): `remote_pad=1` lets tools press pad buttons through
shared memory (`tools\dev\drive.ps1` uses it to play the game for screenshot
tests), `census=1` logs what every frame draws, per ordering-table bucket,
`clip_census=1` logs the terrain clipper's numbers (including how far its
rebuilt vertices land from the game's own, which should be 0), and
`clip_near_scale=N` pushes the terrain clip plane N times further out so you can
see the clipping work, and `keep_small_tris=0` turns off the small-triangle fix
for comparisons.

## Layout

- `src/mml.c` entry point, config, anamorphic widescreen, timing, intro bypass
- `src/display.c` native widescreen: wide mode/viewport/clear rect + per-primitive placement in DrawPrimitive
- `src/ddshim.c` DirectDraw 4 / Direct3D 3 implementation (renderer=mml)
- `src/gfx.h`, `src/gfx_d3d11.c`, `shaders/game.hlsl` rendering backend interface + Direct3D 11 backend
- `src/gte_capture.c`, `src/interp.c`, `src/frame_rec.c` 60 fps frame interpolation
- `src/bglayer.c` smooth background layers (the sky)
- `src/fog.c` distance fog options
- `src/lod.c` always full-detail models
- `src/terrain.c` near-plane clipping for terrain (the floor holes)
- `src/apicensus.c` dev tool: counts every DirectDraw/Direct3D call the game makes
- `src/packets.c` which display-list packet and bucket is being drawn (walker hooks, HUD sprite tags, deferred draws) + the census
- `src/controls.c` orbit camera, camera-relative movement, aim, dodge, button remap
- `src/toast.c` the ORBIT ON/OFF indicator, drawn into the game's own HUD list
- `src/hook.c` mid-function hooks (jump to a stub that saves registers, calls C, replays the stolen bytes)
- `src/iat.c` import table patching (used to catch Xidi's XInput lookup)
- `src/game.h` every game address we touch, named like `tools/ghidra/symbols.csv`
