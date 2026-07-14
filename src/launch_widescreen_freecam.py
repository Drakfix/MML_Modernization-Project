"""
Runtime launcher for the Mega Man Legends Modernization Project.

Spawns MEGAMAN.EXE and injects the runtime portion of the mod with Frida. (The static
executable byte-patches are applied separately by the installer and live in the exe.)

Provides:
  - GetTickCount rebase + frame-limiter drift fix (cutscene audio-timing)
  - anamorphic 16:9 widescreen projection
  - HUD, dialogue and menu un-stretch for widescreen
  - free-orbit camera, camera-relative movement, directional dodge roll
  - twin-stick control remap (LT aim / RT shoot / X dodge) and D-pad-as-left-stick
  - an in-engine orbit on/off indicator

Configuration comes from environment variables:
  MML_ASPECT   '16:9' (default) or '4:3'   - 4:3 uses the game's native projection
  MML_CONTROLS 'modern' (default) or 'vanilla' - vanilla leaves all control hooks inert
  MML_WINDOW   'fullscreen' (default), 'borderless' or 'none'

Usage: python launch_widescreen_freecam.py [closer_factor] [margin_fraction] [yaw_speed] [pitch_speed]

Detaching (Ctrl+C or closing the console) reverts the runtime changes and leaves the game
running; the GetTickCount stub is left installed in game-owned memory so the detach is safe.
"""
import frida
import os
import time
import sys
import math
import ctypes
import threading
from ctypes import wintypes

def _find_game_dir():
    """Locate the game folder: MML_GAME_DIR env override, else walk up from this
    script (dev layout: RE-notes/tools/, release layout: Modernization/), else cwd."""
    env = os.environ.get("MML_GAME_DIR")
    if env and os.path.exists(os.path.join(env, "MEGAMAN.EXE")):
        return os.path.abspath(env)
    p = os.path.dirname(os.path.abspath(__file__))
    for _ in range(4):
        if os.path.exists(os.path.join(p, "MEGAMAN.EXE")):
            return p
        p = os.path.dirname(p)
    if os.path.exists(os.path.join(os.getcwd(), "MEGAMAN.EXE")):
        return os.getcwd()
    raise SystemExit("MEGAMAN.EXE not found - run from the game folder or set MML_GAME_DIR.")


GAME_DIR = _find_game_dir()
GAME_EXE = os.path.join(GAME_DIR, "MEGAMAN.EXE")
INTERNAL_W, INTERNAL_H = 640.0, 480.0

CLOSER_FACTOR = float(sys.argv[1]) if len(sys.argv) > 1 else 0.859375
MARGIN_FRACTION = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
YAW_SPEED = float(sys.argv[3]) if len(sys.argv) > 3 else 28.0
PITCH_SPEED = float(sys.argv[4]) if len(sys.argv) > 4 else 20.0  # angle units per frame at full stick-Y
# Asymmetric pitch clamp (1024 = 90 deg). Looking UP is what clips through the
# floor into the game's unrendered under-geometry, so
# the up travel is limited hard; looking DOWN is harmless so it stays generous.
PITCH_LIMIT_UP = 57       # stick-up / positive pitch (look up) - restricted (clips floor)
PITCH_LIMIT_DOWN = -900   # stick-down / negative pitch (look down) - generous
MOUSE_SPEED = 3.0  # yaw units per pixel of mouse-X (mouse currently disabled, see below)
# Mouse-look via cursor-recenter fights the game's own cursor handling and makes the
# yaw thrash. Disabled until reimplemented with raw input
# (WM_INPUT). Stick orbit is unaffected.
ENABLE_MOUSE = False

user32 = ctypes.windll.user32
user32.FindWindowA.restype = wintypes.HWND
user32.FindWindowA.argtypes = [wintypes.LPCSTR, wintypes.LPCSTR]
user32.GetWindowLongA.restype = ctypes.c_long
user32.GetWindowLongA.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SetWindowLongA.restype = ctypes.c_long
user32.SetWindowLongA.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_long]
user32.SetWindowPos.restype = wintypes.BOOL
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
user32.GetWindowRect.restype = wintypes.BOOL
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.GetForegroundWindow.restype = wintypes.HWND

SM_CXSCREEN, SM_CYSCREEN = 0, 1
WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000
WS_SYSMENU = 0x00080000
WS_MINIMIZEBOX = 0x00020000
WS_MAXIMIZEBOX = 0x00010000
WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
BORDER_BITS = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
SWP_FRAMECHANGED = 0x0020


def get_screen_size():
    return user32.GetSystemMetrics(SM_CXSCREEN), user32.GetSystemMetrics(SM_CYSCREEN)


# Window mode via env vars (set by the .bat):
# MML_WINDOW = 'fullscreen' (default) | 'borderless' (centered, smaller than screen)
# MML_WIN_SCALE = fraction of screen HEIGHT for borderless mode (default 0.85)
# Borderless keeps the SCREEN aspect ratio (just scaled down), so the anamorphic
# RATIO is identical to fullscreen - widescreen stays correct, but the window no
# longer covers everything, so alt-tab / Win+Shift+S screenshots work normally.
# MML_WINDOW: 'fullscreen' (default, borderless-fullscreen reshape) | 'borderless'
# (centered sub-screen reshape - only works when we own the window) | 'none' (an
# external wrapper like dxwrapper owns/sizes the window; we DON'T reshape it).
WINDOW_MODE = os.environ.get("MML_WINDOW", "fullscreen").lower()
WIN_SCALE = float(os.environ.get("MML_WIN_SCALE", "0.85"))
SCR_W, SCR_H = get_screen_size()
if WINDOW_MODE == "none":
 # dxwrapper renders a 16:9 window; RATIO is aspect-based so screen size is fine.
    TARGET_CLIENT_W, TARGET_CLIENT_H = SCR_W, SCR_H
    WIN_X = WIN_Y = 0
elif WINDOW_MODE == "borderless":
    TARGET_CLIENT_H = max(240, int(SCR_H * WIN_SCALE))
    TARGET_CLIENT_W = int(TARGET_CLIENT_H * (SCR_W / SCR_H))  # preserve screen aspect
    WIN_X = (SCR_W - TARGET_CLIENT_W) // 2
    WIN_Y = (SCR_H - TARGET_CLIENT_H) // 2
else:
    TARGET_CLIENT_W = int(SCR_W * MARGIN_FRACTION)
    TARGET_CLIENT_H = int(SCR_H * MARGIN_FRACTION)
    WIN_X = WIN_Y = 0
RATIO = (INTERNAL_W / INTERNAL_H) / (TARGET_CLIENT_W / TARGET_CLIENT_H)
# Front-end selections (set by the chooser / bat). ASPECT 4:3 skips the anamorphic squeeze
# entirely (pure vanilla projection); CONTROLS 'vanilla' leaves the control hooks inert
# (they all gate on the orbit toggle, which we simply never start in vanilla mode).
ASPECT = os.environ.get("MML_ASPECT", "16:9").replace(" ", "")
CONTROLS = os.environ.get("MML_CONTROLS", "modern").strip().lower()
ANAMORPH = 0 if ASPECT in ("4:3", "4x3", "43") else 1
# MML_INTRO = 'play' (default: intro plays, any key/click/pad button skips it) | 'skip'
# (bypass entirely: the 5-byte `call 0x4F9933` at 0x4A6F7E is NOP'd at spawn)
INTRO = os.environ.get("MML_INTRO", "play").strip().lower()
INTRO_SKIP = 1 if INTRO in ("skip", "off", "none", "0") else 0
if not ANAMORPH:
    RATIO = 1.0   # unused when the squeeze is off, but keep the JS format happy
print(f"Window mode: {WINDOW_MODE}  {TARGET_CLIENT_W}x{TARGET_CLIENT_H} @ ({WIN_X},{WIN_Y}); "
      f"aspect {'4:3' if not ANAMORPH else '16:9'} (ratio {RATIO:.4f}); controls {CONTROLS}; "
      f"intro {'BYPASSED' if INTRO_SKIP else 'plays (any input skips)'}")


def make_borderless(hwnd, x, y, target_w, target_h):
 # Always borderless (WS_POPUP). x,y let us CENTER a sub-screen window so it's
 # a normal alt-tab/screenshot-friendly window instead of covering everything.
    style = user32.GetWindowLongA(hwnd, -16) & 0xFFFFFFFF
    new_style = (style & ~BORDER_BITS) | WS_POPUP | WS_VISIBLE
    user32.SetWindowLongA(hwnd, -16, new_style)
    user32.SetWindowPos(hwnd, None, x, y, target_w, target_h, SWP_FRAMECHANGED | 0x4)
    got = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(got))
    return style, new_style, got.right - got.left, got.bottom - got.top


def resize_game_window(x, y, target_w, target_h, find_timeout=15, hold_seconds=10):
    deadline = time.time() + find_timeout
    hwnd = None
    while time.time() < deadline:
        hwnd = user32.FindWindowA(None, b"Megaman")
        if hwnd:
            break
        time.sleep(0.5)
    if not hwnd:
        print("Could not find game window to resize.")
        return False
    kind = "borderless window" if (x or y) else "borderless-fullscreen"
    print(f"Found window {hwnd}, applying {kind} ({target_w}x{target_h} @ {x},{y}) for {hold_seconds}s...")
    end = time.time() + hold_seconds
    last = None
    while time.time() < end:
        _, _, aw, ah = make_borderless(hwnd, x, y, target_w, target_h)
        if (aw, ah) != last:
            last = (aw, ah)
        time.sleep(0.2)
    print(f"Final window size: {last}")
    return True


# ------------------------- XInput -------------------------
class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [("wButtons", wintypes.WORD), ("bLeftTrigger", ctypes.c_ubyte),
                ("bRightTrigger", ctypes.c_ubyte), ("sThumbLX", ctypes.c_short),
                ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short),
                ("sThumbRY", ctypes.c_short)]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [("dwPacketNumber", wintypes.DWORD), ("Gamepad", XINPUT_GAMEPAD)]


def _load_xinput():
    for name in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
        try:
            return ctypes.windll.LoadLibrary(name)
        except OSError:
            continue
    return None


xinput = _load_xinput()
RX_DEADZONE = 7849
XINPUT_LTHUMB = 0x0040  # L3 (left stick click)
XINPUT_RTHUMB = 0x0080  # R3 (right stick click) - either toggles the orbit
XINPUT_LB = 0x0100      # LB = vanilla AIM button (kept as a fallback)
AIM_BUTTON = XINPUT_LB  # while held, orbit override is suspended so aim behaves normally
AIM_TRIG_THRESH = 40    # LT (left trigger) is the primary AIM button now (>this = aiming)
VK_F5 = 0x74

# ---- camera-relative movement ----
# When orbit is engaged, the LEFT stick moves the character relative to the orbit
# camera (push toward camera-forward -> walk that way), replacing tank steering;
# the RIGHT stick orbits. We set the real facing 0x5F83B8 to the stick direction
# taken relative to the orbit yaw, then feed "forward" via the XInput hook.
ENABLE_CAMREL_MOVE = True
MOVE_DEBUG = False  # roll/movement diagnostic ([roll] lines); flip True to debug the dodge
CAM_DEBUG = False   # once/sec [camdbg] funcA/funcB call counts + caller histogram; debug only
MOVE_DEADZONE = 7849
# The game rate-limits how fast the character turns, so on a sharp turn/reversal it
# would coast forward while still swinging around. We ease the forward speed down by
# how far the facing still has to turn (dot product), so reversals pivot ~in place
# instead of coasting. MIN_MOVE_SCALE = the slowest it goes mid-turn (0 = full stop
# while turning, 1 = no easing). Lower = snappier reversals but more "stop to turn".
MIN_MOVE_SCALE = 0.2
# facing = (orbit_yaw + FACE_OFFSET + FACE_SIGN * stick_dir) & 0xffff, in game units
# (4096 = 360deg). These two are calibrated live: push UP and tune FACE_OFFSET so the
# character walks AWAY from the camera; flip FACE_SIGN if left/right is mirrored.
FACE_OFFSET = 2048   # +180deg: stick-up = AWAY from camera (was inverted at 0)
FACE_SIGN = 1
# Rate-limit how fast the character turns toward the stick direction (game angle
# units per frame; 4096 = 360deg). 0 = instant snap (the good, responsive feel).
# Smoothing (>0) caused a walk-back-and-forth oscillation when moving toward the
# camera, so it's OFF. The real cure for the opposite-stick thrash is decoupling the
# camera into a true orbit, not smoothing the facing.
TURN_RATE = 0
# print raw pad state once/sec so we can identify
# which button = AIM and whether aim/menu/cutscene change the funcA caller.
# Set False once gating is wired up.
DEBUG_PAD = False   # [pad] button-name logger; flip True to map controller buttons
XINPUT_BUTTON_NAMES = {
    0x0001: "DPAD_UP", 0x0002: "DPAD_DOWN", 0x0004: "DPAD_LEFT", 0x0008: "DPAD_RIGHT",
    0x0010: "START", 0x0020: "BACK", 0x0040: "L3", 0x0080: "R3",
    0x0100: "LB", 0x0200: "RB", 0x1000: "A", 0x2000: "B", 0x4000: "X", 0x8000: "Y",
}

JS = r"""
'use strict';

// ---- persistent allocations ----
// VirtualAlloc'd memory SURVIVES a Frida detach; Memory.alloc does NOT (it is freed
// with the script, and anything the GAME still references afterwards would crash it).
// Every allocation the game itself reads/executes after we detach must come from here.
var _k32 = Process.getModuleByName('kernel32.dll');
var _VirtualAlloc = new NativeFunction(_k32.getExportByName('VirtualAlloc'),
    'pointer', ['pointer', 'ulong', 'uint32', 'uint32']);
function persistAlloc(size) {
    var p = _VirtualAlloc(NULL, size, 0x3000, 0x40 /*PAGE_EXECUTE_READWRITE*/);
    if (p.isNull()) throw new Error('VirtualAlloc failed');
    return p;
}

// ===== GetTickCount rebase =====
(function rebaseTickCount() {
    var blk = persistAlloc(64);
    var iat = ptr('0x508328');
    var orig = iat.readPointer();
    blk.add(32).writePointer(orig);
    var realTick = new NativeFunction(orig, 'uint32', []);
    blk.add(40).writeU32(realTick());
    var code = [0xFF, 0x15], p32 = blk.add(32), p40 = blk.add(40);
    function abs4(p){ var v=p.toUInt32(); return [v&0xFF,(v>>>8)&0xFF,(v>>>16)&0xFF,(v>>>24)&0xFF]; }
    code = code.concat(abs4(p32), [0x2B, 0x05], abs4(p40), [0xC3]);
    blk.writeByteArray(code);
    Memory.protect(iat, 4, 'rw-');
    iat.writePointer(blk);
    send({type:'info', msg:'GetTickCount rebased (60Hz timing precise).'});
})();

// ===== Frame-limiter float32 drift fix =====
// The limiter's threshold at 0x55BDB4 is a FLOAT32 the game advances each frame by
// step*16.667ms (fstp dword, §4). The GetTickCount rebase makes it start near 0, but
// once a single SESSION passes 2^23 ms (~2.33h) the float's ulp reaches 1ms and the
// += quantizes -> the residual mild drift seen in marathon sessions. Fix: mirror the
// accumulation in a float64 and rewrite the float32 threshold from it once per frame
// (hook = the proven once-per-frame walker call 0x4A70E5, same thread as the limiter,
// so there's no race). Store rounding then stays BOUNDED (<1 ulp) instead of
// accumulating -> exact 60Hz tick pacing for unlimited session lengths. If the game
// resets/rewrites the threshold out from under us (level load, step change mid-frame),
// the 8ms tolerance resyncs the accumulator to the game's value.
(function fixLimiterDrift() {
    var TH = ptr('0x55BDB4');                  // limiter threshold (float32, ms)
    var STEP = ptr('0x77EAC1');                // frame-step byte (2 normal, 4 heavy)
    var D16 = ptr('0x50A398').readDouble();    // the game's own ms-per-step double (1000/60)
    var acc = null;
    Interceptor.attach(ptr('0x4A70E5'), { onEnter: function () {
        try {
            var game = TH.readFloat();
            if (acc === null) { acc = game; return; }
            var expected = acc + STEP.readU8() * D16;
            acc = (Math.abs(game - expected) > 8.0) ? game : expected;
            TH.writeFloat(acc);
        } catch (e) { /* threshold page unavailable this frame - skip */ }
    }});
    send({type:'info', msg:'Frame-limiter drift fix ON (float64 threshold).'});
})();

// ===== CAPCOM.AVI intro skip =====
// The intro is played via mciSendStringA ('open avivideo!CAPCOM.AVI alias vfw' / 'play vfw
// window from 0 notify') inside 0x4F9933, which pumps messages while the 'playing' flag
// byte 0x551FA8 is set. Its own subclassed WndProc (0x4F985F) ALREADY clears that flag on
// any WM_KEYDOWN / mouse click (vanilla keyboard skip!), and the pump's exit path does
// 'stop vfw wait' + 'close vfw wait' itself. Gamepads don't send window messages, so the
// Python side polls XInput during startup and calls introskip() - one byte, the game's own
// graceful stop path does the rest.
rpc.exports.introstate = function () { return ptr('0x551fa8').readU8(); };
rpc.exports.introskip = function () { ptr('0x551fa8').writeU8(0); };
// Full BYPASS option (MML_INTRO=skip): NOP the single `call 0x4F9933` at 0x4A6F7E at
// spawn - the game goes straight from window creation to the title. The CD check stays
// satisfied (CAPCOM.AVI still on disk); dispose() restores the call bytes.
var INTRO_SKIP = %d;
var INTRO_CALL = [0xE8, 0xB0, 0x29, 0x05, 0x00];   // call 0x4F9933
if (INTRO_SKIP) {
    var ic = ptr('0x4A6F7E');
    var cur = new Uint8Array(ic.readByteArray(5));
    var match = true;
    for (var ii = 0; ii < 5; ii++) if (cur[ii] !== INTRO_CALL[ii]) match = false;
    if (match) {
        Memory.protect(ic, 5, 'rwx');
        ic.writeByteArray([0x90, 0x90, 0x90, 0x90, 0x90]);
        send({type:'info', msg:'Intro video: BYPASSED.'});
    } else {
        send({type:'info', msg:'Intro bypass: unexpected bytes at 0x4A6F7E - left vanilla.'});
    }
}

// ===== Anamorphic widescreen - installed only when ANAMORPH (16:9). =====
// In 4:3 mode we skip it entirely so the projection is fully vanilla (DDrawCompat handles
// the 4:3 pillarbox), and the render stays sharp without any horizontal squeeze.
var HY_ADDR = ptr('0x77ebdc');
var CLOSER_FACTOR = %f;
var RATIO = %f;
var ANAMORPH = %d;
var CAM_DEBUG = %d;
// PERSISTENT (persistAlloc, not Memory.alloc): five game instructions get their
// operand byte-patched to point HERE, and raw byte-patches outlive a detach - with
// Memory.alloc the game would read freed memory and crash seconds after Ctrl+C
// (found in the 0.1 pre-ship review). dispose() below also restores the operands
// to the vanilla 0x77ebdc on unload, so a detached game runs fully vanilla.
var shadowX = persistAlloc(4);
function applyAdjustment() {
    var naturalHy = HY_ADDR.readFloat();
    var adjustedHy = naturalHy * CLOSER_FACTOR;
    HY_ADDR.writeFloat(adjustedHy);
    shadowX.writeFloat(adjustedHy * RATIO);
}
if (ANAMORPH) {
    var xSites = [0x4eeeed, 0x4eef21, 0x4eef8d, 0x4f0685, 0x4f07ee];
    Memory.protect(ptr(0x4ee000), 0x3000, 'rwx');
    var shadowNum = uint64(shadowX.toString());
    for (var i = 0; i < xSites.length; i++) ptr(xSites[i]).add(2).writeU32(shadowNum.toNumber());
    Interceptor.attach(ptr('0x4ef097'), { onEnter: function(){ applyAdjustment(); } });
    Interceptor.attach(ptr('0x4ef0e1'), { onEnter: function(){ applyAdjustment(); } });
    send({type:'info', msg:'Anamorphic widescreen ON.'});
} else {
    send({type:'info', msg:'4:3 mode: anamorphic squeeze OFF (vanilla projection).'});
}

// ===== HUD / DIALOGUE UN-STRETCH =====
// The anamorphic squeeze only widens the 3D projection; the 2D HUD/dialogue is drawn in
// screen space and would otherwise look horizontally FAT. Once per frame (before the walker
// draws it) we walk the two HUD overlay buckets (ctx+0x78 = HUD, ctx+0x7c = dialogue) and
// squeeze each 2D sprite's X about screen-centre 160 by 0.75 (= the same ratio the 3D gets),
// so it matches the widescreen. Text glyphs (opcode 0x6c) also get their hard-coded draw
// width narrowed (handler 0x4F6373 'add eax,0xe' -> 11). Only the CORE proven packet types
// (0x2c/0x2e/0x38 textured quads, 0x2a flat quad, 0x6c text) are touched; other types /
// the in-game menu grid (separate render path) are left vanilla
// Gated on ANAMORPH: in 4:3 mode the HUD isn't stretched, so we leave it alone.
if (ANAMORPH) {
    var HU_CTXP = ptr('0x77EA74');
 // HU_WIDTH = RATIO, the same display-derived factor the 3D squeeze uses (exactly
 // 0.75 on a 16:9 monitor). Hardcoding 0.75 here mismatched the HUD on 16:10 /
 // ultrawide displays where the 3D adapts. (Edge-push/toast offsets stay 16:9-tuned.)
    var HU_WIDTH = RATIO, HU_CENTER = 160, HU_GLYPHW = 11, HU_FSX = 300, HU_FSY = 180;
    var HU_XA = [0x8,0x10,0x18,0x20], HU_YA = [0xA,0x12,0x1A,0x22];   // 4-vert, 8-byte stride
    var HU_XB = [0x8,0xC,0x10,0x14],  HU_YB = [0xA,0xE,0x12,0x16];    // 4-vert, 4-byte stride
    var HU_TYPES = {0x2c:[HU_XA,HU_YA], 0x2e:[HU_XA,HU_YA], 0x38:[HU_XA,HU_YA], 0x2a:[HU_XB,HU_YB]};
    var HU_BUCKETS = [0x78, 0x7c];
 // The TITLE screen uses a 640-wide space (centre 320) while gameplay is 320-wide (centre 160).
 // Detect per-frame from the widest sprite (title reaches ~576, gameplay stays under ~304) and
 // squeeze about the right centre, so the title menu text is centred too. 1-frame lag = fine.
    var HU_TITLE_THRESH = 400, HU_TITLE_MID = 240, HU_TITLE_CENTER = 320;
    var HU_lastMaxX = 0, HU_lastMinX = 0, HU_fontLocked = 0;
    var HU_GW = ptr('0x4F6375'); Memory.protect(HU_GW, 1, 'rwx'); HU_GW.writeU8(HU_GLYPHW);
 // 0x64 sprite-rect state: shared center for the staged-vertex hook +
 // per-texture-metric footprint stats for the backdrop classifier (skybox / menu
 // wallpaper tile sets are detected by their COLLECTIVE span and left stretched).
    var HU_curCenter = HU_CENTER;
    var HU_mstat = {}, HU_mblack = {};
    Interceptor.attach(ptr('0x4A70E5'), { onEnter: function () {
        try {
            var ctx = HU_CTXP.readPointer();
            // The title screen sits centered in the 640-wide space (its content is around
            // X=320), while gameplay stays centered near 160 even when a wide boss HUD reaches
            // far to the right. So detect the title by where the content is CENTERED, not just
            // how far right it reaches - a boss bar out at X~444 must NOT read as the title.
            var lastMid = (HU_lastMinX + HU_lastMaxX) / 2;
            var center = (HU_lastMaxX > HU_TITLE_THRESH && lastMid > HU_TITLE_MID) ? HU_TITLE_CENTER : HU_CENTER;
            HU_curCenter = center;
            // Classify last frame's 0x64 metrics: screen-covering chunky tile sets are
            // backdrops (skybox / menu wallpaper) and stay stretched full-screen.
            HU_mblack = {};
            for (var mk in HU_mstat) {
                var ms = HU_mstat[mk];
                if ((ms[1] - ms[0]) >= 300 && (ms[4] >= 6 || (ms[3] - ms[2]) >= 200) && ms[5] > 20) HU_mblack[mk] = 1;
            }
            HU_mstat = {};
            var fmax = -99999, fmin = 99999;
            for (var b = 0; b < HU_BUCKETS.length; b++) {
                var p = ctx.add(HU_BUCKETS[b]).readPointer(), n = 0;
                while ((p.toUInt32() & 1) === 0 && n < 8000) {
                    n++;
                    var op = p.add(7).readU8();
                    if (op === 0x6c) {                       // TEXT glyph: single X @ +8
                        if (!HU_fontLocked) { FONT_METRIC = p.add(0xe).readU16(); HU_fontLocked = 1; }  // real font page for our toast
                        var gx = p.add(8).readU16(); if (gx & 0x8000) gx -= 0x10000;
                        if (gx > fmax) fmax = gx; if (gx < fmin) fmin = gx;
                        p.add(8).writeU16((Math.round(center + (gx - center) * HU_WIDTH)) & 0xffff);
                    } else if (op === 0x64) {
 // tracked for the title-center pick only; the actual 0x64 transform
 // happens in the staged-vertex hook below (packet edits can only
 // crop or cram - U-span = width field)
                        var rx = p.add(8).readU16(); if (rx & 0x8000) rx -= 0x10000;
                        var rw = p.add(0x10).readU16();
                        if (rx >= -320 && rx <= 704 && rw <= 640) { if (rx + rw > fmax) fmax = rx + rw; if (rx < fmin) fmin = rx; }
                    } else {
                        var t = HU_TYPES[op];
                        if (t) {
                            var xoff = t[0], yoff = t[1], nv = xoff.length;
                            var xs = [], xlo = 99999, xhi = -99999, ylo = 99999, yhi = -99999;
                            for (var i = 0; i < nv; i++) {
                                var x = p.add(xoff[i]).readU16(); if (x & 0x8000) x -= 0x10000;
                                var y = p.add(yoff[i]).readU16(); if (y & 0x8000) y -= 0x10000;
                                xs[i] = x; if (x<xlo)xlo=x; if (x>xhi)xhi=x; if (y<ylo)ylo=y; if (y>yhi)yhi=y;
                            }
                            if (!((xhi-xlo) >= HU_FSX && (yhi-ylo) >= HU_FSY)) {   // skip fullscreen bg/fade
                                if (xhi > fmax) fmax = xhi; if (xlo < fmin) fmin = xlo;
                                for (var j = 0; j < nv; j++)
                                    p.add(xoff[j]).writeU16((Math.round(center + (xs[j]-center)*HU_WIDTH)) & 0xffff);
                            }
                        }
                    }
                    p = p.readPointer();   // next @ +0
                }
            }
            HU_lastMaxX = fmax; HU_lastMinX = fmin;
        } catch (e) { /* bad list node - skip this frame */ }
    }});
 // Player-HUD EDGE-PUSH: shove the HP bar and special-weapon bar out toward the widescreen
 // edges. Done at the status-draw emitter 0x4364F0 (edx = X arg), which emits ONLY the player
 // HUD - so the boss bar and map (other emitters) are never touched. BAR_L shifts sprites left
 // of center, BAR_R those right of center (the two bars are on opposite sides). User-tuned.
    var HU_BAR_L = -50, HU_BAR_R = 60;
    Interceptor.attach(ptr('0x4364F0'), { onEnter: function () {
        var edx = this.context.edx.toUInt32();
        var x = edx & 0xffff; if (x & 0x8000) x -= 0x10000;
        x += (x < HU_CENTER) ? HU_BAR_L : HU_BAR_R;
        this.context.edx = ptr(((edx & 0xffff0000) | (x & 0xffff)) >>> 0);
    }});
 // ===== 0x64 SPRITE-RECT un-stretch (menus/UI: item grids, bar fills, borders) =====
 // Handler 0x4F608A (dispatch stub 0x4F7A1B) = textured sprite RECT: X s16 @ +8,
 // Y @ +0xA, U/V bytes @ +0xC/+0xD, WIDTH @ +0x10, HEIGHT @ +0x12 - a 1:1 texel
 // blit, so packet edits can only CROP (U-span = width) or CRAM (reposition).
 // TRUE compression = hook 0x4F6288 (inside the handler, after its 4 XYZRHW verts
 // are staged at 0x8876C4/+0x20/+0x40/+0x60 with UVs already spanning the full
 // texture; ESI = packet on BOTH inbound paths incl. the text handler's shared-tail
 // weave) and squeeze the 4 staged X floats about the device-space center - the
 // full texture maps onto the narrower quad, like the 3D anamorphic squeeze.
 // Gates: [esi+7] must be EXACTLY 0x64 (not 0x6c text - double-transform; not 0x66
 // world/cutscene tiles); per-metric backdrop classifier (HU_mblack, built each
 // frame above) leaves skybox/menu-wallpaper tile sets stretched (full-bleed, no
 // black side bars); per-tile sanity cap w<=288/h<=240 keeps skinny tall borders
 // IN (they must squeeze with the boxes they frame).
    var HU_V64X = [ptr('0x8876C4'), ptr('0x8876E4'), ptr('0x887704'), ptr('0x887724')];
    Interceptor.attach(ptr('0x4F6288'), { onEnter: function () {
        try {
            var pkt = this.context.esi;
            if (pkt.add(7).readU8() !== 0x64) return;
            var rw = pkt.add(0x10).readU16(), rh = pkt.add(0x12).readU16();
            var rx = pkt.add(8).readU16(); if (rx & 0x8000) rx -= 0x10000;
            var ry = pkt.add(0xA).readU16(); if (ry & 0x8000) ry -= 0x10000;
            var met = pkt.add(0xE).readU16();
            var s = HU_mstat[met] || (HU_mstat[met] = [9999, -9999, 9999, -9999, 0, 0]);
            if (rx < s[0]) s[0] = rx; if (rx + rw > s[1]) s[1] = rx + rw;
            if (ry < s[2]) s[2] = ry; if (ry + rh > s[3]) s[3] = ry + rh;
            s[4]++;
            var md = rw < rh ? rw : rh; if (md > s[5]) s[5] = md;
            if (HU_mblack[met]) return;                 // backdrop set -> stays stretched
            if (rw > 288 || rh > 240) return;           // lone-tile sanity cap
            var xs = [], lo = 1e9, hi = -1e9;
            for (var i = 0; i < 4; i++) { var x = HU_V64X[i].readFloat(); xs.push(x); if (x < lo) lo = x; if (x > hi) hi = x; }
            if (!(lo > -1000 && hi < 3000) || (hi - lo) >= 600) return;
            var dc = HU_curCenter * 2;                  // device space = logical x2
            for (var j = 0; j < 4; j++) HU_V64X[j].writeFloat(dc + (xs[j] - dc) * HU_WIDTH);
        } catch (e) { /* skip this draw */ }
    }});
    send({type:'info', msg:'HUD/dialogue/menu un-stretch + edge-push ON.'});
}

// ===== ORBIT-MODE INDICATOR (native in-engine toast: coloured dot + game-font text) =====
// On each orbit toggle we show "[dot] ORBIT ON/OFF" (green dot = ON, red = OFF) top-left for
// ~2.7s, then it slides up and off. Drawn INSIDE the game's display list so it survives our
// DDrawCompat borderless-FLIP present (which an external overlay window can't draw over - the
// same thing that blocks Win+Shift+S). Two primitive kinds, both re-linked into the HUD bucket
// (ctx+0x78) once per frame:
// DOT = flat translucent quad, opcode 0x2a -> handler 0x4F3E53. TRIANGLESTRIP (DrawPrim type
// 5) so verts go TL,TR,BL,BR (perimeter order draws a crossing bowtie = the old "flag").
// W float 0.1 -> RHW 1.0 (screen-space). cmd bit1 = the semi-transparent alpha path.
// TEXT = game-font glyph packets, opcode 0x6c -> handler 0x4F62DF, one 0x10-byte packet per
// char: RGB@+4/5/6, X@+8, Y@+0xA, ASCII char@+0xC, font-page metric@+0xE. The handler
// rasterises+caches each glyph from (char,metric) itself. Char code is plain ASCII;
// advance widths come live from the game's own table 0x52DFA0. 0x6c's cmd bit2 DOUBLES
// the colour, so 0x80 -> full white. Metric defaults to 0x52E3C4[0]=0x7f3c and is auto-
// locked to a real on-screen glyph's metric the first time the un-stretch pass sees one.
// Our hook prepends AFTER the un-stretch pass has walked the bucket this frame, so it never
// re-squeezes our packets - we pre-apply the 0.75 squeeze (sqx) ourselves. Glyphs are fixed-size
// (font atlas) so they can't scale; the whole toast slides IN from the top on show and slides OUT
// upward on dismiss (position animation, not scale).
var TOAST_DUR = 80;                     // frames visible (~2.7s at 30fps)
var TOAST_FADE = 12;                    // last N frames: slide up & off
var TOAST_SLIDE = 34;                   // px (320-space) travelled off the top during slide-out
var TOAST_X = -2, TOAST_Y = 16;         // top-left anchor (HUD 320-space)
var DOT_W = 9, DOT_H = 9, TXT_GAP = 5;  // status-dot size + gap before the text
var TXT_DY = -2;                         // text vertical offset vs the dot (negative = higher)
var TXT_COL = 0x80;                     // glyph colour byte (opcode 0x6c doubles it -> 0x80 = white)
var toastFrames = Memory.alloc(4); toastFrames.writeS32(0);
var toastOn = 0;                        // 1 = ON (green dot), 0 = OFF (red); set by rpc.toast
var DOT_PKT = Memory.alloc(0x40);
DOT_PKT.add(7).writeU8(0x2a);
(function () { var w = [0xCD, 0xCC, 0xCC, 0x3D]; [0x18, 0x1c, 0x20, 0x24].forEach(function (o) { DOT_PKT.add(o).writeByteArray(w); }); })();
var FONT_WID = ptr('0x52dfa0');         // game's own per-ASCII advance-width table (read live)
var FONT_METRIC = 0x7f3c;               // font-page key (0x52E3C4[0]); auto-locked from a real glyph (see un-stretch)
var GLYPHS = []; for (var gi = 0; gi < 12; gi++) { var gb = Memory.alloc(0x10); gb.add(7).writeU8(0x6c); GLYPHS.push(gb); }
function sqx(x) { return ANAMORPH ? Math.round(160 + (x - 160) * RATIO) : Math.round(x); }   // match HUD un-stretch
function prependHud(ctx, pkt) { var b = ctx.add(0x78); pkt.writePointer(b.readPointer()); b.writePointer(pkt); }
// MML's font is TILE-indexed, not ASCII (verified from a dialogue capture): the glyph packet's +0xC
// holds a font-tile code. The layout is linear: A-Z = 0x15.., a-z = 0x2f.., space = 0x0A. The advance
// width table 0x52DFA0 is indexed by this SAME tile code.
function tileOf(cc) {                                    // cc = ASCII code from the JS string
    if (cc >= 65 && cc <= 90) return 0x15 + (cc - 65);   // A-Z
    if (cc >= 97 && cc <= 122) return 0x2f + (cc - 97);  // a-z
    return 0x0a;                                         // space / anything else
}
function writeDot(y) {
    DOT_PKT.add(4).writeU8(toastOn ? 0 : 224); DOT_PKT.add(5).writeU8(toastOn ? 224 : 0); DOT_PKT.add(6).writeU8(0);
    var xl = sqx(TOAST_X) & 0xffff, xr = sqx(TOAST_X + DOT_W) & 0xffff, yt = y & 0xffff, yb = (y + DOT_H) & 0xffff;
    DOT_PKT.add(0x08).writeU16(xl); DOT_PKT.add(0x0a).writeU16(yt);   // TL
    DOT_PKT.add(0x0c).writeU16(xr); DOT_PKT.add(0x0e).writeU16(yt);   // TR
    DOT_PKT.add(0x10).writeU16(xl); DOT_PKT.add(0x12).writeU16(yb);   // BL
    DOT_PKT.add(0x14).writeU16(xr); DOT_PKT.add(0x16).writeU16(yb);   // BR
}
rpc.exports.toast = function (on) { toastOn = on ? 1 : 0; toastFrames.writeS32(TOAST_DUR); };
Interceptor.attach(ptr('0x4A70E5'), { onEnter: function () {
    var left = toastFrames.readS32();
    if (left <= 0) return;
    try {
        var ctx = ptr('0x77EA74').readPointer();
        if (ctx.isNull()) return;
 // slide IN from off the top over the first TOAST_FADE frames, hold, then slide OUT the last TOAST_FADE
        var slide = 0;
        if (left > TOAST_DUR - TOAST_FADE) slide = (left - (TOAST_DUR - TOAST_FADE)) / TOAST_FADE * TOAST_SLIDE;
        else if (left < TOAST_FADE) slide = (TOAST_FADE - left) / TOAST_FADE * TOAST_SLIDE;
        var y = TOAST_Y - Math.round(slide);            // slide up & off (out) / down into place (in)
        writeDot(y);
        prependHud(ctx, DOT_PKT);
        var s = toastOn ? "ORBIT ON" : "ORBIT OFF";
        var m = FONT_METRIC & 0xffff, ty = (y + TXT_DY) & 0xffff, cx = TOAST_X + DOT_W + TXT_GAP;
        for (var i = 0; i < s.length && i < GLYPHS.length; i++) {
            var tc = tileOf(s.charCodeAt(i)), gp = GLYPHS[i];
            gp.add(4).writeU8(TXT_COL); gp.add(5).writeU8(TXT_COL); gp.add(6).writeU8(TXT_COL);
            gp.add(0x08).writeU16(sqx(cx) & 0xffff); gp.add(0x0a).writeU16(ty);
            gp.add(0x0c).writeU16(tc); gp.add(0x0e).writeU16(m);
            prependHud(ctx, gp);
            cx += FONT_WID.add(tc).readU8() + 1;        // advance by the tile's own width (+1 tracking)
        }
        toastFrames.writeS32(left - 1);
    } catch (e) { /* bad ctx this frame - skip */ }
}});

// ===== FREE-ORBIT CAMERA =====
// The two camera-apply routines both store yaw -> 0x77EA0A then build the matrix:
// funcA 0x42961B: yaw arg in EBX ([esp+0x18]=ebx -> 0x77EA0A)
// funcB 0x4298F6: yaw arg in EDX (and dh,0xf; mov [0x77ea0a],dx)
// We hook BOTH at entry, override the right register when active, and COUNT
// calls so we can see which path actually drives the camera while walking.
// Seed is mode-independent: on toggle-ON, Python reads current 0x77EA0A via
// getyaw() so there's no snap regardless of which routine is live.
var camYaw = Memory.alloc(4);  camYaw.writeS32(0);     // -> EBX (angle1 yaw, stored directly)
var camPitchEcx = Memory.alloc(4); camPitchEcx.writeS32(0);  // -> ECX (angle0 pitch; funcA negates it, so Python pre-negates)
var camActive = Memory.alloc(4); camActive.writeS32(0);
var camSuppress = Memory.alloc(4); camSuppress.writeS32(0);  // 1 = don't override this frame (aim mode)
// TRUE-ORBIT: decouple the camera POSITION from the player facing so moving opposite
// to the look doesn't thrash the follow-cam. We rotate the game's own camera-offset
// vector by (look_yaw - 0x77E960) so the camera sits behind where you LOOK, keeping
// the game's distance/height/pitch. Set ORBIT_TRUE=0 to disable; flip ORBIT_SIGN if
// it rotates the wrong way.
var ORBIT_TRUE = 1;
var ORBIT_SIGN = 1;
// DARK-SOULS DODGE ROLL: the roll rotates a fixed SIDEWAYS local vector by the
// character's facing, so it goes facing+/-90. While a roll is active, we offset the
// facing we write by ROLL_OFFSET (~-/+90deg) so that +/-90 cancels and the roll goes
// along the MOVE direction instead. Roll is detected by the roll-velocity field
// 0x5F8384 spiking (|.|>thresh; it's ~-1.0 idle/walk, ~-16 during a roll).
var ROLL_REDIRECT = 1;
// Per-frame recon (watch_roll) proved the roll direction is computed LIVE as facing +/- 90:
// right roll = facing + 1024, left roll = facing - 1024 (1024 units = 90 deg; 4096 = 360).
// The player struct 0x5F8364 gives us two clean signals:
// * +0x08 (0x5F836C) bit 0x800 = "roll in progress" (sets ~3 frames BEFORE the roll moves,
// stays on through recovery) -> lets us steer before the motion commits.
// * +0x08 bit 0x01000000 = left/right flag (1 = left roll, 0 = right roll).
// So we STEER the roll along the last walk direction by writing facing 0x5F83B8 each roll
// frame to (walkAngle - offset), where offset = +1024 (right) / -1024 (left); then the
// engine's own facing+/-90 lands exactly on walkAngle. The old field 0x5F8384 we were
// band-gating on turned out to be the roll's vertical HOP (also lights up on jumps), which
// is why the position-overwrite kept missing - abandoned in favour of this.
var ROLL_STATE = 0x5f836c;       // player struct +0x08: bit 0x800 = rolling, bit 0x1000000 = left
var ROLL_MOVE_MIN = 200000;      // 16.16 world delta (~3 units) to trust a walk-heading sample
// 0x5F8384 (+0x20) is the player's Y HEIGHT (16.16), NOT a velocity - don't gate on it.
var rollLastX = 0, rollLastZ = 0, rollWalkAng = 0, rollWasRolling = false, rollHasMoved = false;
// ---- DODGE trigger synthesis, paced in GAME FRAMES (funcA @30Hz) not XInput polls ----
// The roll fires on "hold camera-rotate trigger for a few frames, THEN tap jump". Pacing
// this in XInput polls was racy (polls run faster than the 30Hz game logic) -> coin-flip
// roll-vs-jump. So funcA runs a game-frame state machine; the XInput hook just mirrors the
// dodgeInj* flags into the buffer. LT held DODGE_LEAD frames to enter the roll stance, then
// A pressed for DODGE_A_FRAMES frames = a clean, deterministic roll edge.
var DODGE_LEAD = 3;              // game frames to hold LT before the A tap (lower = snappier)
var DODGE_A_FRAMES = 2;          // game frames to hold A
var DODGE_ZERO_LEAD = 2;         // force the standstill only this many frames before the A tap
 // (so a RUN keeps its momentum through the LT lead = no dead-stop)
var DODGE_TOTAL = DODGE_LEAD + DODGE_A_FRAMES + 1;
var DODGE_STEER_WIN = 35;        // game frames after a dodge starts during which roll-steer is
 // allowed. The 0x800 bit also sets on JUMPS (fires on landing),
 // so we ONLY steer inside a window opened by OUR dodge -> a plain
 // jump never opens it and is left alone.
var dodgePhase = 0;             // game-frame countdown (closure)
var rollSteerWin = 0;          // game-frame countdown; >0 = a dodge is in flight, steering allowed
var cntA = 0, cntB = 0;
var callers = {};   // funcA return-address histogram (identify follow vs aim/cutscene)
rpc.exports.setcam = function(yaw, pitchEcx, active, suppress) {
    camYaw.writeS32(yaw & 0xffff);
    camPitchEcx.writeS32(pitchEcx & 0xffff);
    camActive.writeS32(active);
    camSuppress.writeS32(suppress);
};
// returns [angle1 (yaw @0x77EA0A), angle0 (pitch @0x77EA08)] for seamless seeding
rpc.exports.getcam = function() { return [ptr('0x77ea0a').readU16(), ptr('0x77ea08').readU16()]; };
rpc.exports.getroll = function() {
    var st = ptr(ROLL_STATE).readU32();
    return [(st & 0x800) ? 1 : 0,      // roll-in-progress bit
            (st >>> 24) & 1,            // left flag (1=left, 0=right)
            rollWalkAng,                // last walk heading (game angle units)
            (ptr('0x5f83b8').readU32() >>> 16) & 0xfff,   // live facing
            ptr('0x5f8380').readS32() >> 16, ptr('0x5f8388').readS32() >> 16,  // X,Z
            ptr('0x5f8384').readS32() >> 16,                    // Y height (int)
            ptr('0x5f838c').readS32(), ptr('0x5f8390').readS32(),   // candidate vel (+0x28/+0x2c)
            ptr('0x5f8394').readS32(), ptr('0x5f8398').readS32()];  // candidate vel (+0x30/+0x34)
};

// ===== CAMERA-RELATIVE MOVEMENT =====
// Real player facing = 0x5F83B8 (16.16 fixed; angle = high word). Writing it turns
// the character (validated). MML is tank-style (moves along facing), so to get
// camera-relative movement we (a) WRITE the facing to the left-stick direction
// taken relative to the orbit camera, and (b) feed the game "forward" via the
// XInput hook so it walks that way at the stick's magnitude, with no tank steering.
var moveActive = Memory.alloc(4); moveActive.writeS32(0);  // 1 = inject forward on left stick
var moveFwd = Memory.alloc(4); moveFwd.writeS32(0);        // forward amount (0..32767)
var moveFace = Memory.alloc(4); moveFace.writeS32(0);      // desired facing (game angle units)
var moveGate = Memory.alloc(4); moveGate.writeS32(0);      // >0 = player-movement update ran recently
var rollLatch = Memory.alloc(4); rollLatch.writeS32(0);   // 1 = a roll is being steered; move-facing writers stand down
var injectFwd = Memory.alloc(4); injectFwd.writeS32(0);   // funcA->hook: 1 = override L-stick with forward (gameplay only, NOT menus)
var MOVE_START_GRACE = 4; var moveStartGrace = 0, movePrev = 0;  // bridge the idle->walk bootstrap before pos-credit kicks in
var dodgeReq = Memory.alloc(4); dodgeReq.writeS32(0);     // hook->funcA: X pressed (dodge requested)
var dodgeInjLT = Memory.alloc(4); dodgeInjLT.writeS32(0); // funcA->hook: hold game LT this frame
var dodgeInjA = Memory.alloc(4); dodgeInjA.writeS32(0);   // funcA->hook: press A this frame
var dodgeZero = Memory.alloc(4); dodgeZero.writeS32(0);   // funcA->hook: zero left stick this frame
var frameTick = Memory.alloc(4); frameTick.writeS32(0);   // funcA heartbeat (stale-flag detection in the XInput hook)
// Secondary facing gate: is the player POSITION actually changing? True during a
// standstill->walk start (before moveGate's heartbeat catches up) but false during
// dialogue/cutscene (player locked = static), so it opens the facing gate early
// without reintroducing dialogue circling.
var lastPX = Memory.alloc(4); lastPX.writeS32(0);
var lastPZ = Memory.alloc(4); lastPZ.writeS32(0);
var posMoveCredit = Memory.alloc(4); posMoveCredit.writeS32(0);
// The facing write is done INSIDE the once-per-frame funcA hook (below), not here,
// so it's frame-synced and can't race the game's own movement update.
rpc.exports.setmove = function(face, fwd, active) {
    moveFace.writeS32(face); moveFwd.writeS32(fwd); moveActive.writeS32(active);
};

Interceptor.attach(ptr('0x42961b'), { onEnter: function () {
 // Game-frame heartbeat: the XInput hook compares this to detect a stalled funcA
 // (pause/transition) and then IGNORES the funcA-computed injection flags, which
 // would otherwise go stale and hold LT / zero the stick indefinitely.
    frameTick.writeS32((frameTick.readS32() + 1) & 0x3fffffff);
    if (CAM_DEBUG) {
        cntA++;
        var ra = this.returnAddress.toUInt32().toString(16);
        callers[ra] = (callers[ra] || 0) + 1;
    }
 // Frame-synced facing write: set the player facing 0x5F83B8 here (once/frame)
 // so it can't race the game's own per-frame movement update. GATED on moveGate
 // (the player-movement heartbeat 0x4B53D3) so we DON'T rotate the character
 // during dialogue/cutscene locks - the gate decays when that update stops.
 // update the position-moving credit (opens the facing gate at a standstill start,
 // before moveGate's heartbeat catches up; stays closed during dialogue = static)
    var px = ptr('0x77e998').readS32(), pz = ptr('0x77e9a0').readS32();
    if (px !== lastPX.readS32() || pz !== lastPZ.readS32()) posMoveCredit.writeS32(4);
    else { var pc = posMoveCredit.readS32(); if (pc > 0) posMoveCredit.writeS32(pc - 1); }
    lastPX.writeS32(px); lastPZ.writeS32(pz);
    var g = moveGate.readS32();
    if (moveActive.readS32() && !rollLatch.readS32() && (g > 0 || posMoveCredit.readS32() > 0)) {
 // 12-bit angle in the high word; mask 0xfff so (<<16) stays a positive uint32
 // (0x8000<<16 would go negative in JS and writeU32 would reject it).
 // Suppressed while rollLatch is set so it can't fight the roll-steer facing.
        ptr('0x5f83b8').writeU32((moveFace.readS32() & 0xfff) << 16);
    }
    if (g > 0) moveGate.writeS32(g - 1);
 // Decide whether the hook may override the LEFT stick with forward. Gate it like the
 // facing write (player-movement heartbeat / position-change credit) so it stands down in
 // MENUS (player never moves -> both stay 0 -> raw stick reaches the menu = down works).
 // moveStartGrace bridges the idle->walk bootstrap: at a stick-push from rest the credit
 // isn't set yet, so grant a few forced-forward frames until the position starts changing.
    var ma = moveActive.readS32();
    if (ma && !movePrev) moveStartGrace = MOVE_START_GRACE;
    movePrev = ma;
    injectFwd.writeS32((ma && (moveGate.readS32() > 0 || posMoveCredit.readS32() > 0 || moveStartGrace > 0)) ? 1 : 0);
    if (moveStartGrace > 0) moveStartGrace--;
 // ===== DARK-SOULS DODGE ROLL: steer the roll along the walk direction =====
    if (ROLL_REDIRECT) {
        var pxr = ptr('0x5f8380').readS32(), pzr = ptr('0x5f8388').readS32();  // player X/Z (16.16)
        var st = ptr(ROLL_STATE).readU32();
 // rolling = roll-in-progress bit set, NOT aiming, and inside a dodge-opened window.
 // The 0x800 bit is a general action bit set during aim (=> !camSuppress gate) AND on
 // jumps (fires on landing => rollSteerWin gate: only OUR dodge opens the window, so a
 // plain jump's landing is left alone). We steer every (windowed) roll moving or not
 // so a dodge out of a run works; rollLatch tells move-facing writers to stand down.
 // NOTE: 0x5F8384 (+0x20) is the player's Y HEIGHT, not a velocity - it reads ~-1 on
 // downtown ground but ~-769 on the elevated sector, so the old |.|<ROLL_JUMP_VEL
 // guard wrongly killed steering at height (=> vanilla 90-deg side roll). Dropped.
        var rolling = ((st & 0x800) !== 0) && !camSuppress.readS32() && (rollSteerWin > 0);
        rollLatch.writeS32(rolling ? 1 : 0);
        if (rolling) {
 // While the roll is TRAVELLING, face sideways (walkAng -/+ 90) so the engine's
 // own +/-90 lands travel on walkAng. Once it STOPS moving (recovery frames),
 // face the travel dir (walkAng, register +2048) right away so there's no
 // stationary-sideways moment before the end snap -> no jarring correction.
            var mdx = pxr - rollLastX, mdz = pzr - rollLastZ;
            if (!rollWasRolling) rollHasMoved = false;      // reset at roll start
            var moved2 = mdx * mdx + mdz * mdz;
            if (moved2 > (ROLL_MOVE_MIN * ROLL_MOVE_MIN)) rollHasMoved = true;
            var stopped = rollHasMoved && (moved2 <= (ROLL_MOVE_MIN * ROLL_MOVE_MIN));
            var faceTgt;
            if (stopped) {
                faceTgt = (rollWalkAng + 2048) & 0xfff;     // recovery: face travel dir
            } else {
                var leftRoll = (st >>> 24) & 1;
                faceTgt = (rollWalkAng + (leftRoll ? 1024 : -1024)) & 0xfff;  // steer
            }
            ptr('0x5f83b8').writeU32(faceTgt << 16);
        } else {
            if (rollWasRolling) {
 // roll just ended: face where we actually rolled. rollWalkAng is a TRAVEL
 // (position) angle; the VISUAL facing = register + 2048 (same 180 offset as
 // FACE_OFFSET in movement), so to face the roll dir the register is +2048.
                ptr('0x5f83b8').writeU32(((rollWalkAng + 2048) & 0xfff) << 16);
            }
 // normal locomotion: continuously learn the world walk heading from the
 // position delta so it's fresh when a roll starts (the roll needs a standstill,
 // so the last-known heading = the direction the player was just walking).
            var wdx = pxr - rollLastX, wdz = pzr - rollLastZ;
            if ((wdx * wdx + wdz * wdz) > (ROLL_MOVE_MIN * ROLL_MOVE_MIN)) {
                rollWalkAng = Math.round(Math.atan2(wdx, wdz) * 2048.0 / Math.PI) & 0xfff;
            }
        }
        rollLastX = pxr; rollLastZ = pzr;
        rollWasRolling = rolling;
    }
 // ===== DODGE trigger state machine (game-frame paced; hook mirrors the flags) =====
 // Only START a dodge from a NEUTRAL/grounded state: the 0x800 bit is set while jumping/
 // rolling/aiming, so requiring it CLEAR means X in the air (or mid-action) is a no-op and
 // can't leave a dodge window open that fires the sideways steer on landing.
    if (dodgeReq.readS32() && dodgePhase === 0) {
        if (!(ptr(ROLL_STATE).readU32() & 0x800)) { dodgePhase = DODGE_TOTAL; rollSteerWin = DODGE_STEER_WIN; }
        dodgeReq.writeS32(0);   // consume the request either way (discard if airborne)
    }
    if (rollSteerWin > 0) rollSteerWin--;   // only inside this (dodge-opened) window do we steer rolls
    if (dodgePhase > 0) {
        var df = DODGE_TOTAL - dodgePhase;                 // 0-based game-frame index in window
        dodgeInjLT.writeS32(1);                            // LT (rotate) held the whole window
        dodgeInjA.writeS32((df >= DODGE_LEAD && df < DODGE_LEAD + DODGE_A_FRAMES) ? 1 : 0);
 // hold the run through the lead; only force the standstill just before the A tap
        dodgeZero.writeS32((df >= DODGE_LEAD - DODGE_ZERO_LEAD) ? 1 : 0);
        dodgePhase--;
    } else {
        dodgeInjLT.writeS32(0); dodgeInjA.writeS32(0); dodgeZero.writeS32(0);
    }
 // Override only when active AND not suppressed. Suppress = aim mode: we let
 // the game drive the camera so aim direction and view agree; Python keeps our
 // held yaw/pitch tracking the live camera meanwhile, so releasing aim = no snap.
    if (camActive.readS32() && !camSuppress.readS32()) {
        this.context.ebx = ptr(camYaw.readS32() & 0xffff);      // yaw
        this.context.ecx = ptr(camPitchEcx.readS32() & 0xffff); // pitch
        if (ORBIT_TRUE) {
 // Rotate the camera-to-target offset so the camera sits behind the LOOK
 // (camYaw), not behind the facing-derived angle 0x77E960. delta cancels
 // the facing drift; we then set 0x77E960 = camYaw so re-entry is idempotent.
            var camAz = ptr('0x77e960').readU32() & 0xfff;
            var yaw = camYaw.readS32() & 0xfff;
            var delta = (yaw - camAz) & 0xfff;
            if (delta !== 0) {
                var tx = ptr('0x77e998').readS32(), tz = ptr('0x77e9a0').readS32();
                var offX = ptr('0x56f182').readS32() - tx;
                var offZ = ptr('0x56f186').readS32() - tz;
                var rad = ORBIT_SIGN * delta * (Math.PI / 2048.0);   // 2048 units = 180deg
                var cs = Math.cos(rad), sn = Math.sin(rad);
                ptr('0x56f182').writeS32((tx + Math.round(offX * cs - offZ * sn)) | 0);
                ptr('0x56f186').writeS32((tz + Math.round(offX * sn + offZ * cs)) | 0);
                ptr('0x77e960').writeU32(yaw);
            }
        }
    }
}});
Interceptor.attach(ptr('0x4298f6'), { onEnter: function () {
    if (CAM_DEBUG) cntB++;
    if (camActive.readS32()) { this.context.edx = ptr(camYaw.readS32() & 0xffff); }
}});

// Player-movement heartbeat: 0x4B53D3 is the movement helper the player state
// machine calls each frame WHILE the player has movement control (walking/running
// states). It stops during dialogue/cutscene/interrupt locks. We refresh moveGate
// whenever it fires; funcA decays it. So our facing/forward override only applies
// while the player is genuinely controllable -> no more spinning on the spot in
// dialogue, and no menu/cutscene misbehaviour for movement.
Interceptor.attach(ptr('0x4b53d3'), { onEnter: function () { moveGate.writeS32(6); } });

// Defeat the game's rate-limited turn during camera-relative movement: the turn
// integrator 0x4B5502 sweeps the facing toward the target at a capped speed, so a
// reversal coasts forward while it swings around. We overwrite the facing with our
// target right AFTER the integrator (its result is discarded), so the character faces
// the move direction immediately and the position update uses it - no coast. Gated
// like the facing write so dialogue/locks are unaffected.
Interceptor.attach(ptr('0x4b5502'), { onLeave: function () {
    if (moveActive.readS32() && !rollLatch.readS32() && (moveGate.readS32() > 0 || posMoveCredit.readS32() > 0)) {
        ptr('0x5f83b8').writeU32((moveFace.readS32() & 0xfff) << 16);
    }
}});

// once/sec camera diagnostics (funcA/funcB call rates + caller histogram). Debug
// tooling only - gated off by default so a shipped session has a quiet console.
if (CAM_DEBUG) setInterval(function () {
    send({type:'camdbg', a: cntA, b: cntB, yaw: ptr('0x77ea0a').readU16(),
          active: camActive.readS32(), callers: callers});
    cntA = 0; cntB = 0; callers = {};
}, 1000);

// ===== AIM-ON-RIGHT-STICK (input hijack via Xidi's XInput poll) =====
// The game's aim logic reads the LEFT stick (through Xidi -> DirectInput). Xidi
// feeds itself from XInputGetState. So while orbit is engaged AND aim (LB) is
// held, we swap the physical LEFT and RIGHT stick axes right after the real
// XInputGetState fills the buffer - the game's own unmodified aim code then runs
// off the RIGHT stick, keeping aim consistent with the orbit camera. Vanilla
// left-stick aim whenever orbit is off. xinput loads lazily, so we poll for it.
(function hookXInputSwap() {
    var LB = 0x0100, A_BTN = 0x1000, X_BTN = 0x4000;
    var TRIG_ON = 40;        // analog trigger considered "pressed" above this
 // Dodge = synthesize the vanilla roll (hold camera-rotate trigger, THEN tap jump). The
 // timing is run by funcA in GAME frames (DODGE_LEAD/DODGE_A_FRAMES above); this hook only
 // detects the physical-X edge (-> dodgeReq) and mirrors funcA's dodgeInj* flags.
    function attachTo(addr, modname) {
        var dodgePrevX = false;   // for physical-X rising-edge detection (persists across polls)
        var lastFt = -1, ftStale = 0;   // funcA-heartbeat freshness (see below)
        var dodgeReqAge = 0;            // polls a dodge request has sat unconsumed
        Interceptor.attach(addr, {
            onEnter: function (args) { this.buf = args[1]; },
            onLeave: function (retval) {
                if (retval.toInt32() !== 0) return;   // ERROR_DEVICE_NOT_CONNECTED etc.
                var buf = this.buf;
                if (buf.isNull()) return;
 // ===== D-PAD -> LEFT STICK (menu navigation + movement, both modes) =====
 // The LEFT stick already drives both menu cursors and movement (user-confirmed);
 // the D-pad drives neither. So when the physical D-pad is pressed, feed the LEFT
 // stick axes full-deflection in that direction -> the D-pad behaves exactly like
 // the left stick. Runs BEFORE the orbit-mode early-return so it works in vanilla
 // too. In modern gameplay the movement injection below may still override the
 // stick while you're actively moving - fine; in menus it passes straight through.
                var dpb = buf.add(4).readU16();
                var ddx = ((dpb & 0x0008) ? 1 : 0) - ((dpb & 0x0004) ? 1 : 0);   // right - left
                var ddy = ((dpb & 0x0001) ? 1 : 0) - ((dpb & 0x0002) ? 1 : 0);   // up - down
                if (ddx || ddy) {
                    buf.add(8).writeS16(ddx * 32767);     // LX
                    buf.add(10).writeS16(ddy * 32767);    // LY (XInput: up = positive)
                }
                if (!camActive.readS32()) return;      // mod off -> untouched (vanilla)
 // FRESHNESS: dodgeInj*/injectFwd are computed by the once-per-frame funcA
 // hook. If the game stops running funcA (pause menu, transition), those
 // flags go STALE - acting on them would hold LT / zero the stick until
 // gameplay resumes. If funcA hasn't ticked in ~30 polls, ignore them all.
                var ft = frameTick.readS32();
                if (ft === lastFt) { if (ftStale < 1000) ftStale++; } else { ftStale = 0; lastFt = ft; }
                var fresh = ftStale < 30;
 // XINPUT_STATE: +4 wButtons, +6 LT, +7 RT, +8 LX, +10 LY, +12 RX, +14 RY
                var btn = buf.add(4).readU16();
                var physX  = (btn & X_BTN) !== 0;                 // vanilla SHOOT -> now DODGE
                var physLT = buf.add(6).readU8() > TRIG_ON;       // -> AIM (read before we zero it)
                var physRT = buf.add(7).readU8() > TRIG_ON;       // -> SHOOT

 // ---- BUTTON REMAP ----
 // Kill ALL vanilla trigger-rotate in orbit mode (orbit replaced it). LT is
 // re-synthesized below only for the dodge; RT becomes shoot; LT becomes aim.
                buf.add(6).writeU8(0); buf.add(7).writeU8(0);     // no trigger-rotate in orbit mode
                var out = btn & ~X_BTN;                            // X is dodge now, never shoots
                if (physRT) out |= X_BTN;                          // RT -> shoot (synthesize X)
 // AIM = physical LT (new) OR physical LB (kept as harmless fallback). The game
 // aims off its LB button, so synthesize LB when aiming; the L<->R swap below
 // then runs the game's left-stick aim off the RIGHT stick.
                var aiming = physLT || ((btn & LB) !== 0);
                if (aiming) out |= LB;

 // ---- DODGE: physical X edge (not while aiming) -> ask funcA to run the roll
 // trigger (game-frame paced). The hook only MIRRORS funcA's dodgeInj* flags. ----
                var xEdge = physX && !dodgePrevX;
                dodgePrevX = physX;
                if (xEdge && !aiming) { dodgeReq.writeS32(1); dodgeReqAge = 0; }
 // TTL: funcA normally consumes a request within one game frame. If it sits
 // unconsumed (funcA stalled = pause menu), expire it - otherwise X pressed
 // in a menu would fire a phantom roll the moment gameplay resumes.
                if (dodgeReq.readS32()) { if (++dodgeReqAge > 20) dodgeReq.writeS32(0); }
                else dodgeReqAge = 0;
                var dodging = fresh && dodgeInjLT.readS32() !== 0;
                if (dodging) buf.add(6).writeU8(255);             // hold game LT (rotate)
                if (fresh && dodgeInjA.readS32()) out |= A_BTN;   // press A
                buf.add(4).writeU16(out);

 // ---- LEFT STICK ----
                if (fresh && dodgeZero.readS32()) {
 // force standstill so the game's roll gate accepts it even mid-run
                    buf.add(8).writeS16(0); buf.add(10).writeS16(0);
                } else if (aiming) {
 // AIM: swap L<->R so the game's left-stick aim runs off the right stick
                    var lx = buf.add(8).readS16(),  ly = buf.add(10).readS16();
                    var rx = buf.add(12).readS16(), ry = buf.add(14).readS16();
                    buf.add(8).writeS16(rx);  buf.add(10).writeS16(ry);
                    buf.add(12).writeS16(lx); buf.add(14).writeS16(ly);
                } else if (fresh && moveActive.readS32() && injectFwd.readS32()) {
 // MOVE: override the left stick to (0, forward) while walking in gameplay.
 // - LX=0 so the game never tank-turns; LY=forward so it walks along the
 // facing Python set. The raw physical stick (meaningless tank input in
 // camera-relative mode) never leaks.
 // - Gated (in funcA) on the player-movement heartbeat / position credit so
 // it STANDS DOWN in menus (where the player never moves) -> the raw stick
 // reaches the menu and DOWN works again. A short grace bridges walk-start.
 // - moveActive (refreshed at 60Hz from Python, independent of funcA) plus
 // the freshness guard make a stale injectFwd harmless.
                    buf.add(8).writeS16(0);
                    buf.add(10).writeS16(moveFwd.readS32());
                }
            }
        });
        send({type:'info', msg:'Input remap hook armed (RT=shoot, X=dodge; XInputGetState in ' + modname + ').'});
    }
    var scanErrs = 0;
    var iv = setInterval(function () {
        try {
            var mods = Process.enumerateModules();
            for (var i = 0; i < mods.length; i++) {
                if (!/xinput/i.test(mods[i].name)) continue;
 // Frida-version-agnostic: prefer the module-instance method
 // (module.findExportByName), fall back to getExportByName.
                var m = mods[i], ex = null;
                if (typeof m.findExportByName === 'function') ex = m.findExportByName('XInputGetState');
                else if (typeof m.getExportByName === 'function') { try { ex = m.getExportByName('XInputGetState'); } catch (e) {} }
                if (ex && !ex.isNull()) { clearInterval(iv); attachTo(ex, m.name); return; }
            }
            scanErrs = 0;   // a clean pass = any earlier errors were transient
        } catch (e) {
 // Transient (module-list churn while DLLs load) - keep polling; only a
 // PERSISTENT error (~15s of consecutive failures) disables the remap.
            if (++scanErrs >= 60) {
                clearInterval(iv);
                send({type:'info', msg:'Input remap hook FAILED to install (persistent error): ' + e});
            }
        }
    }, 250);
})();

// ===== clean detach =====
// Frida calls dispose() when the script unloads (Ctrl+C, launcher console closed, or
// game exit). Interceptor hooks revert automatically; this restores the RAW byte-
// patches so a still-running game continues fully vanilla instead of half-modded.
// (The GetTickCount IAT stub is deliberately LEFT installed - it lives in persistent
// game-owned memory, and yanking it would make the rebased clock jump hours forward,
// stampeding the frame limiter.)
rpc.exports.dispose = function () {
    try {
        if (ANAMORPH) {
            for (var i = 0; i < xSites.length; i++) ptr(xSites[i]).add(2).writeU32(0x77ebdc);
            HU_GW.writeU8(0x0E);   // vanilla text glyph advance width
        }
        if (INTRO_SKIP) ptr('0x4A6F7E').writeByteArray(INTRO_CALL);   // restore intro call
    } catch (e) { /* game may already be gone */ }
};

send({type:'info', msg:'All runtime mods armed. Press F5 (or click L3/R3) to toggle the orbit camera.'});
"""  % (INTRO_SKIP, CLOSER_FACTOR, RATIO, ANAMORPH, 1 if CAM_DEBUG else 0)
# NOTE: the JS placeholders are POSITIONAL, in order of appearance in the template:
# %d INTRO_SKIP (intro block, near the top) -> %f CLOSER_FACTOR -> %f RATIO ->
# %d ANAMORPH -> %d CAM_DEBUG (anamorphic section). A mis-ordered tuple silently
# shuffles every constant (the 0.1 pre-ship bug that turned ANAMORPH off).

# ------------------------- shared state -------------------------
state = {"yaw": 0, "pitch": 0, "active": 0, "facing": 0, "run": True}
lock = threading.Lock()


def _turn_toward(cur, target, rate):
    """Step `cur` toward `target` by at most `rate` game-angle units (handles wrap)."""
    if rate <= 0:
        return target & 0xffff
    diff = (target - cur) & 0xffff
    if diff > 0x8000:
        diff -= 0x10000  # take the short way around
    if diff > rate:
        diff = rate
    elif diff < -rate:
        diff = -rate
    return (cur + diff) & 0xffff


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def on_message(message, data):
    if message['type'] == 'send':
        p = message['payload']
        if isinstance(p, dict) and p.get('type') == 'camdbg':
            callers = p.get('callers') or {}
            ctxt = "  ".join(f"0x{k}:{v}" for k, v in sorted(callers.items(), key=lambda kv: -kv[1]))
            print(f"[camdbg] funcA={p['a']}/s funcB={p['b']}/s live_yaw={p['yaw']} "
                  f"active={p['active']}  callers[{ctxt}]")
        else:
            print(p)
    elif message['type'] == 'error':
        print('[error]', message.get('description'), message.get('stack'))


def intro_skip_thread(script):
    """Gamepad skip for the CAPCOM.AVI intro (keyboard/mouse skip is native - the video's
    own WndProc clears the playing flag on WM_KEYDOWN/clicks; pads don't send window
    messages). Polls XInput while the game's 'video playing' byte 0x551FA8 is set; any
    button/trigger clears it via rpc - the game then stops+closes MCI gracefully itself."""
    deadline = time.time() + 60
    was_playing = False
    while time.time() < deadline and state["run"]:
        time.sleep(0.05)
        try:
            playing = bool(script.exports_sync.introstate())
        except Exception:
            return                       # script gone (game exited)
        if was_playing and not playing:
            return                       # video ended (naturally or skipped) - done
        was_playing = was_playing or playing
        if not playing:
            continue
        if not xinput:
            continue
        st = XINPUT_STATE()
        if xinput.XInputGetState(0, ctypes.byref(st)) == 0:
            gp = st.Gamepad
            if gp.wButtons or gp.bLeftTrigger > 40 or gp.bRightTrigger > 40:
                try:
                    script.exports_sync.introskip()
                    print("[intro] skipped (gamepad).")
                except Exception:
                    pass
                return


def input_thread(script):
    prev_key = False
    prev_click = False
    prev_moving = False
    last_move_dbg = 0.0
    scr_w, scr_h = get_screen_size()
    cx, cy = scr_w // 2, scr_h // 2
    game_hwnd = user32.FindWindowA(None, b"Megaman")
    mouse_armed = False   # becomes true once we've centered the cursor for a clean first delta
    last_pad_dbg = 0.0
    while state["run"]:
 # ---- toggle (F5 key or L3/R3 stick click), edge-triggered ----
        f5 = bool(user32.GetAsyncKeyState(VK_F5) & 0x8000)
        click = False
        rx = ry = 0
        lx = ly = 0
        buttons = lt = rt = 0
        if xinput:
            st = XINPUT_STATE()
            if xinput.XInputGetState(0, ctypes.byref(st)) == 0:
                click = bool(st.Gamepad.wButtons & (XINPUT_LTHUMB | XINPUT_RTHUMB))
                rx = st.Gamepad.sThumbRX
                ry = st.Gamepad.sThumbRY
                lx = st.Gamepad.sThumbLX
                ly = st.Gamepad.sThumbLY
                buttons = st.Gamepad.wButtons
                lt = st.Gamepad.bLeftTrigger
                rt = st.Gamepad.bRightTrigger
 # ---- capture round: log held buttons/triggers once/sec ----
        if DEBUG_PAD and (buttons or lt > 30 or rt > 30):
            now = time.time()
            if now - last_pad_dbg >= 1.0:
                last_pad_dbg = now
                held = [n for b, n in XINPUT_BUTTON_NAMES.items() if buttons & b]
                extra = []
                if lt > 30:
                    extra.append(f"LT={lt}")
                if rt > 30:
                    extra.append(f"RT={rt}")
                print(f"[pad] {' '.join(held + extra) or '(sticks only)'}")
        if (f5 and not prev_key) or (click and not prev_click):
            with lock:
                going_on = not state["active"]
            if going_on:
 # seed yaw+pitch from live camera (mode-independent) so there's no snap
                try:
                    cam = script.exports_sync.getcam()
                    yaw_seed = int(cam[0])
                    praw = int(cam[1])
                    pitch_seed = praw - 4096 if praw > 2048 else praw  # to signed
                except Exception:
                    yaw_seed, pitch_seed = 0, 0
                with lock:
                    state["yaw"] = yaw_seed
                    state["pitch"] = _clamp(pitch_seed, PITCH_LIMIT_DOWN, PITCH_LIMIT_UP)
                    state["active"] = 1
                print(f"[freecam] orbit ON (seeded yaw={yaw_seed} pitch={state['pitch']})")
                try:
                    script.exports_sync.toast(1)   # green in-engine flash
                except Exception:
                    pass
            else:
                with lock:
                    state["active"] = 0
                print("[freecam] orbit OFF")
                try:
                    script.exports_sync.toast(0)   # red in-engine flash
                except Exception:
                    pass
        prev_key, prev_click = f5, click
 # ---- aim mode: while LB is held, hand the camera back to the game so aim
 # and view agree. We stop overriding (suppress) AND keep our held
 # yaw/pitch tracking the live camera, so releasing aim doesn't snap. ----
        with lock:
            active_now = state["active"]
        aiming = bool(buttons & AIM_BUTTON) or (lt > AIM_TRIG_THRESH)  # LT (new) or LB (fallback)
        if active_now and aiming:
            try:
                cam = script.exports_sync.getcam()
                praw = int(cam[1])
                psig = praw - 4096 if praw > 2048 else praw
                with lock:
                    state["yaw"] = int(cam[0]) & 0xffff
                    state["pitch"] = _clamp(psig, PITCH_LIMIT_DOWN, PITCH_LIMIT_UP)
            except Exception:
                pass
 # ---- integrate right stick: X -> yaw, Y -> pitch (skipped while aiming) ----
        if not (active_now and aiming):
            if abs(rx) > RX_DEADZONE:
                norm = (abs(rx) - RX_DEADZONE) / (32767.0 - RX_DEADZONE)
                step = YAW_SPEED * norm * (1 if rx > 0 else -1)
                with lock:
                    state["yaw"] = int(state["yaw"] + step) & 0xffff
            if abs(ry) > RX_DEADZONE:
                norm = (abs(ry) - RX_DEADZONE) / (32767.0 - RX_DEADZONE)
 # stick UP (ry>0) tilts the view UP; flip here if it feels inverted
                step = PITCH_SPEED * norm * (1 if ry > 0 else -1)
                with lock:
                    state["pitch"] = _clamp(int(state["pitch"] + step), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP)
 # ---- mouse-X orbit (only when active + game focused; recenter cursor for
 # relative deltas so it doesn't drift to a screen edge) ----
        if ENABLE_MOUSE and state["active"] and game_hwnd and user32.GetForegroundWindow() == game_hwnd:
            pt = wintypes.POINT()
            user32.GetCursorPos(ctypes.byref(pt))
            if mouse_armed:
                dx = pt.x - cx
                if dx:
                    with lock:
                        state["yaw"] = int(state["yaw"] + dx * MOUSE_SPEED) & 0xffff
            user32.SetCursorPos(cx, cy)
            mouse_armed = True
        else:
            mouse_armed = False
 # ---- camera-relative movement: LEFT stick moves relative to the orbit
 # camera; RIGHT stick orbits. Active only while orbiting and not aiming.
 # Set facing 0x5F83B8 = orbit_yaw + stick_direction, feed forward=|stick|. ----
        moving = False
        fwd = 0
        facing = 0
        if ENABLE_CAMREL_MOVE and active_now and not aiming:
            lmag = math.hypot(lx, ly)
            if lmag > MOVE_DEADZONE:
                theta = math.atan2(lx, ly)  # 0 = stick up/forward, + = toward right
                theta_units = int(theta * 4096.0 / (2.0 * math.pi))
                with lock:
                    target = (state["yaw"] + FACE_OFFSET + FACE_SIGN * theta_units) & 0xffff
                    if not prev_moving:
                        state["facing"] = target  # snap to intended direction on move-start
                    else:
                        state["facing"] = _turn_toward(state["facing"], target, TURN_RATE)
                    facing = state["facing"]
                norm = min(1.0, (lmag - MOVE_DEADZONE) / (32767.0 - MOVE_DEADZONE))
                fwd = int(norm * 32767)
                moving = True
        if MOVE_DEBUG and active_now and (time.time() - last_move_dbg >= 0.1):
            last_move_dbg = time.time()
            try:
                r = script.exports_sync.getroll()
            except Exception:
                r = None
            if r:
 # r = [roll800, leftFlag, walkAng, liveFacing, X, Z, Yint, +0x28,+0x2c,+0x30,+0x34]
                st = "ROLL" if r[0] else "----"
                print(f"[roll] 0x800={st} walkAng={r[2]:4d} facing={r[3]:4d} pXZ=({r[4]},{r[5]}) "
                      f"o28={r[7]:9d} o2c={r[8]:9d} o30={r[9]:9d} o34={r[10]:9d}")
        prev_moving = moving
        try:
 # facing is applied frame-synced inside the funcA hook (no async race)
            script.exports_sync.setmove(facing, fwd, 1 if moving else 0)
        except Exception:
            pass
 # ---- push to the hook ----
        with lock:
            y = state["yaw"] & 0xffff
            pitch_ecx = (-state["pitch"]) & 0xffff   # funcA negates ECX -> angle0, so pre-negate
            a = state["active"]
        suppress = 1 if (a and aiming) else 0        # aim mode -> don't override
        try:
            script.exports_sync.setcam(y, pitch_ecx, a, suppress)
        except Exception:
            pass
        time.sleep(1.0 / 60.0)


# CRITICAL when this runner is executed by the frozen GUI exe (PyInstaller onefile):
# PyInstaller sets THIS process's DLL directory to its _MEI temp-extraction folder (via
# SetDllDirectory), which is full of 64-bit MSVC runtime DLLs (VCRUNTIME140/MSVCP140/
# CONCRT140/...). A frida-spawned child INHERITS that DLL directory (proven empirically:
# scratchpad/probe2.py), so the 32-bit MEGAMAN.EXE finds the wrong-architecture
# VCRUNTIME140.dll there first -> "Machine Type Mismatch" + DSOUND/DINPUT entry-point
# failures + game death (which frida then mis-reports as "refused to load frida-agent").
# Reset the DLL directory to the default search BEFORE spawning: the game then inherits an
# empty DLL directory and loads its own/system DLLs normally, exactly like the plain-python
# Play-MML.bat path. No-op when not frozen (default is already empty). This - not PATH/env -
# is the real fix; the cleaned env below is kept only as cheap belt-and-suspenders.
try:
    ctypes.windll.kernel32.SetDllDirectoryW(None)
except Exception:
    pass
_game_env = dict(os.environ)
_game_env.pop("_MEIPASS", None)
_game_env.pop("_MEIPASS2", None)
_game_env["PATH"] = os.pathsep.join(
    p for p in _game_env.get("PATH", "").split(os.pathsep)
    if p and not os.path.basename(p.rstrip("\\/ ")).upper().startswith("_MEI"))

device = frida.get_local_device()
pid = device.spawn([GAME_EXE], cwd=GAME_DIR, env=_game_env)
session = device.attach(pid)
script = session.create_script(JS)
script.on('message', on_message)
script.load()
device.resume(pid)
print(f"Spawned PID {pid}. Widescreen + free-cam armed from the start.")
if INTRO_SKIP:
    print("Intro video: bypassed entirely (MML_INTRO=skip).")
else:
    print("Intro video: press ANY pad button (or any key / click) to skip.")
    threading.Thread(target=intro_skip_thread, args=(script,), daemon=True).start()

if WINDOW_MODE == "none":
    print("External wrapper owns the window (dxwrapper windowed) - not reshaping it.")
else:
    resize_game_window(WIN_X, WIN_Y, TARGET_CLIENT_W, TARGET_CLIENT_H)

# In 'vanilla' controls mode we DON'T start the input thread: every control hook (orbit
# camera, camera-relative movement, aim swap, dodge/remap) gates on the orbit toggle, which
# only the input thread can set, so leaving it unstarted keeps them fully inert = stock
# controls. The widescreen squeeze + GetTickCount timing fix (both in JS) still apply.
if CONTROLS == "vanilla":
    print("Controls: VANILLA (stock) - orbit/aim/movement/dodge disabled. Widescreen + timing fix still on.")
else:
    t = threading.Thread(target=input_thread, args=(script,), daemon=True)
    t.start()
    print("Play normally. Press F5 (keyboard) or click L3/R3 (pad) to TOGGLE the orbit.")
    print("When ON: right stick X orbits the camera and it STAYS where you leave it.")
print("Ctrl+C (or closing this window) removes the mods; the game KEEPS RUNNING as vanilla.")
print("To stop playing, quit from the game itself.")
try:
    while True:
        time.sleep(2)
        if session.is_detached:
            print("Frida detached (game exited).")
            break
except KeyboardInterrupt:
    print("Detaching - runtime mods reverted, game keeps running (vanilla).")
finally:
    state["run"] = False
    try:
        script.unload()      # runs the JS dispose(): restores the raw byte-patches
    except Exception:
        pass                 # game already gone / session already down
