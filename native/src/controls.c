/* Modern controls, ported from the Frida runtime.
 *
 * Where things run:
 *   cam_apply_angles hook (once per game frame, game thread)
 *       reads the pad, integrates the orbit camera, computes the movement
 *       facing, writes the player facing, steers dodge rolls, runs the dodge
 *       trigger state machine and overrides the camera yaw/pitch arguments.
 *   XInputGetState wrapper (whenever Xidi polls the pad)
 *       rewrites what the game sees: D-pad -> left stick, button remap,
 *       stick swap while aiming, forward injection while moving, and the
 *       synthesized LT+A that triggers a roll.
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
#include <string.h>

#include "controls.h"
#include "game.h"
#include "hook.h"
#include "iat.h"
#include "input.h"
#include "log.h"
#include "toast.h"

typedef struct {
    DWORD dwPacketNumber;
    pad_t pad;
} xstate_t;
typedef DWORD(WINAPI *xgs_t)(DWORD, xstate_t *);

#define TRIG_ON        40
#define STICK_DZ       7849

/* Tuning carried over from the Frida build (user-calibrated). Speeds are in
 * angle units (4096 = 360 deg) per 1/60 s at full deflection. */
#define YAW_SPEED        28.0
#define PITCH_SPEED      20.0
#define PITCH_LIMIT_UP   57      /* looking up clips through the floor, keep it tight */
#define PITCH_LIMIT_DOWN (-900)
#define FACE_OFFSET      2048    /* stick up = away from the camera */
#define FACE_SIGN        1
#define MOVE_START_GRACE 4
#define ROLL_MOVE_MIN    200000.0    /* 16.16 world delta (~3 units) */
#define DODGE_LEAD       3       /* frames of LT held before the A tap */
#define DODGE_A_FRAMES   2
#define DODGE_ZERO_LEAD  2       /* zero the stick only this close to the A tap */
#define DODGE_TOTAL      (DODGE_LEAD + DODGE_A_FRAMES + 1)
#define DODGE_STEER_WIN  35      /* frames a dodge may steer the roll */
#define STALE_POLLS      30

#define PI 3.14159265358979323846

#define RD16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define RD32(a) (*(volatile int32_t *)(uintptr_t)(a))
#define WR32(a, v) (*(volatile int32_t *)(uintptr_t)(a) = (int32_t)(v))

static int g_modern;

/* shared between the frame hook and the pad wrapper */
static volatile LONG cam_active, cam_suppress;
static volatile LONG move_active, move_fwd, inject_fwd;
static volatile LONG dodge_req, dodge_inj_lt, dodge_inj_a, dodge_zero;
static volatile LONG frame_tick;
static volatile LONG move_gate;

/* frame-hook state */
static int cam_yaw, cam_pitch_ecx;
static int move_face, roll_latch;
static int move_start_grace, move_prev;
static int32_t last_px, last_pz;
static int pos_move_credit;
static int32_t roll_last_x, roll_last_z;
static int roll_walk_ang, roll_was_rolling, roll_has_moved;
static int dodge_phase, roll_steer_win;

/* input state (this was the Python thread) */
static double st_yaw, st_pitch;
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
    if (a <= STICK_DZ)
        return 0;
    a = (a - STICK_DZ) / (32767.0 - STICK_DZ);
    if (a > 1)
        a = 1;
    return v < 0 ? -a : a;
}

static int signed12(int raw) { return raw > 2048 ? raw - 4096 : raw; }

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
    click = (p.wButtons & (BTN_L3 | BTN_R3)) != 0;
    if (g_modern && ((f5 && !prev_key) || (click && !prev_click))) {
        if (!st_active) {
            st_yaw = RD16(G_CAM_YAW);
            st_pitch = clampd(signed12(RD16(G_CAM_PITCH)), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
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

    /* aiming hands the camera back to the game; keep tracking it so release doesn't snap */
    aiming = (p.wButtons & BTN_LB) || p.bLeftTrigger > TRIG_ON;
    if (st_active && aiming) {
        st_yaw = RD16(G_CAM_YAW);
        st_pitch = clampd(signed12(RD16(G_CAM_PITCH)), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
    } else {
        n = stick_norm(p.sThumbRX);
        if (n != 0) {
            st_yaw = fmod(st_yaw + YAW_SPEED * n * ticks, 65536.0);
            if (st_yaw < 0)
                st_yaw += 65536.0;
        }
        n = stick_norm(p.sThumbRY);   /* stick up tilts the view up */
        if (n != 0)
            st_pitch = clampd(st_pitch + PITCH_SPEED * n * ticks, PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
    }

    /* camera-relative movement on the left stick */
    if (st_active && !aiming) {
        double mag = sqrt((double)p.sThumbLX * p.sThumbLX + (double)p.sThumbLY * p.sThumbLY);
        if (mag > STICK_DZ) {
            double theta = atan2((double)p.sThumbLX, (double)p.sThumbLY);   /* 0 = up, + = right */
            int units = (int)(theta * 4096.0 / (2.0 * PI));
            facing = ((int)st_yaw + FACE_OFFSET + FACE_SIGN * units) & 0xffff;
            fwd = (int)(clampd((mag - STICK_DZ) / (32767.0 - STICK_DZ), 0, 1) * 32767);
            moving = 1;
        }
    }
    move_face = facing;
    move_fwd = fwd;
    move_active = moving;
    cam_yaw = (int)st_yaw & 0xffff;
    cam_pitch_ecx = (-(int)st_pitch) & 0xffff;   /* cam_apply_angles negates ECX */
    cam_active = st_active;
    cam_suppress = st_active && aiming;
}

/* ---- per-frame hook: cam_apply_angles entry ---- */

static void write_facing(int angle)
{
    WR32(G_PLAYER_FACING, (uint32_t)(angle & 0xfff) << 16);
}

static void roll_steer(void)
{
    int32_t px = RD32(G_PLAYER_X), pz = RD32(G_PLAYER_Z);
    uint32_t st = (uint32_t)RD32(G_PLAYER_STATE);
    /* bit 0x800 is also set while aiming and on jump landings, hence the extra gates */
    int rolling = (st & 0x800) && !cam_suppress && roll_steer_win > 0;
    double dx = (double)px - roll_last_x, dz = (double)pz - roll_last_z;
    double moved2 = dx * dx + dz * dz;
    double min2 = ROLL_MOVE_MIN * ROLL_MOVE_MIN;

    roll_latch = rolling;
    if (rolling) {
        int stopped;
        if (!roll_was_rolling)
            roll_has_moved = 0;
        if (moved2 > min2)
            roll_has_moved = 1;
        stopped = roll_has_moved && moved2 <= min2;
        if (stopped)
            write_facing(roll_walk_ang + 2048);   /* recovery: face the travel direction */
        else   /* the engine rolls along facing -/+ 90; pre-rotate so it lands on the walk heading */
            write_facing(roll_walk_ang + (((st >> 24) & 1) ? 1024 : -1024));
    } else {
        if (roll_was_rolling)
            write_facing(roll_walk_ang + 2048);
        /* keep learning the walk heading so it's fresh when a roll starts */
        if (moved2 > min2)
            roll_walk_ang = iround(atan2(dx, dz) * 2048.0 / PI) & 0xfff;
    }
    roll_last_x = px;
    roll_last_z = pz;
    roll_was_rolling = rolling;
}

static void dodge_machine(void)
{
    /* only start from a neutral, grounded state */
    if (dodge_req && dodge_phase == 0) {
        if (!(RD32(G_PLAYER_STATE) & 0x800)) {
            dodge_phase = DODGE_TOTAL;
            roll_steer_win = DODGE_STEER_WIN;
        }
        dodge_req = 0;
    }
    if (roll_steer_win > 0)
        roll_steer_win--;
    if (dodge_phase > 0) {
        int df = DODGE_TOTAL - dodge_phase;
        dodge_inj_lt = 1;
        dodge_inj_a = df >= DODGE_LEAD && df < DODGE_LEAD + DODGE_A_FRAMES;
        dodge_zero = df >= DODGE_LEAD - DODGE_ZERO_LEAD;
        dodge_phase--;
    } else {
        dodge_inj_lt = dodge_inj_a = dodge_zero = 0;
    }
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
    if (move_active && !roll_latch && (gate > 0 || pos_move_credit > 0))
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

    roll_steer();
    dodge_machine();

    if (cam_active && !cam_suppress) {
        r->ebx = (uint32_t)cam_yaw;
        r->ecx = (uint32_t)cam_pitch_ecx;
        true_orbit();
    }
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
    if (move_active && !roll_latch && (move_gate > 0 || pos_move_credit > 0))
        write_facing(move_face);
}

/* ---- what the game sees from the pad ---- */

void controls_filter(pad_t *p)
{
    static int prev_x, last_ft = -1, ft_stale, req_age;
    int ddx, ddy, fresh, phys_x, phys_lt, phys_rt, aiming;
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

    phys_x = (p->wButtons & BTN_X) != 0;
    phys_lt = p->bLeftTrigger > TRIG_ON;
    phys_rt = p->bRightTrigger > TRIG_ON;

    /* triggers no longer rotate the camera; RT shoots, LT (or LB) aims, X dodges */
    p->bLeftTrigger = 0;
    p->bRightTrigger = 0;
    out = p->wButtons & ~BTN_X;
    if (phys_rt)
        out |= BTN_X;
    aiming = phys_lt || (p->wButtons & BTN_LB);
    if (aiming)
        out |= BTN_LB;

    if (phys_x && !prev_x && !aiming) {
        dodge_req = 1;
        req_age = 0;
    }
    prev_x = phys_x;
    if (dodge_req) {
        if (++req_age > 20)   /* never consumed (menu): drop it, no phantom roll later */
            dodge_req = 0;
    } else {
        req_age = 0;
    }
    if (fresh && dodge_inj_lt)
        p->bLeftTrigger = 255;
    if (fresh && dodge_inj_a)
        out |= BTN_A;
    p->wButtons = out;

    if (fresh && dodge_zero) {
        p->sThumbLX = p->sThumbLY = 0;
    } else if (aiming) {
        SHORT lx = p->sThumbLX, ly = p->sThumbLY;
        p->sThumbLX = p->sThumbRX;
        p->sThumbLY = p->sThumbRY;
        p->sThumbRX = lx;
        p->sThumbRY = ly;
    } else if (fresh && move_active && inject_fwd) {
        p->sThumbLX = 0;
        p->sThumbLY = (SHORT)move_fwd;
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

/* ---- install ---- */

void controls_install(int modern, int intro_pad_skip)
{
    static const uint8_t cam_apply_bytes[] = { 0x56, 0x57, 0x55, 0x83, 0xEC, 0x1C };
    static const uint8_t cam_set_b_bytes[] = { 0x51, 0x56, 0x57, 0x83, 0xEC, 0x10 };
    static const uint8_t player_move_bytes[] = { 0x51, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x04 };
    static const uint8_t player_turn_ret_bytes[] = { 0x89, 0xC8, 0x5F, 0x5E, 0xC3 };
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
        hook_mid(G_SITE_PLAYER_TURN_RET, sizeof player_turn_ret_bytes, player_turn_ret_bytes, on_player_turn_ret))
        log_printf("controls: %s (F5 / L3 / R3 toggles the orbit camera)", modern ? "modern" : "vanilla");
    else
        log_printf("controls: game hooks failed");

    if (intro_pad_skip)
        CloseHandle(CreateThread(NULL, 0, intro_skip_thread, NULL, 0, NULL));
}
