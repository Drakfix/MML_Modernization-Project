/* Modern controls.
 *
 * Where things run:
 *   cam_apply_angles hook (once per game frame, game thread)
 *       reads the pad, integrates the orbit camera, computes the movement
 *       facing, writes the player facing and overrides the camera yaw/pitch
 *       arguments.
 *   controls_filter (once per pad poll: SDL path in input.c, or the
 *   XInputGetState wrapper on the Xidi path)
 *       rewrites what the game sees: D-pad -> left stick, button remap,
 *       stick swap while aiming, forward injection while moving; X asks for
 *       a roll.
 *   player_update hook: starts the roll (the game's cartwheel, entered
 *       directly), tunes its speed, lets the stick cancel its landing, and
 *       adds the walk modifier on a light tilt (analog walk).
 *   player_move_helper hook: heartbeat proving the player has control.
 *   player_turn epilogue hook: re-applies our facing after the game's
 *       rate-limited turn so reversals don't coast.
 *
 * The game's own logic is tank-style: it walks along the facing. Camera-
 * relative movement sets the facing to (camera yaw + stick direction) and
 * feeds the game a straight "forward" on the left stick.
 */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "controls.h"
#include "game.h"
#include "hook.h"
#include "iat.h"
#include "input.h"
#include "lockon.h"
#include "log.h"
#include "toast.h"

typedef struct {
    DWORD dwPacketNumber;
    pad_t pad;
} xstate_t;
typedef DWORD(WINAPI *xgs_t)(DWORD, xstate_t *);

#define TRIG_ON        40
#define STICK_DZ       7849    /* default deadzone (~24%), [controls] deadzone */

/* Tuning carried over from the Frida build (user-calibrated). Speeds are in
 * angle units (4096 = 360 deg) per 1/60 s at full deflection. */
#define YAW_SPEED        28.0
#define PITCH_SPEED      20.0
#define PITCH_LIMIT_UP   448     /* camera collision keeps it above the floor */
#define PITCH_LIMIT_DOWN (-900)
#define FACE_OFFSET      2048    /* stick up = away from the camera */
#define FACE_SIGN        1
#define MOVE_START_GRACE 4
#define WALK_FRAC        0.55    /* stick tilt below this walks, above runs */
#define ROLL_SPEED_DEF   0x300   /* stock cartwheel speed is 0x200 (= running) */
#define ROLL_ANIM_DEF    50      /* % extra animation speed during the roll */
#define ROLL_TURN_STEP   0xA0    /* facing turn per update while the cartwheel lands (0x400 = 90 deg) */
#define WALK_MIN_PCT     40      /* walk speed/animation at the lightest tilt */
#define RUN_MIN_PCT      50      /* run speed/animation right past the walk range */
#define ROLL_BUFFER      12      /* player updates a roll press waits for a chance to start */
#define LOOK_ORBIT_MAX   100     /* the camera orbits up to this pitch; looking further up tilts in place */
#define ORBIT_SETTLE     20      /* camera calls the game camera keeps after a handover */
#define LOCK_CAM_EASE    0.12    /* per 1/60 s: how fast the camera swings behind Mega Man on lock-on */
#define LOCK_STEP_PCT    200     /* walk / backstep speed while locked on, % of stock */
#define LOCK_SIDESTEP_PCT 500    /* sidestep while locked on (stock 0x20 is 1/16 of a run) */
#define LOCK_TURN_STEP   0x100   /* max facing turn per update toward the target */
#define CAM_STEP         24      /* collision probe spacing along the camera line (world units) */
#define CAM_MARGIN       48      /* keep the camera this far in front of a hit */
#define CAM_MIN_DIST     80
#define CAM_EASE_OUT     0.12    /* fraction of the gap regained per update after a hit clears */
#define STALE_POLLS      30

#define PI 3.14159265358979323846

#define RD16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define RD32(a) (*(volatile int32_t *)(uintptr_t)(a))
#define WR32(a, v) (*(volatile int32_t *)(uintptr_t)(a) = (int32_t)(v))
#define PL(o)   (G_PLAYER + (o))
#define P8(o)   (*(volatile uint8_t *)PL(o))
#define P16(o)  (*(volatile uint16_t *)PL(o))
#define S16(o)  (*(volatile int16_t *)PL(o))
#define RIG8(o) (*(volatile uint8_t *)(uintptr_t)(G_CAM_RIG + (o)))

static int g_modern;

/* ---- button remapping ----
 * While the orbit camera drives, physical inputs are read as actions
 * ([controls] btn_a=jump ...). The game actions are then fed to the game as
 * the virtual buttons its own bindings expect (A Cross, B Triangle, X Square,
 * Y Circle, LB R2, Start, Back Select); roll, aim/lock-on and the orbit toggle
 * are ours. Menus and orbit-off tank controls keep the stock layout. */
enum { ACT_NONE, ACT_JUMP, ACT_SHOOT, ACT_SPECIAL, ACT_TALK, ACT_ROLL, ACT_AIM, ACT_ORBIT, ACT_MENU, ACT_SELECT,
       ACT_COUNT };
enum { PH_A, PH_B, PH_X, PH_Y, PH_LB, PH_RB, PH_LT, PH_RT, PH_L3, PH_R3, PH_BACK, PH_START, PH_COUNT };
static const char *const g_act_names[ACT_COUNT] = { "none", "jump", "shoot", "special", "talk", "roll", "aim",
                                                    "orbit", "menu", "select" };
static const char *const g_ph_keys[PH_COUNT] = { "btn_a", "btn_b", "btn_x", "btn_y", "btn_lb", "btn_rb", "btn_lt",
                                                 "btn_rt", "btn_l3", "btn_r3", "btn_back", "btn_start" };
static const WORD g_ph_btn[PH_COUNT] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, 0, 0, BTN_L3, BTN_R3,
                                         BTN_BACK, BTN_START };
static uint8_t g_map[PH_COUNT] = { ACT_JUMP, ACT_SPECIAL, ACT_ROLL, ACT_TALK, ACT_AIM, ACT_NONE, ACT_AIM, ACT_SHOOT,
                                   ACT_ORBIT, ACT_ORBIT, ACT_SELECT, ACT_MENU };
#define ACT(set, a) (((set) >> (a)) & 1u)

static unsigned actions_of(const pad_t *p)
{
    unsigned set = 0;
    int i;
    for (i = 0; i < PH_COUNT; i++) {
        int down = i == PH_LT ? p->bLeftTrigger > TRIG_ON
                 : i == PH_RT ? p->bRightTrigger > TRIG_ON
                 : (p->wButtons & g_ph_btn[i]) != 0;
        if (down)
            set |= 1u << g_map[i];
    }
    return set & ~(1u << ACT_NONE);
}

/* shared between the frame hook and the pad wrapper */
static volatile LONG cam_active, cam_suppress;
static volatile LONG move_active, move_fwd, move_walk, inject_fwd;
static volatile LONG dodge_req;
static volatile LONG lock_active, lock_free, lock_face;   /* lock_free: aim still held after the lock was lost */
static int g_lock_test;   /* Z-targeting: locked on, facing toward the target */
static volatile LONG frame_tick;
static volatile LONG move_gate;

/* frame-hook state */
static int cam_yaw;
static int move_face, roll_latch;
static int move_start_grace, move_prev;
static int32_t last_px, last_pz;
static int pos_move_credit;

/* roll (player_update hook) */
static int roll_ours, roll_dir, roll_anim_acc, roll_buffer;
static int g_roll_speed = ROLL_SPEED_DEF, g_roll_anim = ROLL_ANIM_DEF;
static double g_walk_frac = WALK_FRAC;
static int g_walk_min = WALK_MIN_PCT, g_run_min = RUN_MIN_PCT;
static int loco_anim_acc, loco_pct = 100;
static uint16_t talk_saved;
static int talk_hidden, walk_injected;

/* input state (this was the Python thread) */
static double st_yaw, st_pitch;
static int st_allowed = 1, flick_armed = 1;
/* launcher Controls tab: camera speed (signed: negative = inverted) and deadzone */
static double g_yaw_speed = YAW_SPEED, g_pitch_speed = PITCH_SPEED, g_stick_dz = STICK_DZ;
static double cam_dist_cur = -1;
static int32_t g_view_target[4];
static int st_active, prev_key, prev_click;
static LARGE_INTEGER qpc_last, qpc_freq;

/* XInput */
static xgs_t g_xgs_real, g_xgs_ex_real, g_xgs_own;

/* Debug remote pad: a named shared-memory block other tools can write to
 * (Local\MML_DEBUG_PAD). While `active`, its buttons are OR'd into pad 0
 * and any non-zero stick value replaces the physical one. Only created when
 * mml.ini has [debug] remote_pad=1. */
typedef struct {
    volatile LONG active;
    volatile WORD buttons;
    volatile BYTE lt, rt;
    volatile SHORT lx, ly, rx, ry;
} debug_pad_t;
static debug_pad_t *g_dbg;

int controls_debug_overlay(pad_t *p, int connected)
{
    if (!g_dbg || !g_dbg->active)
        return connected;
    if (!connected)   /* no physical pad (or it went to sleep): present a neutral one */
        memset(p, 0, sizeof *p);
    p->wButtons |= g_dbg->buttons;
    if (g_dbg->lt) p->bLeftTrigger = g_dbg->lt;
    if (g_dbg->rt) p->bRightTrigger = g_dbg->rt;
    if (g_dbg->lx) p->sThumbLX = g_dbg->lx;
    if (g_dbg->ly) p->sThumbLY = g_dbg->ly;
    if (g_dbg->rx) p->sThumbRX = g_dbg->rx;
    if (g_dbg->ry) p->sThumbRY = g_dbg->ry;
    return 1;
}

/* returns the (possibly faked) XInput result */
static DWORD debug_pad_apply(DWORD idx, xstate_t *s, DWORD r)
{
    if (idx != 0 || !s)
        return r;
    if (r != 0)
        memset(s, 0, sizeof *s);
    return controls_debug_overlay(&s->pad, r == 0) ? 0 : r;
}

void controls_debug_lock_test(void)
{
    g_lock_test = 1;
    lockon_test_reticle(1);
    log_printf("debug: lock-on test target = support object %08X", G_SUPPORT_OBJ);
}

void controls_debug_pad(void)
{
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(debug_pad_t),
                                  "Local\\MML_DEBUG_PAD");
    if (m)
        g_dbg = (debug_pad_t *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(debug_pad_t));
    log_printf("debug: remote pad %s", g_dbg ? "listening (Local\\MML_DEBUG_PAD)" : "failed");
}
static FARPROC(WINAPI *g_real_gpa)(HMODULE, LPCSTR);

static int iround(double v) { return (int)floor(v + 0.5); }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- reading the physical pad ---- */

static xgs_t own_xinput(void)
{
    static int tried;
    static const char *names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    int i;
    if (g_xgs_own || tried)
        return g_xgs_own;
    tried = 1;
    for (i = 0; i < 3; i++) {
        HMODULE m = LoadLibraryA(names[i]);
        if (m) {
            g_xgs_own = (xgs_t)(void *)GetProcAddress(m, "XInputGetState");
            if (g_xgs_own)
                break;
        }
    }
    return g_xgs_own;
}

static int pad_read(pad_t *out)
{
    xstate_t s;
    xgs_t f;
    if (input_sdl_active())
        return controls_debug_overlay(out, input_sdl_pad(out));
    f = g_xgs_real ? g_xgs_real : own_xinput();
    memset(out, 0, sizeof *out);
    if (!f || debug_pad_apply(0, &s, f(0, &s)) != 0)
        return 0;
    *out = s.pad;
    return 1;
}

static double stick_norm(int v)
{
    double a = v < 0 ? -(double)v : (double)v;
    if (a <= g_stick_dz)
        return 0;
    a = (a - g_stick_dz) / (32767.0 - g_stick_dz);
    if (a > 1)
        a = 1;
    return v < 0 ? -a : a;
}

static int signed12(int raw) { return raw > 2048 ? raw - 4096 : raw; }

/* the camera the game itself asked for this call (cam_apply_angles EBX yaw,
 * ECX -pitch); the stored angles at 0x77EA0A/0x77EA08 lag a frame and read 0
 * before a level's first camera */
static int g_game_yaw, g_game_pitch_ecx, g_game_cam_valid, g_game_cam_age = 1000;

static double game_yaw(void)
{
    return g_game_cam_valid ? (double)(g_game_yaw & 0xfff) : (double)RD16(G_CAM_YAW);
}

static double game_pitch(void)
{
    return g_game_cam_valid ? (double)-signed12(g_game_pitch_ecx & 0xfff) : (double)signed12(RD16(G_CAM_PITCH));
}

static int orbit_allowed(void)
{
    /* +4 rig active, +5 mode 0 (follow), +6 no script, +1 follow not suspended
     * (talking sets it and blends the camera to frame both speakers) */
    return RIG8(4) == 1 && RIG8(5) == 0 && RIG8(6) == 0 && RIG8(1) == 0 && !(P8(PL_INPUT_FLAGS) & 0x60) &&
           g_game_cam_age < 24;   /* the gameplay camera itself is running (not title/menus/other cameras) */
}

/* facing that looks from Mega Man at a world point (same as FUN_004DDE21) */
static int face_toward(int x, int z)
{
    double dx = (double)x - S16(0x14), dz = (double)z - S16(0x18);
    return (iround(atan2(dx, dz) * 2048.0 / PI) + 0x800) & 0xfff;
}

/* world_line_collision 0x42CE61 (watcall EAX p1, EDX p2, EBX p3, ECX p4):
 * despite the name, with p1 == p2 it tests one point (shorts x, y, z) against
 * the level's collision cells, walls and floor heights; 0 = free. The follow
 * camera calls it the same way (p3 0x5110B0, p4 0xFFFF). */
typedef uint32_t(__cdecl *world_collision_fn)(void *p1, void *p2, uint32_t p3, uint32_t p4);
static __declspec(naked) void world_collision_thunk(void)   /* called as world_collision_fn */
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax, [esp + 20]
        mov edx, [esp + 24]
        mov ebx, [esp + 28]
        mov ecx, [esp + 32]
        mov esi, G_FN_WORLD_COLLISION
        call esi
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

static uint32_t cam_probe(double x, double y, double z)
{
    int16_t pt[4];
    pt[0] = (int16_t)iround(x);
    pt[1] = (int16_t)iround(y);
    pt[2] = (int16_t)iround(z);
    pt[3] = 0;
    return ((world_collision_fn)(void *)world_collision_thunk)(pt, pt, G_CAM_COLLISION_PARAMS, 0xFFFF);
}

/* Same placement as cam_apply_angles: camera = target - dist * dir(yaw, p4)
 * with dir = (sin yaw cos p4, sin p4, cos yaw cos p4), 4096 units per turn.
 * Walk out from the target and stop in front of the first blocked point. */
static int cam_collide(const int32_t *t, int dist, int yaw, int p4)
{
    double a = (yaw & 0xfff) * (PI / 2048.0), b = signed12(p4 & 0xfff) * (PI / 2048.0);
    double dx = sin(a) * cos(b), dy = sin(b), dz = cos(a) * cos(b);
    int d;
    for (d = CAM_STEP; d <= dist + CAM_MARGIN; d += CAM_STEP) {
        if (cam_probe(t[0] - d * dx, t[1] - d * dy, t[2] - d * dz) != 0) {
            d -= CAM_MARGIN;
            return d < CAM_MIN_DIST ? CAM_MIN_DIST : d < dist ? d : dist;
        }
    }
    return dist;
}

static void input_update(void)
{
    pad_t p;
    LARGE_INTEGER now;
    double ticks = 0, n;
    int f5, click, aiming, moving = 0, fwd = 0, facing = 0;

    pad_read(&p);
    QueryPerformanceCounter(&now);
    if (qpc_last.QuadPart)
        ticks = clampd((double)(now.QuadPart - qpc_last.QuadPart) / qpc_freq.QuadPart, 0, 0.1) * 60.0;
    qpc_last = now;

    /* toggle: F5 or either stick click, edge-triggered */
    f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    click = ACT(actions_of(&p), ACT_ORBIT) != 0;
    if (g_modern && ((f5 && !prev_key) || (click && !prev_click))) {
        if (!st_active) {
            st_yaw = game_yaw();
            st_pitch = clampd(game_pitch(), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
            st_active = 1;
            log_printf("orbit ON (yaw %d pitch %d)", (int)st_yaw, (int)st_pitch);
        } else {
            st_active = 0;
            log_printf("orbit OFF");
        }
        toast_show(st_active);
    }
    prev_key = f5;
    prev_click = click;

    /* the orbit only drives the plain follow camera with the player in control;
     * cutscenes, scripted cameras, talk/boss framing get the game camera back */
    {
        /* coming back, the game camera keeps driving for a moment: right after a
         * load, a scene or a talk it is still blending in, so its angle (and Mega
         * Man's facing) isn't settled yet; then the orbit takes over from it */
        static int settle;
        int allowed = orbit_allowed();
        if (!allowed)
            settle = 0;
        else if (settle < ORBIT_SETTLE)
            settle++;
        allowed = allowed && (settle >= ORBIT_SETTLE || st_allowed);
        if (st_active && allowed && !st_allowed) {
            st_yaw = game_yaw();
            st_pitch = clampd(game_pitch(), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
            cam_dist_cur = -1;
            log_printf("orbit: back from the game camera (yaw %d pitch %d)", (int)st_yaw, (int)st_pitch);
        } else if (st_active && !allowed && st_allowed) {
            log_printf("orbit: handing over to the game camera (hold %d mode %d script %d input %02X)",
                       RIG8(1), RIG8(5), RIG8(6), P8(PL_INPUT_FLAGS));
        }
        st_allowed = allowed;
    }

    /* aiming with a target = lock-on (Z-targeting): Mega Man faces the target
     * and moves freely, the camera swings behind him; without a target, aiming
     * hands the camera back to the game (stock aim, rooted) and we keep
     * tracking it so release doesn't snap */
    aiming = ACT(actions_of(&p), ACT_AIM) != 0;
    {
        int hold = st_active && st_allowed && aiming, was = lock_active;
        /* a lock starts only where the game allows targeting (combat HUD up);
         * once it happened, the hold stays a lock-on hold until aim is released */
        lockon_want(hold && (lockon_had() || g_lock_test ||
                             (*(volatile uint8_t *)(uintptr_t)G_TARGET_FLAGS & 2)));
        lock_active = hold && (lockon_locked() || g_lock_test);
        lock_free = hold && !lock_active && lockon_had();
        if (lock_active) {
            int tx, ty, tz;
            double want, d;
            if (g_lock_test) {
                tx = *(volatile int16_t *)(uintptr_t)(G_SUPPORT_OBJ + 0x14);
                tz = *(volatile int16_t *)(uintptr_t)(G_SUPPORT_OBJ + 0x18);
            } else {
                lockon_pos(&tx, &ty, &tz);
            }
            lock_face = face_toward(tx, tz);
            want = (double)((lock_face - FACE_OFFSET) & 0xfff);
            d = fmod(want - fmod(st_yaw, 4096.0) + 6144.0, 4096.0) - 2048.0;
            st_yaw = fmod(st_yaw + d * (1.0 - pow(1.0 - LOCK_CAM_EASE, ticks)) + 65536.0, 65536.0);
            /* right stick flick: next enemy to that side (re-arms near centre) */
            n = stick_norm(p.sThumbRX);
            if (flick_armed && (n > 0.7 || n < -0.7)) {
                lockon_switch(n > 0 ? 1 : -1);
                flick_armed = 0;
            } else if (n > -0.3 && n < 0.3) {
                flick_armed = 1;
            }
        }
        if (lock_active && !was)
            log_printf("lock-on: on");
    }
    if (lock_active) {
        n = stick_norm(p.sThumbRY);
        if (n != 0)
            st_pitch = clampd(st_pitch + g_pitch_speed * n * ticks, PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
    } else if (st_active && aiming && !lock_free) {
        st_yaw = game_yaw();
        st_pitch = clampd(game_pitch(), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
    } else {
        n = stick_norm(p.sThumbRX);
        if (n != 0) {
            st_yaw = fmod(st_yaw + g_yaw_speed * n * ticks, 65536.0);
            if (st_yaw < 0)
                st_yaw += 65536.0;
        }
        n = stick_norm(p.sThumbRY);   /* stick up tilts the view up */
        if (n != 0)
            st_pitch = clampd(st_pitch + g_pitch_speed * n * ticks, PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
    }

    /* camera-relative movement on the left stick */
    if (st_active && st_allowed && (!aiming || lock_active || lock_free)) {
        double mag = sqrt((double)p.sThumbLX * p.sThumbLX + (double)p.sThumbLY * p.sThumbLY);
        if (mag > g_stick_dz) {
            double theta = atan2((double)p.sThumbLX, (double)p.sThumbLY);   /* 0 = up, + = right */
            int units = (int)(theta * 4096.0 / (2.0 * PI));
            facing = ((int)st_yaw + FACE_OFFSET + FACE_SIGN * units) & 0xffff;
            fwd = (int)(clampd((mag - g_stick_dz) / (32767.0 - g_stick_dz), 0, 1) * 32767);
            moving = 1;
        }
    }
    move_face = facing;
    move_fwd = fwd;
    move_walk = moving && fwd < (int)(g_walk_frac * 32767);
    move_active = moving;
    cam_yaw = (int)st_yaw & 0xffff;
    cam_active = st_active && st_allowed;
    cam_suppress = cam_active && aiming && !lock_active && !lock_free;
}

/* ---- per-frame hook: cam_apply_angles entry ---- */

static void write_facing(int angle)
{
    WR32(G_PLAYER_FACING, (uint32_t)(angle & 0xfff) << 16);
}

static void true_orbit(void)
{
    /* Rotate the camera offset so the camera sits behind the look direction,
     * not behind the player's facing (which is what cam_rig_yaw tracks). */
    int az = (int)RD32(G_CAM_RIG_YAW) & 0xfff;
    int yaw = cam_yaw & 0xfff;
    int delta = (yaw - az) & 0xfff;
    if (delta != 0) {
        int32_t tx = RD32(G_CAM_TARGET_X), tz = RD32(G_CAM_TARGET_Z);
        double ox = (double)RD32(G_CAM_POS_X) - tx, oz = (double)RD32(G_CAM_POS_Z) - tz;
        double rad = delta * (PI / 2048.0), cs = cos(rad), sn = sin(rad);
        WR32(G_CAM_POS_X, (int32_t)((int64_t)tx + iround(ox * cs - oz * sn)));
        WR32(G_CAM_POS_Z, (int32_t)((int64_t)tz + iround(ox * sn + oz * cs)));
        WR32(G_CAM_RIG_YAW, yaw);   /* idempotent if called twice in a frame */
    }
}

static void on_cam_apply(regs_t *r)
{
    int32_t px, pz;
    int gate, ma;

    frame_tick = (frame_tick + 1) & 0x3fffffff;
    {
        /* only the gameplay camera (camera_update's common tail) is "the game's
         * camera"; cam_follow_lookat & co. also come through here every frame
         * with angles of their own */
        uint32_t ret = *(uint32_t *)(uintptr_t)(r->esp + 4);
        if (ret == G_RET_CAM_COMMON_BLEND || ret == G_RET_CAM_COMMON) {
            g_game_yaw = (int)r->ebx;
            g_game_pitch_ecx = (int)r->ecx;
            g_game_cam_valid = 1;
            g_game_cam_age = 0;
        } else if (g_game_cam_age < 1000) {
            g_game_cam_age++;
        }
    }
    input_update();

    /* position-change credit: opens the facing gate at a standstill start,
     * stays closed while dialogue holds the player still */
    px = RD32(G_CAM_TARGET_X);
    pz = RD32(G_CAM_TARGET_Z);
    if (px != last_px || pz != last_pz)
        pos_move_credit = 4;
    else if (pos_move_credit > 0)
        pos_move_credit--;
    last_px = px;
    last_pz = pz;

    gate = move_gate;
    if (lock_active && !roll_latch && !(P8(PL_INPUT_FLAGS) & 0x60)) {
        int cur = (RD32(G_PLAYER_FACING) >> 16) & 0xfff;
        int d = ((lock_face - cur + 2048) & 0xfff) - 2048;
        if (d > LOCK_TURN_STEP) d = LOCK_TURN_STEP;
        if (d < -LOCK_TURN_STEP) d = -LOCK_TURN_STEP;
        if (d)
            write_facing(cur + d);
    } else if (move_active && !roll_latch && (gate > 0 || pos_move_credit > 0))
        write_facing(move_face);
    if (gate > 0)
        move_gate = gate - 1;

    /* forward injection only in gameplay (menus never move the player) */
    ma = move_active;
    if (ma && !move_prev)
        move_start_grace = MOVE_START_GRACE;
    move_prev = ma;
    inject_fwd = ma && (move_gate > 0 || pos_move_credit > 0 || move_start_grace > 0);
    if (move_start_grace > 0)
        move_start_grace--;


    if (cam_active && !cam_suppress) {
        const int32_t *t = (const int32_t *)(uintptr_t)r->eax;
        int pitch = iround(st_pitch), orbit = pitch > LOOK_ORBIT_MAX ? LOOK_ORBIT_MAX : pitch;
        int want, d;
        r->ebx = (uint32_t)cam_yaw;
        true_orbit();
        /* camera collision on the orbit position: pull in at once, ease back out */
        want = cam_collide(t, (int)r->edx, cam_yaw, -orbit);
        if (cam_dist_cur < 0 || want < cam_dist_cur)
            cam_dist_cur = want;
        else
            cam_dist_cur += (want - cam_dist_cur) * CAM_EASE_OUT;
        d = iround(cam_dist_cur);
        r->edx = (uint32_t)d;
        r->ecx = (uint32_t)(-pitch & 0xffff);   /* cam_apply_angles negates ECX */
        if (pitch > orbit) {
            /* looking up past the orbit range: the camera stays where the orbit
             * put it (above the floor) and only the view tilts further up.
             * cam_apply_angles places the camera at target - d * dir(pitch), so
             * hand it a virtual target d along the steeper view from there. */
            double a = (cam_yaw & 0xfff) * (PI / 2048.0);
            double bo = -orbit * (PI / 2048.0), bv = -pitch * (PI / 2048.0);
            double cx = t[0] - d * sin(a) * cos(bo), cy = t[1] - d * sin(bo), cz = t[2] - d * cos(a) * cos(bo);
            g_view_target[0] = iround(cx + d * sin(a) * cos(bv));
            g_view_target[1] = iround(cy + d * sin(bv));
            g_view_target[2] = iround(cz + d * cos(a) * cos(bv));
            g_view_target[3] = t[3];
            r->eax = (uint32_t)(uintptr_t)g_view_target;
        }
    } else {
        cam_dist_cur = -1;
    }
}

/* ---- player_update hook: roll + analog walk ----
 *
 * The roll is the game's own side cartwheel (state 9), entered directly
 * instead of through the jump crouch it normally comes from: state 9 sub 0
 * only waits for the current animation to end, so marking it ended
 * (+0xA7 = 0xFF) makes the game start the cartwheel this same update, with
 * its i-frames, sound and landing. The cartwheel travels at 90 degrees to
 * the facing (+0x116 = +-0x400), so the facing is set to the wanted heading
 * +-0x400, whichever side is closer to where Mega Man faces now. */


static int ang_dist(int a, int b)
{
    int d = (a - b) & 0xfff;
    return d > 2048 ? 4096 - d : d;
}

static int roll_try_start(void)
{
    int st = P8(PL_STATE), cur = (RD32(G_PLAYER_FACING) >> 16) & 0xfff, f, var;
    /* grounded movement states, or the landing pose of a previous roll (chaining) */
    if (st != 0 && st != 1 && st != 2 && st != 4 && st != 5 && st != 6 && !(st == 8 && P8(PL_SUBSTATE) >= 6))
        return 0;
    if (P8(PL_AIRBORNE) || (P8(PL_INPUT_FLAGS) & 0x60))
        return 0;
    if (move_active)
        f = move_face & 0xfff;
    else if (roll_ours)
        f = roll_dir;   /* chained, no stick: keep going the same way */
    else
        f = cur;        /* no stick: along the facing */
    var = ang_dist(f + 0x400, cur) <= ang_dist(f - 0x400, cur) ? 0 : 1;
    write_facing(var == 0 ? f + 0x400 : f - 0x400);
    P8(PL_RUN_DIR) = 0;
    P16(PL_MOVE_ANGLE) = 0;
    P16(0xB4) = 0;
    P16(PL_VEL_FWD) = 0;
    P16(PL_VEL_SIDE) = 0;
    P8(PL_STATE) = 9;
    P8(PL_SUBSTATE) = 0;
    P8(PL_VARIANT) = (uint8_t)var;
    P8(PL_ANIM_FLAGS) = 0xFF;   /* "current animation finished" */
    roll_ours = 1;
    roll_dir = f;
    roll_anim_acc = 0;
    return 1;
}

static void roll_tune(void)
{
    int st = P8(PL_STATE);
    if (st == 9) {
        int id = P8(PL_ANIM_ID);
        if (S16(PL_VEL_SIDE) == 0x200)
            S16(PL_VEL_SIDE) = (int16_t)g_roll_speed;
        else if (S16(PL_VEL_SIDE) == -0x200)
            S16(PL_VEL_SIDE) = (int16_t)-g_roll_speed;
        if (S16(PL_VEL_FWD) == 0x200)
            S16(PL_VEL_FWD) = (int16_t)g_roll_speed;
        /* play the cartwheel animations faster: shave hold ticks (never to 0,
         * anim_tick does the step) */
        if (id >= 0x18 && id <= 0x1C) {
            roll_anim_acc += g_roll_anim;
            while (roll_anim_acc >= 100) {
                roll_anim_acc -= 100;
                if (P8(PL_ANIM_HOLD) > 1)
                    P8(PL_ANIM_HOLD)--;
            }
        }
        /* landing: turn from side-on to the roll heading over a few updates,
         * keeping the travel line (travel = facing - +0x116) */
        if (P8(PL_SUBSTATE) >= 4) {
            int cur = (RD32(G_PLAYER_FACING) >> 16) & 0xfff;
            int d = ((roll_dir - cur + 2048) & 0xfff) - 2048;
            if (d > ROLL_TURN_STEP) d = ROLL_TURN_STEP;
            if (d < -ROLL_TURN_STEP) d = -ROLL_TURN_STEP;
            cur = (cur + d) & 0xfff;
            write_facing(cur);
            S16(PL_MOVE_ANGLE) = (int16_t)(((cur - roll_dir + 2048) & 0xfff) - 2048);
            /* the landing that follows (state 8 sub 6) picks its recovery
             * animation by run direction; the cartwheel left it at "sideways"
             * (0x2B/0x2C, a side-on body), we now face forward: 0x2A */
            P8(PL_RUN_DIR) = 0;
        }
    } else if (st == 8 && P8(PL_SUBSTATE) >= 6) {
        /* landing pose: the cartwheel left Mega Man side-on, face where he rolled */
        write_facing(roll_dir);
        /* moving the stick cancels it into a run */
        if (move_active) {
            P8(PL_STATE) = 0;
            P8(PL_SUBSTATE) = 0;
            P8(PL_VARIANT) = 0;
            P16(PL_VEL_FWD) = 0;
            P16(PL_VEL_SIDE) = 0;
            P16(PL_MOVE_ANGLE) = 0;
            P8(PL_RUN_DIR) = 0;
            P16(0xB4) = 0;
            P8(0x54A) = 0;
            P8(0x175) |= 1;
            P8(PL_ANIM_ID) = 0;
            P8(PL_ANIM_ENTRY) = 0;
            write_facing(move_face);
            roll_ours = 0;
        }
    } else {
        if (st == 7) {   /* rolled off a ledge: falling, face the roll heading */
            write_facing(roll_dir);
            S16(PL_MOVE_ANGLE) = 0;
        }
        roll_ours = 0;
    }
}

/* Analog speed: within the walk range the walk goes from WALK_MIN_PCT to its
 * full speed, past it the run goes from RUN_MIN_PCT to full speed. The
 * animation is slowed by the same factor so the feet keep up with the ground
 * (an extra hold tick now and then, the game's own per-tick blend does the rest). */
static void locomotion_tune(void)
{
    int st = P8(PL_STATE), id = P8(PL_ANIM_ID), pct;
    double n = move_fwd / 32767.0, w = g_walk_frac, t;
    loco_pct = 100;
    if ((st == 1 && P8(PL_SUBSTATE) >= 1 && (P8(PL_VARIANT) == 0 || lock_active)) ||
        (st == 6 && lock_active)) {
        t = w > 0 ? n / w : 1;
        if (t > 1) t = 1;
        pct = g_walk_min + (int)((100 - g_walk_min) * t);
        /* locked on, the walk-mode steps are how Mega Man moves while facing
         * the target; stock they crawl (the sidestep at 1/16 of a run) */
        if (lock_active)
            pct = pct * (st == 6 ? LOCK_SIDESTEP_PCT : LOCK_STEP_PCT) / 100;
        loco_pct = pct;
        if (id < 0x0C || id > 0x0F)
            return;
    } else if (st == 2 && P8(PL_SUBSTATE) >= 2 && (P8(PL_RUN_DIR) == 0 || lock_active)) {
        t = w < 1 ? (n - w) / (1 - w) : 1;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        pct = g_run_min + (int)((100 - g_run_min) * t);
        loco_pct = pct;
        if (id != 0x01 && id != 0x04)
            return;
    } else {
        loco_anim_acc = 0;
        return;
    }
    /* animation rate follows the speed: slower = extra hold ticks, faster =
     * shaved ones (never to 0, anim_tick steps on its own) */
    loco_anim_acc += 100 - pct;
    while (loco_anim_acc >= 100) {
        loco_anim_acc -= 100;
        if (P8(PL_ANIM_HOLD) < 0x7F)
            P8(PL_ANIM_HOLD)++;
    }
    while (loco_anim_acc <= -100) {
        loco_anim_acc += 100;
        if (P8(PL_ANIM_HOLD) > 1)
            P8(PL_ANIM_HOLD)--;
    }
}

static void on_player_update(regs_t *r)
{
    if (r->eax != G_PLAYER)
        return;
    if (talk_hidden) {
        P16(PL_MASK_TALK) = talk_saved;
        talk_hidden = 0;
    }
    if (!cam_active) {   /* orbit off: stock controls, but let a roll in flight finish tuned */
        if (roll_ours)
            roll_tune();
        roll_latch = roll_ours;
        return;
    }
    /* a press that can't start yet (mid-roll, mid-air) stays buffered for a few updates */
    if (dodge_req && !cam_suppress) {
        if (!roll_buffer)
            roll_buffer = ROLL_BUFFER;
        if (roll_try_start()) {
            dodge_req = 0;
            roll_buffer = 0;
        } else if (--roll_buffer == 0) {
            dodge_req = 0;
        }
    }
    if (roll_ours)
        roll_tune();
    roll_latch = roll_ours;

    loco_pct = 100;
    if (move_active && inject_fwd && !roll_ours)
        locomotion_tune();

    /* analog walk: a light tilt adds the walk modifier (Triangle) to what the
     * player holds, so the game picks its own walk state */
    if (walk_injected && !(*(volatile uint16_t *)G_PAD_HELD & P16(PL_MASK_WALK)))
        P16(PL_HELD) &= (uint16_t)~P16(PL_MASK_WALK);   /* in case held wasn't refreshed yet */
    walk_injected = 0;
    /* locked on, a light tilt walks too (sidestep / backstep facing the target),
     * but only from standing or walking: walk + strafe while running turns in place */
    if (move_walk && move_active && inject_fwd && !roll_ours && !(P8(PL_INPUT_FLAGS) & 0x60) &&
        (!lock_active || P8(PL_STATE) == 0 || P8(PL_STATE) == 1 || P8(PL_STATE) == 6)) {
        uint16_t walk = P16(PL_MASK_WALK);
        walk_injected = 1;
        P16(PL_HELD) |= walk;
        /* walk and talk share a button: an injected walk must not open a
         * conversation with whoever stands in front (the real button still does) */
        if (!(*(volatile uint16_t *)G_PAD_HELD & walk) && (P16(PL_MASK_TALK) & walk)) {
            talk_saved = P16(PL_MASK_TALK);
            P16(PL_MASK_TALK) = (uint16_t)(talk_saved & ~walk);
            talk_hidden = 1;
        }
    }
}

/* player_apply_velocity(EAX p, EDX fwd, EBX vert, ECX side): the walk state
 * re-enters itself every update (resetting its speed), so the analog speed
 * is applied here, where walk and run hand their speed to the game. */
static void on_apply_velocity(regs_t *r)
{
    int st;
    if (r->eax != G_PLAYER)
        return;
    st = P8(PL_STATE);
    /* locked on: the run state only knows 4 directions; steer the travel to
     * the exact stick heading (travel = facing - +0x116) */
    if (lock_active && move_active && st == 2) {
        int f = (RD32(G_PLAYER_FACING) >> 16) & 0xfff;
        S16(PL_MOVE_ANGLE) = (int16_t)(((f - move_face + 2048) & 0xfff) - 2048);
    }
    if (loco_pct == 100)
        return;
    if (st == 1 || st == 2)
        r->edx = (uint32_t)(int32_t)((int16_t)r->edx * loco_pct / 100);
    else if (st == 6)   /* sidestep moves on the side axis */
        r->ecx = (uint32_t)(int32_t)((int16_t)r->ecx * loco_pct / 100);
}

static void on_cam_set_b(regs_t *r)
{
    if (cam_active)
        r->edx = (uint32_t)cam_yaw;
}

static void on_player_move(regs_t *r)
{
    (void)r;
    move_gate = 6;
}

static void on_player_turn_ret(regs_t *r)
{
    (void)r;
    if (lock_active)
        return;
    if (move_active && !roll_latch && (move_gate > 0 || pos_move_credit > 0))
        write_facing(move_face);
}

/* ---- what the game sees from the pad ---- */

void controls_filter(pad_t *p)
{
    static int prev_roll, last_ft = -1, ft_stale, req_age;
    int ddx, ddy, fresh, roll, aiming;
    unsigned acts;
    WORD out;

    /* D-pad drives the left stick (menus + movement), in both control modes */
    ddx = ((p->wButtons & BTN_DPAD_RIGHT) ? 1 : 0) - ((p->wButtons & BTN_DPAD_LEFT) ? 1 : 0);
    ddy = ((p->wButtons & BTN_DPAD_UP) ? 1 : 0) - ((p->wButtons & BTN_DPAD_DOWN) ? 1 : 0);
    if (ddx || ddy) {
        p->sThumbLX = (SHORT)(ddx * 32767);
        p->sThumbLY = (SHORT)(ddy * 32767);
    }
    if (!cam_active)
        return;

    /* flags set by the frame hook go stale when it stops running (pause, transitions) */
    if (frame_tick == last_ft) {
        if (ft_stale < 1000)
            ft_stale++;
    } else {
        ft_stale = 0;
        last_ft = frame_tick;
    }
    fresh = ft_stale < STALE_POLLS;

    /* physical buttons -> actions -> the virtual buttons the game's bindings expect */
    acts = actions_of(p);
    roll = ACT(acts, ACT_ROLL) != 0;
    aiming = ACT(acts, ACT_AIM) != 0;
    p->bLeftTrigger = 0;
    p->bRightTrigger = 0;
    out = p->wButtons & (BTN_DPAD_UP | BTN_DPAD_DOWN | BTN_DPAD_LEFT | BTN_DPAD_RIGHT);
    if (ACT(acts, ACT_JUMP))    out |= BTN_A;      /* Cross */
    if (ACT(acts, ACT_SHOOT))   out |= BTN_X;      /* Square: buster */
    if (ACT(acts, ACT_SPECIAL)) out |= BTN_B;      /* Triangle: special weapon */
    if (ACT(acts, ACT_TALK))    out |= BTN_Y;      /* Circle: talk / check / walk */
    if (ACT(acts, ACT_MENU))    out |= BTN_START;
    if (ACT(acts, ACT_SELECT))  out |= BTN_BACK;
    if (aiming && !lock_active && !lock_free)   /* locked on: no stock aim state, it roots Mega Man */
        out |= BTN_LB;                             /* R2 */

    if (roll && !prev_roll && (!aiming || lock_active || lock_free)) {
        dodge_req = 1;
        req_age = 0;
    }
    prev_roll = roll;
    if (dodge_req) {
        if (++req_age > 60)   /* never consumed (menu): drop it, no phantom roll later */
            dodge_req = 0;
    } else {
        req_age = 0;
    }
    p->wButtons = out;

    if (lock_active) {
        /* the stick picks the game's run direction relative to the facing
         * (forward / strafe / back); the exact heading is set at apply time */
        p->sThumbLX = p->sThumbLY = 0;
        if (fresh && move_active && inject_fwd) {
            int rel = ((move_face - lock_face + 2048) & 0xfff) - 2048;
            if (rel >= -512 && rel <= 512)
                p->sThumbLY = 32767;
            else if (rel > 512 && rel <= 1536)
                p->sThumbLX = 32767;
            else if (rel < -512 && rel >= -1536)
                p->sThumbLX = -32767;
            else
                p->sThumbLY = -32767;
        }
    } else if (aiming && !lock_free) {
        SHORT lx = p->sThumbLX, ly = p->sThumbLY;
        p->sThumbLX = p->sThumbRX;
        p->sThumbLY = p->sThumbRY;
        p->sThumbRX = lx;
        p->sThumbRY = ly;
    } else if (fresh && move_active && inject_fwd) {
        p->sThumbLX = 0;
        p->sThumbLY = 32767;   /* walk vs run is decided in the player hook */
    }
}

static DWORD WINAPI hooked_xgs(DWORD idx, xstate_t *s)
{
    DWORD r = debug_pad_apply(idx, s, g_xgs_real(idx, s));
    if (r == 0 && s)
        controls_filter(&s->pad);
    return r;
}

static DWORD WINAPI hooked_xgs_ex(DWORD idx, xstate_t *s)
{
    DWORD r = debug_pad_apply(idx, s, g_xgs_ex_real(idx, s));
    if (r == 0 && s)
        controls_filter(&s->pad);
    return r;
}

static int is_xinput(HMODULE m)
{
    char name[MAX_PATH];
    DWORD n = GetModuleFileNameA(m, name, sizeof name);
    if (!n || n >= sizeof name)
        return 0;
    _strlwr_s(name, sizeof name);
    return strstr(name, "xinput") != NULL;
}

/* Xidi resolves XInput at runtime, so its GetProcAddress import is where we
 * hand it our wrapper instead. */
static FARPROC WINAPI hooked_gpa(HMODULE m, LPCSTR name)
{
    FARPROC f = g_real_gpa(m, name);
    if (!f)
        return f;
    if (IS_INTRESOURCE(name)) {
        if ((uintptr_t)name == 100 && is_xinput(m)) {   /* XInputGetStateEx */
            g_xgs_ex_real = (xgs_t)(void *)f;
            log_printf("controls: hooked XInputGetStateEx (ordinal 100)");
            return (FARPROC)(void *)hooked_xgs_ex;
        }
        return f;
    }
    if (strcmp(name, "XInputGetState") == 0) {
        g_xgs_real = (xgs_t)(void *)f;
        log_printf("controls: hooked XInputGetState");
        return (FARPROC)(void *)hooked_xgs;
    }
    return f;
}

/* ---- gamepad intro skip ---- */

static DWORD WINAPI intro_skip_thread(LPVOID unused)
{
    int i, seen = 0;
    (void)unused;
    for (i = 0; i < 60 * 20; i++) {   /* 60 s */
        int playing = *(volatile uint8_t *)G_INTRO_PLAYING;
        Sleep(50);
        if (seen && !playing)
            return 0;
        if (!playing)
            continue;
        seen = 1;
        {
            int down = input_sdl_any_button();
            if (down < 0) {
                xstate_t s;
                xgs_t f = own_xinput();
                down = f && f(0, &s) == 0 &&
                       (s.pad.wButtons || s.pad.bLeftTrigger > TRIG_ON || s.pad.bRightTrigger > TRIG_ON);
            }
            if (down) {
                *(volatile uint8_t *)G_INTRO_PLAYING = 0;
                log_printf("intro: skipped by gamepad");
                return 0;
            }
        }
    }
    return 0;
}

void controls_load_buttons(const char *ini)
{
    char v[32], line[256] = "";
    int i, a;
    for (i = 0; i < PH_COUNT; i++) {
        GetPrivateProfileStringA("controls", g_ph_keys[i], "", v, sizeof v, ini);
        if (!v[0])
            continue;
        for (a = 0; a < ACT_COUNT && _stricmp(v, g_act_names[a]) != 0; a++)
            ;
        if (a < ACT_COUNT)
            g_map[i] = (uint8_t)a;
        else
            log_printf("controls: unknown action \"%s\" for %s, kept %s", v, g_ph_keys[i], g_act_names[g_map[i]]);
    }
    for (i = 0; i < PH_COUNT; i++)
        sprintf_s(line + strlen(line), sizeof line - strlen(line), "%s%s=%s", i ? " " : "", g_ph_keys[i] + 4,
                  g_act_names[g_map[i]]);
    log_printf("controls: buttons %s", line);
}

void controls_camera(int speed_x_pct, int speed_y_pct, int invert_x, int invert_y, int deadzone_pct, int orbit_start)
{
    if (speed_x_pct > 0)
        g_yaw_speed = YAW_SPEED * speed_x_pct / 100.0;
    if (speed_y_pct > 0)
        g_pitch_speed = PITCH_SPEED * speed_y_pct / 100.0;
    if (invert_x)
        g_yaw_speed = -g_yaw_speed;
    if (invert_y)
        g_pitch_speed = -g_pitch_speed;
    if (deadzone_pct > 0 && deadzone_pct < 90)
        g_stick_dz = 32767.0 * deadzone_pct / 100.0;
    if (orbit_start && g_modern) {
        /* resyncs to the game camera on the first frame the orbit may drive */
        st_active = 1;
        st_allowed = 0;
    }
    log_printf("controls: camera speed %.0f/%.0f%s%s, deadzone %.0f%%, orbit %s at start",
               g_yaw_speed / YAW_SPEED * 100, g_pitch_speed / PITCH_SPEED * 100,
               invert_x ? " invert X" : "", invert_y ? " invert Y" : "", g_stick_dz / 327.67,
               st_active ? "on" : "off");
}

void controls_tune(int roll_speed, int roll_anim_pct, int walk_pct, int walk_min_pct, int run_min_pct)
{
    if (walk_min_pct >= 0 && walk_min_pct <= 100)
        g_walk_min = walk_min_pct;
    if (run_min_pct >= 0 && run_min_pct <= 100)
        g_run_min = run_min_pct;
    if (roll_speed > 0)
        g_roll_speed = roll_speed;
    if (roll_anim_pct >= 0)
        g_roll_anim = roll_anim_pct;
    if (walk_pct >= 0)
        g_walk_frac = walk_pct / 100.0;
    log_printf("controls: roll speed 0x%X, roll animation +%d%%, walk below %d%% tilt, walk from %d%%, run from %d%%",
               g_roll_speed, g_roll_anim, (int)(g_walk_frac * 100), g_walk_min, g_run_min);
}

/* ---- install ---- */

void controls_install(int modern, int intro_pad_skip)
{
    static const uint8_t cam_apply_bytes[] = { 0x56, 0x57, 0x55, 0x83, 0xEC, 0x1C };
    static const uint8_t cam_set_b_bytes[] = { 0x51, 0x56, 0x57, 0x83, 0xEC, 0x10 };
    static const uint8_t player_move_bytes[] = { 0x51, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x04 };
    static const uint8_t player_turn_ret_bytes[] = { 0x89, 0xC8, 0x5F, 0x5E, 0xC3 };
    static const uint8_t player_update_bytes[] = { 0x53, 0x51, 0x52, 0x56, 0x57, 0x55 };
    static const uint8_t apply_velocity_bytes[] = { 0x56, 0x89, 0xC6, 0x66, 0x8B, 0x80, 0x16, 0x01, 0x00, 0x00 };
    HMODULE xidi = GetModuleHandleA("dinput.dll");

    g_modern = modern;
    QueryPerformanceFrequency(&qpc_freq);

    if (GetModuleHandleA("xinput1_4.dll") || GetModuleHandleA("xinput1_3.dll") ||
        GetModuleHandleA("xinput9_1_0.dll"))
        log_printf("controls: warning, XInput already loaded before us");
    if (xidi && iat_patch(xidi, "kernel32.dll", "GetProcAddress", (void *)hooked_gpa, (void **)&g_real_gpa))
        log_printf("controls: watching dinput.dll (Xidi) for XInput");
    else
        log_printf("controls: dinput.dll has no GetProcAddress import, pad remap unavailable");

    if (hook_mid(G_FN_CAM_APPLY, sizeof cam_apply_bytes, cam_apply_bytes, on_cam_apply) &&
        hook_mid(G_FN_CAM_SET_B, sizeof cam_set_b_bytes, cam_set_b_bytes, on_cam_set_b) &&
        hook_mid(G_FN_PLAYER_MOVE, sizeof player_move_bytes, player_move_bytes, on_player_move) &&
        hook_mid(G_SITE_PLAYER_TURN_RET, sizeof player_turn_ret_bytes, player_turn_ret_bytes, on_player_turn_ret) &&
        hook_mid(G_FN_PLAYER_UPDATE, sizeof player_update_bytes, player_update_bytes, on_player_update) &&
        hook_mid(G_FN_PLAYER_APPLY_VEL, sizeof apply_velocity_bytes, apply_velocity_bytes, on_apply_velocity))
        log_printf("controls: %s (F5 / L3 / R3 toggles the orbit camera)", modern ? "modern" : "vanilla");
    else
        log_printf("controls: game hooks failed");

    if (intro_pad_skip)
        CloseHandle(CreateThread(NULL, 0, intro_skip_thread, NULL, 0, NULL));
}
