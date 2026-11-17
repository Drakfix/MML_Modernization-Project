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
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "controls.h"
#include "game.h"
#include "guest.h"
#include "hook.h"
#include "iat.h"
#include "kbm.h"
#include "input.h"
#include "lockon.h"
#include "mml.h"
#include "log.h"
#include "padprompt.h"
#include "recomp.h"
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
/* over-the-shoulder lock-on camera ([controls] lockon_camera=shoulder) */
#define SH_SIDE          110     /* world units the view point slides beside Mega Man */
#define SH_LIFT          25      /* ...and up (y grows downward) */
#define SH_DIST          0.62    /* camera distance, share of the normal one */
#define SH_DIST_HIP      0.82    /* camera_shoulder (always over the shoulder), not aiming: a little closer */
#define ADS_ZOOM         1.3     /* camera_shoulder, aiming: the view zooms in this much (like a shooter's ADS) */
#define ADS_EASE         0.2     /* per 1/60 s, in and out */
#define TIGHT_ROOM       0.75    /* less free room behind the shoulder view than this share of its distance: tight */
#define TIGHT_CENTER     0.7     /* ...then the view slides back toward the middle by up to this share */
#define TIGHT_LIFT       70      /* ...and up over his head by up to this much (y grows downward) */
#define COAST_FRAMES     8       /* 1/60 s: in a burst, a run goes on through a stick gap this short */
#define BODY_EASE        0.35    /* per update: the strafe run's body turn eases toward its travel */
#define SH_EASE          0.14    /* per 1/60 s: easing into / out of the shoulder framing */
#define SH_SWAP_EASE     0.16    /* per 1/60 s: sliding across on a shoulder swap */
#define SH_PITCH_EASE    0.08    /* per 1/60 s: tilting toward the enemy's height */
#define SH_CHEST         100     /* the view point's height above Mega Man's position */
#define SH_LEVEL_TIME    24      /* 1/60 s: shoulder aim levels the view toward the horizon at first */
#define LOCK_STEP_PCT    200     /* walk / backstep speed while locked on, % of stock */
#define LOCK_SIDESTEP_PCT 500    /* sidestep while locked on (stock 0x20 is 1/16 of a run) */
#define WS_SIDE_SIGN     1       /* side axis of player_apply_velocity: rel > 0 (right of the facing) = this * -side */
#define BACKPEDAL_PCT    225     /* the backpedal at full tilt, % of the stock walk 0x50 (LOCK_STEP_PCT at the walk's
                                    edge): 225 = 35% of a run; its legs then step ~1.3x as fast as the run's
                                    (640 = the run's speed needed 3.7x: a frantic shuffle) */
#define ANIM_RATE_MAX    8       /* at most this many poses per update when an animation is sped up */
#define MOVE_EASE        0.4     /* face-hold movement: share of the way to the stick's velocity per update (30/s):
                                    ~0.1 s to settle on a change of direction or speed */
#define BACKJOG_PCT      55      /* backpedal=jog: its speed at full tilt, % of a run */
#define G_ANIM_SCRIPTS   0x765764u   /* player body animations 0x00-0x3F: script per id (anim_start) */
#define RUN_BACK_EASE    683     /* in a burst the strafe run slows from this angle off the facing (60 deg)... */
#define RUN_BACK_EDGE    1229    /* ...to the backpedal's speed where the backpedal takes over (108 deg: past it
                                    the strafe run's body would turn away from the target; a pure sideways
                                    strafe sits at 90-103 deg with the shoulder camera's aim, so the edge stays
                                    clear of it, or strafing flickered between the run and the backpedal) */
#define BACK_HYST        57      /* +-5 deg around RUN_BACK_EDGE, so the stick near it doesn't flip states */
#define LOCK_TURN_STEP   0x100   /* max facing turn per update toward the target */
#define AIM_HOLD         36      /* camera aim: 1/60 s it stays on after the fire button is let go */
#define AIM_TURN_STEP    0x400   /* camera aim: max facing turn per update (180 deg in 2 updates) */
#define AIM_FIRE_TOL     0x100   /* camera aim: a shot waits until the facing is this close... */
#define AIM_FIRE_WAIT    6       /* ...or this many updates have passed */
#define CAM_STEP         24      /* collision probe spacing along the camera line (world units) */
#define CAM_MARGIN       48      /* keep the camera this far in front of a hit */
#define CAM_MIN_DIST     80
#define CAM_EASE_OUT     0.12    /* fraction of the gap regained per update after a hit clears */
#define STALE_POLLS      30
/* Jet Skates (state 3). Stock speeds: 0x400 stick forward, 0x340 coasting, 0x280 braking. Rates
 * are per update at 30 updates/s (frame step 2) and scale with the frame step. */
#define SKATE_STOCK      0x400
#define SKATE_MAX_DEF    130     /* % of the stock top speed at full tilt */
#define SKATE_MIN_DEF    25      /* % of the stock top speed at the lightest tilt */
#define SKATE_ACCEL      0x48    /* speed gained per update toward what the stick asks */
#define SKATE_DECEL      0x40    /* speed lost per update when it asks for less (or nothing) */
#define SKATE_TURN_STEP  0xC0    /* max facing turn per update (0x400 = 90 deg) */
#define SKATE_SHARP      0x500   /* a turn wider than this slows to the low speed while carving */
#define SKATE_LEAN_MIN   0x30    /* a turn of this much per update leans into it */
#define SKATE_LEAN_HOLD  6       /* updates a lean stays after the turn eases off */
#define SKATE_BRAKE_HOLD 8       /* updates a turn-around at speed skids (stock brake: smoke at the feet) */

#define PI 3.14159265358979323846

#define RD16(a) GVAR(uint16_t, a)
#define RD32(a) GVAR(int32_t, a)
#define WR32(a, v) (GVAR(int32_t, a) = (int32_t)(v))
#define PL(o)   (G_PLAYER + (o))
#define P8(o)   GVAR(uint8_t, PL(o))
#define P16(o)  GVAR(uint16_t, PL(o))
#define S16(o)  GVAR(int16_t, PL(o))
#define RIG8(o) GVAR(uint8_t, G_CAM_RIG + (o))

static int g_modern;

/* ---- button remapping ----
 * While the orbit camera drives, physical inputs are read as actions
 * ([controls] btn_a=jump ...). The game actions are then fed to the game as
 * the virtual buttons its own bindings expect (A Cross, B Triangle, X Square,
 * Y Circle, LB R2, Start, Back Select); roll, aim/lock-on and the orbit toggle
 * are ours. Menus and orbit-off tank controls keep the stock layout. */
enum { ACT_NONE, ACT_JUMP, ACT_SHOOT, ACT_SPECIAL, ACT_TALK, ACT_ROLL, ACT_AIM, ACT_ORBIT, ACT_MENU, ACT_SELECT,
       ACT_SKATES, ACT_SHOULDER, ACT_COUNT };
enum { PH_A, PH_B, PH_X, PH_Y, PH_LB, PH_RB, PH_LT, PH_RT, PH_L3, PH_R3, PH_BACK, PH_START, PH_COUNT };
static const char *const g_act_names[ACT_COUNT] = { "none", "jump", "shoot", "special", "talk", "roll", "aim",
                                                    "orbit", "menu", "select", "skates", "shoulder" };
static const char *const g_ph_keys[PH_COUNT] = { "btn_a", "btn_b", "btn_x", "btn_y", "btn_lb", "btn_rb", "btn_lt",
                                                 "btn_rt", "btn_l3", "btn_r3", "btn_back", "btn_start" };
static const WORD g_ph_btn[PH_COUNT] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, 0, 0, BTN_L3, BTN_R3,
                                         BTN_BACK, BTN_START };
static uint8_t g_map[PH_COUNT] = { ACT_JUMP, ACT_SPECIAL, ACT_ROLL, ACT_TALK, ACT_AIM, ACT_SKATES, ACT_AIM, ACT_SHOOT,
                                   ACT_ORBIT, ACT_SHOULDER, ACT_SELECT, ACT_MENU };
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

/* keyboard + mouse (orbit layout): bindable slots, two keys each ([controls] key_*) */
enum { KS_UP, KS_DOWN, KS_LEFT, KS_RIGHT, KS_WALK, KS_JUMP, KS_SHOOT, KS_AIM, KS_ROLL, KS_TALK, KS_SPECIAL,
       KS_MENU, KS_SELECT, KS_ORBIT, KS_SKATES, KS_SHOULDER, KS_COUNT };
static const struct { const char *key; uint8_t act; int def0, def1; } g_slot_info[KS_COUNT] = {
    { "key_up", ACT_NONE, 'W', VK_UP },          { "key_down", ACT_NONE, 'S', VK_DOWN },
    { "key_left", ACT_NONE, 'A', VK_LEFT },      { "key_right", ACT_NONE, 'D', VK_RIGHT },
    { "key_walk", ACT_NONE, VK_LCONTROL, 0 },    { "key_jump", ACT_JUMP, VK_SPACE, 0 },
    { "key_shoot", ACT_SHOOT, VK_LBUTTON, 0 },   { "key_aim", ACT_AIM, VK_RBUTTON, 0 },
    { "key_roll", ACT_ROLL, VK_LSHIFT, 0 },      { "key_talk", ACT_TALK, 'E', 0 },
    { "key_special", ACT_SPECIAL, 'Q', 0 },      { "key_menu", ACT_MENU, VK_TAB, 0 },
    { "key_select", ACT_SELECT, 'M', 0 },        { "key_orbit", ACT_ORBIT, VK_F5, 0 },
    { "key_skates", ACT_SKATES, 'C', VK_XBUTTON1 },
    { "key_shoulder", ACT_SHOULDER, VK_MBUTTON, 0 },
};
static int g_slot_vk[KS_COUNT][2];

static int slot_down(int slot)
{
    return (g_slot_vk[slot][0] && kbm_key(g_slot_vk[slot][0])) || (g_slot_vk[slot][1] && kbm_key(g_slot_vk[slot][1]));
}
#define MOUSE_YAW_PER_COUNT   0.75   /* angle units (4096 = 360 deg) per raw mouse count at 100% */
#define MOUSE_PITCH_PER_COUNT 0.60
#define KBM_WALK_TILT         0.35   /* Left Ctrl: a light tilt, the walk range */
static int g_kbm = 1, g_mouse_invert_y;
static double g_mouse_scale = 1.0;
static volatile LONG g_kbm_acts;   /* actions from keys/buttons, refreshed every camera update */
static volatile LONG g_kbm_lx, g_kbm_ly;     /* WASD as a stick, for the stock aim's turn/tilt */
static volatile LONG g_aim_mdx, g_aim_mdy;   /* mouse motion for the stock (rooted) aim */
#define AIM_PITCH_LIMIT       0x1C0  /* the aim state's own clamp on +0x108 */
#define AIM_PITCH_SIGN        (-1)   /* +0x108 grows downward: mouse up = aim up (checked in game) */

static unsigned kbm_actions(void)
{
    unsigned set = 0;
    int i;
    for (i = KS_JUMP; i < KS_COUNT; i++)
        if (g_slot_info[i].act != ACT_NONE && slot_down(i))
            set |= 1u << g_slot_info[i].act;
    return set;
}

/* WASD as a left stick (camera-relative like the stick); 0 if no key */
static int kbm_move(SHORT *lx, SHORT *ly)
{
    int x = (slot_down(KS_RIGHT) ? 1 : 0) - (slot_down(KS_LEFT) ? 1 : 0);
    int y = (slot_down(KS_UP) ? 1 : 0) - (slot_down(KS_DOWN) ? 1 : 0);
    double m;
    if (!x && !y)
        return 0;
    m = (slot_down(KS_WALK) ? KBM_WALK_TILT : 1.0) * 32767.0 / ((x && y) ? 1.41421356 : 1.0);
    *lx = (SHORT)(x * m);
    *ly = (SHORT)(y * m);
    return 1;
}

int controls_kbm_active(void)
{
    return g_modern && g_kbm && kbm_foreground();
}

/* shared between the frame hook and the pad wrapper */
static volatile LONG cam_active, cam_suppress;
static volatile LONG move_active, move_fwd, move_walk, inject_fwd;
static volatile LONG backpedal;   /* in a face hold, the stick back: walk back facing the target, any tilt */
static int fire_btn;   /* a fire button held where Mega Man can shoot (any aim mode) */
static volatile LONG combat;   /* a face hold during a burst with walk_shoot: moving every way while shooting */
static int ws_in_state;        /* the state handler is running (its player_apply_velocity, not the weapon's) */
static unsigned ws_update;     /* player updates counted (before the pre-handler) */
static int g_backjog;          /* [controls] backpedal=jog: the backpedal past a light tilt is the run played backward */
static int g_walk_shoot = 1, ws_hooked;   /* walking while shooting ([controls] walk_shoot; its hooks in) */
static volatile LONG dodge_req;
static volatile LONG lock_active, lock_free, lock_face;   /* lock_free: aim still held after the lock was lost */
static int g_lock_test;   /* Z-targeting: locked on, facing toward the target */
/* face hold: Mega Man keeps facing face_dir and moves freely around it (strafe, backstep); lock-on
   faces the target, camera aim ([controls] aim=camera) faces where the camera looks while firing */
static volatile LONG face_hold, face_dir, cam_aim, aim_ready;
static int g_aim_camera, g_crosshair;   /* crosshair: 0 off, 1 while aiming, 2 always (with camera aim) */
static int g_aim_free;     /* [controls] aim_button=free: the aim button never locks on, it aims over the shoulder */
static int g_sh_always;    /* [controls] camera_shoulder: the camera over the shoulder all the time; aiming zooms in */
static double ads_zoom = 1.0, sh_close;   /* the aiming zoom (camera_shoulder); 0..1 the aiming framing (eased) */
static double sh_tight;    /* 0..1: how cramped the room behind the shoulder view is (eased) */
#define FADE_NEAR        240     /* camera to his chest: at this distance or closer Mega Man is FADE_MIN opaque (a
                                    wall right behind him: ~160-240; the shoulder camera in the open ~650,
                                    aiming ~490)... */
#define FADE_FAR         380     /* ...and fully opaque from here */
#define FADE_MIN         0.25
#define FADE_EASE        0.2     /* per 1/60 s */
static int g_fade_on = 1, g_fade_log;   /* [controls] camera_fade; (testing: MML_FADE_LOG=1) */
static volatile LONG g_player_alpha = 255;
static int g_aim_assist;   /* [controls] aim_assist: slower camera and a tighter crosshair over an enemy */
#define AIM_ASSIST_SLOW  0.45    /* camera speed while the crosshair is over an enemy, with aim assist */
#define AIM_ASSIST_EASE  0.25    /* per 1/60 s toward it and back */
static double aim_hold;
static double since_fire = 1e9;   /* 1/60 s since the last shot with camera aim on (crosshair) */

/* Vertical aim. Every shot spawner (the buster's FUN_004B08D5 and the special weapons) builds its
 * velocity from a pitch it gets from FUN_0042A85C: the aim camera's pitch [0x77E964] when the aim
 * camera is up ([0x77E958] != 0), else 0 (level). That function's two operands are retargeted to
 * a flag and a pitch of ours, which mirror the game's values except while camera-aiming, when
 * they hold the pitch from Mega Man's chest to the point the camera looks at (AIM_DIST along the
 * view): the crosshair is drawn there too. The game's globals stay untouched (the camera and many
 * others read [0x77E964]). */
#define G_FN_SHOT_PITCH   GA(0x42A85Cu)   /* cmp byte [0x77E958],0 / ... / mov eax,[0x77E964] at +0xC */
#define G_AIMCAM_ON       0x77E958u
#define G_AIMCAM_PITCH    0x77E964u
#define G_CAM_PLACED      0x77E9E8u   /* s32 x, y, z: where cam_apply_angles put the camera */
#define G_CAM_PITCH_NOW   0x77EA08u
#define G_CAM_YAW_NOW     0x77EA0Au
#define AIM_DIST          2000.0      /* the aim point's distance when the view ray isn't cast */
#define AIM_RAY_MAX       3500.0      /* the view ray's reach (nothing hit: the point out there) */
#define AIM_RAY_STEP      64.0        /* march step (then 4 halvings: ~4 units) */
#define AIM_RAY_SKIP      40.0        /* the ray starts this far past Mega Man (not the space behind him) */
#define AIM_ENEMY_R       90.0        /* an enemy's hit volume radius for the ray */
#define AIM_FACE_MIN      250.0       /* closer than this, he faces the camera's heading instead */
#define AIM_CHEST         100         /* the buster's height above the player's position (y down) */
#define AIM_PITCH_MAX     0x300       /* shots no steeper than ~67 degrees */
#define SHOT_PITCH_SIGN   (1)         /* the spawners' pitch: + = ... (checked in game) */
static volatile uint8_t *g_shot_flag;
static volatile int32_t *g_shot_pitch;
static double aim_px, aim_py, aim_pz;   /* where the camera looks (world), refreshed per camera update */
static volatile LONG aim_face;          /* the facing that sends shots to that point */
static volatile LONG lock_tx, lock_ty, lock_tz;   /* the locked target (world), for the shots' pitch */
static volatile LONG aim_on_enemy;      /* the view ray stopped at an enemy (crosshair feedback) */
static int g_aim_sens = 60;             /* % camera speed while shoulder-aiming ([controls] aim_sensitivity) */
static LONG aim_since;   /* frame_tick when camera aim started (the shot waits for the turn) */
static int air_travel, air_steered;   /* a jump's heading; +0x116 was set for it in a face hold */
static int g_sh_mode, g_sh_side = 1;  /* over-the-shoulder lock-on camera; side +1 right, -1 left */
/* shoulder aim: with the shoulder camera on, aiming with nothing locked = the shoulder camera
   with free movement (instead of the original aim state, which roots Mega Man) */
static volatile LONG sh_aim;
static double sh_aim_age;   /* 1/60 s since shoulder aim began (the view levels at first) */
static double sh_blend, sh_side_cur = 1.0;   /* eased: 0 classic .. 1 shoulder; -1 left .. +1 right */
static int32_t *g_sh_target;          /* the view point handed to cam_apply_angles: in guest memory */
static char g_ini_path[MAX_PATH];     /* (the shoulder side is remembered there) */
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

/* Jet Skates: the skates button (held, or toggled) skates whenever Mega Man could stand or run */
static volatile LONG skate_want;
static int g_skate_max = SKATE_MAX_DEF, g_skate_min = SKATE_MIN_DEF, g_skate_toggle;
static int skate_speed;            /* forward speed we hand the game while skating (and in a skate jump) */
static int skate_air;              /* in a jump that started on skates */
static int skate_turn;             /* facing change of the last camera update (- left, + right) */
static int skate_lean, skate_lean_hold;   /* carving: -1 left, +1 right (held a few updates) */
static int skate_anim_acc;         /* skating loop: hold ticks owed (its rate follows the speed) */
static int skate_brake;            /* updates left of a turn-around skid: the stock brake (smoke at the feet) */
static uint16_t skate_inj_held, skate_inj_pressed;   /* stock turn / brake buttons we handed the game */

/* input state (this was the Python thread) */
static double st_yaw, st_pitch;
static int st_allowed = 1, flick_armed = 1;   /* st_allowed: the player has the plain follow camera's control */
static int cam_owned = 1;                      /* ...and the orbit drives the camera (after the settle) */
static volatile LONG cam_follow;               /* modern controls on, the game camera still settling */
static volatile LONG cam_fresh;                /* the camera updates paused (a room load): take over at once */
/* the menus' Controls page: camera speed (signed: negative = inverted) and deadzone */
static double g_yaw_speed = YAW_SPEED, g_pitch_speed = PITCH_SPEED, g_stick_dz = STICK_DZ;
static double cam_dist_cur = -1;
static int32_t *g_view_target;   /* handed to the game (EAX): in guest memory */
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

#ifdef _WIN32
void controls_debug_pad(void)
{
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(debug_pad_t),
                                  "Local\\MML_DEBUG_PAD");
    if (m)
        g_dbg = (debug_pad_t *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(debug_pad_t));
    log_printf("debug: remote pad %s", g_dbg ? "listening (Local\\MML_DEBUG_PAD)" : "failed");
}
#else
/* Elsewhere the block arrives over a socket: a Unix socket in the abstract namespace ("mml_pad";
   from a PC, adb forward tcp:<port> localabstract:mml_pad), one 16-byte copy of the block per
   message. No network permission is needed. A dropped connection releases the pad. */
#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static debug_pad_t g_dbg_block;

static void *debug_pad_thread(void *p)
{
    int srv = (int)(intptr_t)p;
    for (;;) {
        int c = accept(srv, NULL, NULL);
        unsigned char msg[sizeof(debug_pad_t)];
        size_t have = 0;
        if (c < 0)
            continue;
        for (;;) {
            ssize_t n = read(c, msg + have, sizeof msg - have);
            if (n <= 0)
                break;
            have += (size_t)n;
            if (have == sizeof msg) {
                memcpy((void *)&g_dbg_block, msg, sizeof msg);
                have = 0;
            }
        }
        close(c);
        g_dbg_block.active = 0;
    }
    return NULL;
}

void controls_debug_pad(void)
{
    struct sockaddr_un a;
    pthread_t t;
    int srv;
    if (g_dbg)
        return;   /* (the start menu set it up already) */
    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path + 1, "mml_pad", 7);   /* (abstract: a leading 0) */
    if (srv < 0 || bind(srv, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 8)) ||
        listen(srv, 1) || pthread_create(&t, NULL, debug_pad_thread, (void *)(intptr_t)srv)) {
        log_printf("debug: remote pad failed");
        return;
    }
    pthread_detach(t);
    g_dbg = &g_dbg_block;
    log_printf("debug: remote pad listening (abstract socket mml_pad)");
}
#endif
static FARPROC(WINAPI *g_real_gpa)(HMODULE, LPCSTR);

static int iround(double v) { return (int)floor(v + 0.5); }

/* game updates per 1/60 s step: 2 = 30 updates/s */
static int frame_step(void)
{
    int s = GVAR(uint8_t, G_FRAME_STEP);
    return s < 1 ? 1 : s > 4 ? 4 : s;
}
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
static LARGE_INTEGER g_last_cam_qpc;   /* last camera update: menus don't run the camera at all */

/* the orbit is really driving right now: on, and the game camera ran in the last 0.1 s
 * (cam_active alone goes stale while a menu is open) */
static int orbit_live(void)
{
    LARGE_INTEGER now;
    if (!cam_active || !g_last_cam_qpc.QuadPart || !qpc_freq.QuadPart)
        return 0;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_last_cam_qpc.QuadPart) * 10 < qpc_freq.QuadPart;
}

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
#ifndef MML_RUNNER
typedef uint32_t(__cdecl *world_collision_fn)(void *p1, void *p2, uint32_t p3, uint32_t p4);
static uint32_t g_fn_world_collision;   /* G_FN_WORLD_COLLISION in the running program (controls_install) */
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
        mov esi, g_fn_world_collision
        call esi
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}
#endif

static uint32_t cam_probe(double x, double y, double z)
{
    static int16_t *pt;   /* the point the game reads: in guest memory */
    if (!pt)
        pt = (int16_t *)GUEST(guest_alloc(8));
    pt[0] = (int16_t)iround(x);
    pt[1] = (int16_t)iround(y);
    pt[2] = (int16_t)iround(z);
    pt[3] = 0;
    if (recomp_in_shim()) {   /* the recompiled game: its own world_line_collision, no machine code */
        uint32_t regs[4];
        regs[0] = regs[1] = GADDR(pt), regs[2] = G_CAM_COLLISION_PARAMS, regs[3] = 0xFFFF;
        return recomp_call_watcall(G_FN_WORLD_COLLISION, regs, 0, NULL);
    }
#ifndef MML_RUNNER
    return ((world_collision_fn)(void *)world_collision_thunk)(pt, pt, G_CAM_COLLISION_PARAMS, 0xFFFF);
#else
    return 0;   /* (never: the runner always runs the game recompiled) */
#endif
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

/* ---- rail shooting (the boat rides) ----
 * G_RAIL_AIM == 1 forces the aim state (14) on while the boat drives: Mega Man only turns, tilts
 * the aim and shoots (player_try_aim 0x4ACF2E; pre_14 never leaves it then). The camera is
 * scripted, so the orbit stands aside, and the pad used to reach the game in the stock layout
 * (X shoots, the stick turns at the stock digital rate, RT does nothing). In modern mode the
 * sticks and the mouse turn and tilt with analog speed, the modern buttons shoot, and the aim
 * action zooms the view in (slower aim while zoomed, crosshair shown). The game's own aim button
 * (R2) would turn Mega Man to the nearest enemy here (FUN_004ABAE7 -> the target branch of
 * player_state_14), but the targets are mostly beyond its pick range, so it isn't handed over.
 * Without R2 the state turns by
 * the turn masks (FUN_004B5429, up to 0x48 per update) and tilts +0x108 by the fwd/back masks
 * (FUN_004B52F0, up to 0x40 per update, clamp +-0x200); we hand it neither and write facing and
 * +0x108 ourselves, with their 16-bit fractions so slow tilts stay smooth. */
#define RAIL_YAW_SPEED    36.0    /* angle units per 1/60 s at full tilt (stock top speed ~0x48 / 2) */
#define RAIL_PITCH_SPEED  26.0
#define RAIL_PITCH_LIMIT  0x200   /* the manual aim's own clamp in this mode (FUN_004B52F0) */
#define RAIL_ZOOM         1.5     /* view zoom while the aim action is held */
#define RAIL_ZOOM_EASE    0.22    /* fraction of the way to the wanted zoom per 1/60 s */
static LARGE_INTEGER g_rail_qpc;              /* last player update in rail mode (game thread) */
static volatile LONG g_rail_mdx, g_rail_mdy;  /* mouse counts not applied yet */
static volatile LONG g_rail_sx, g_rail_sy;    /* the aiming stick (right, else left / D-pad, else WASD) */
static volatile LONG g_rail_zoom_want;        /* the aim action is held: zoom in */
static double rail_zoom = 1.0;                /* current zoom (eased), game thread */
static double rail_since_fire = 1e9;          /* 1/60 s since the last shot in rail mode (crosshair) */
static double rail_px, rail_py, rail_pz;      /* where the shots go: first enemy or wall on their path */
static volatile LONG rail_on_enemy;

static int rail_mode(void)
{
    return GVAR(uint8_t, G_RAIL_AIM) == 1 && P8(PL_STATE) == 14 && !(P8(PL_INPUT_FLAGS) & 0x60);
}

/* modern controls are on and the rail aim ran in the last 0.1 s (menus stop the player updates) */
static int rail_live(void)
{
    LARGE_INTEGER now;
    if (!g_modern || !st_active || !g_rail_qpc.QuadPart || !qpc_freq.QuadPart)
        return 0;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_rail_qpc.QuadPart) * 10 < qpc_freq.QuadPart;
}

/* pad poll (game thread): the modern buttons as the virtual buttons the game's bindings expect;
   the sticks are ours (rail_apply), the game gets none */
static void rail_filter(pad_t *p)
{
    unsigned acts = actions_of(p);
    int sx = 0, sy = 0;
    WORD out = 0;
    if (abs(p->sThumbRX) > g_stick_dz || abs(p->sThumbRY) > g_stick_dz)
        sx = p->sThumbRX, sy = p->sThumbRY;
    else if (abs(p->sThumbLX) > g_stick_dz || abs(p->sThumbLY) > g_stick_dz)
        sx = p->sThumbLX, sy = p->sThumbLY;   /* (the D-pad is on the left stick by now) */
    if (g_kbm && kbm_foreground()) {
        SHORT kx, ky;
        int mdx = 0, mdy = 0, mw = 0;
        kbm_attach_auto();
        acts |= kbm_actions();
        kbm_take_mouse(&mdx, &mdy, &mw);
        InterlockedExchangeAdd(&g_rail_mdx, mdx);
        InterlockedExchangeAdd(&g_rail_mdy, mdy);
        if (!sx && !sy && kbm_move(&kx, &ky))
            sx = kx, sy = ky;
    }
    g_rail_sx = sx;
    g_rail_sy = sy;
    g_rail_zoom_want = ACT(acts, ACT_AIM) != 0;
    if (ACT(acts, ACT_JUMP))    out |= BTN_A;
    if (ACT(acts, ACT_SHOOT))   out |= BTN_X;
    if (ACT(acts, ACT_SPECIAL)) out |= BTN_B;
    if (ACT(acts, ACT_TALK))    out |= BTN_Y;
    if (ACT(acts, ACT_MENU))    out |= BTN_START;
    if (ACT(acts, ACT_SELECT))  out |= BTN_BACK;
    p->wButtons = out;
    p->bLeftTrigger = p->bRightTrigger = 0;
    p->sThumbLX = p->sThumbLY = p->sThumbRX = p->sThumbRY = 0;
}

/* back to no zoom (leaving the rail aim, or modern controls off) */
static void rail_zoom_reset(void)
{
    if (rail_zoom != 1.0) {
        rail_zoom = 1.0;
        mml_zoom(1.0f);
    }
}

/* player update (game thread), in rail mode */
static void rail_apply(void)
{
    double k = frame_step(), yaw, pitch, want, sens;
    int dx = (int)InterlockedExchange(&g_rail_mdx, 0), dy = (int)InterlockedExchange(&g_rail_mdy, 0);
    int64_t pv;
    QueryPerformanceCounter(&g_rail_qpc);
    if (!g_modern || !st_active) {
        rail_zoom_reset();
        return;
    }
    /* zoom: eased in and out; the aim slows toward the shoulder-aim sensitivity as it zooms */
    want = g_rail_zoom_want ? RAIL_ZOOM : 1.0;
    rail_zoom += (want - rail_zoom) * (1.0 - pow(1.0 - RAIL_ZOOM_EASE, k));
    if (fabs(rail_zoom - want) < 0.002)
        rail_zoom = want;
    mml_zoom((float)rail_zoom);
    sens = 1.0 + (g_aim_sens / 100.0 - 1.0) * (rail_zoom - 1.0) / (RAIL_ZOOM - 1.0);
    yaw = sens * (RAIL_YAW_SPEED * (g_yaw_speed / YAW_SPEED) * stick_norm(g_rail_sx) * k +
                  dx * MOUSE_YAW_PER_COUNT * g_mouse_scale);
    pitch = sens * (AIM_PITCH_SIGN * RAIL_PITCH_SPEED * (g_pitch_speed / PITCH_SPEED) * stick_norm(g_rail_sy) * k -
                    dy * MOUSE_PITCH_PER_COUNT * g_mouse_scale * AIM_PITCH_SIGN * (g_mouse_invert_y ? -1 : 1));
    if (yaw != 0) {   /* facing: 12-bit angle . 16-bit fraction */
        uint32_t f = (uint32_t)RD32(G_PLAYER_FACING) + (uint32_t)(int32_t)floor(yaw * 65536.0 + 0.5);
        WR32(G_PLAYER_FACING, f & 0x0FFFFFFFu);
    }
    if (pitch != 0) {   /* +0x108 with its fraction at +0x106 */
        pv = (int64_t)GVAR(int32_t, PL(0x106)) + (int64_t)floor(pitch * 65536.0 + 0.5);
        if (pv > (int64_t)RAIL_PITCH_LIMIT << 16) pv = (int64_t)RAIL_PITCH_LIMIT << 16;
        if (pv < -((int64_t)RAIL_PITCH_LIMIT << 16)) pv = -((int64_t)RAIL_PITCH_LIMIT << 16);
        GVAR(int32_t, PL(0x106)) = (int32_t)pv;
    }
}

/* player update (game thread), in rail mode: the crosshair. The shots leave the buster along the
   facing (travel = facing - 0x800) at the aim angle +0x108 (+ = down, y grows downward; the aim
   camera hands the spawners 0x77E964 = +0x108 here), so the crosshair sits where that path first
   meets an enemy or the level, and shows for a while after each shot like the camera-aim one */
static void rail_crosshair(void)
{
    double a = ((((RD32(G_PLAYER_FACING) >> 16) & 0xfff) - 0x800) & 0xfff) * (PI / 2048.0);
    double p = S16(PL_AIM_PITCH) * (PI / 2048.0);
    double fx = sin(a) * cos(p), fy = sin(p), fz = cos(a) * cos(p);
    double ox = S16(0x14), oy = S16(0x16) - AIM_CHEST, oz = S16(0x18), dist = AIM_RAY_MAX, s;
    int vol[64][3], n, i, on_enemy = 0;
    rail_since_fire = (P8(PL_FIRE_FLAGS) & 0xC0) ? 0 : rail_since_fire < 1e9 ? rail_since_fire + frame_step() : 1e9;
    if (rail_since_fire >= 600 && rail_zoom <= 1.0)
        return;
    n = lockon_volumes(vol, 64);
    for (i = 0; i < n; i++) {   /* enemies: the nearest whose volume the path passes through */
        double rx = vol[i][0] - ox, ry = vol[i][1] - oy, rz = vol[i][2] - oz;
        double t = rx * fx + ry * fy + rz * fz, d2 = rx * rx + ry * ry + rz * rz - t * t;
        if (t > AIM_RAY_SKIP && d2 < AIM_ENEMY_R * AIM_ENEMY_R) {
            t -= sqrt(AIM_ENEMY_R * AIM_ENEMY_R - d2);
            if (t < dist) {
                dist = t < AIM_RAY_SKIP ? AIM_RAY_SKIP : t;
                on_enemy = 1;
            }
        }
    }
    for (s = AIM_RAY_SKIP; s < dist; s += AIM_RAY_STEP) {   /* the level */
        if (cam_probe(ox + fx * s, oy + fy * s, oz + fz * s) != 0) {
            dist = s;
            on_enemy = 0;
            break;
        }
    }
    rail_px = ox + fx * dist;
    rail_py = oy + fy * dist;
    rail_pz = oz + fz * dist;
    rail_on_enemy = on_enemy;
}

static void input_update(void)
{
    pad_t p;
    LARGE_INTEGER now;
    double ticks = 0, n;
    int f5, click, aiming, moving = 0, fwd = 0, facing = 0, mdx = 0, mdy = 0, mwheel = 0, firing;
    double aim_scale;

    pad_read(&p);
    if (g_kbm && g_modern) {
        SHORT kx, ky;
        kbm_attach_auto();
        kbm_take_mouse(&mdx, &mdy, &mwheel);
        if (rail_live()) {   /* the boat rides: the mouse aims there (rail_apply) */
            InterlockedExchangeAdd(&g_rail_mdx, mdx);
            InterlockedExchangeAdd(&g_rail_mdy, mdy);
            mdx = mdy = mwheel = 0;
        }
        g_kbm_acts = (LONG)kbm_actions();
        if (kbm_move(&kx, &ky)) {   /* keys win over a resting stick */
            p.sThumbLX = kx;
            p.sThumbLY = ky;
        } else {
            kx = ky = 0;
        }
        g_kbm_lx = kx;
        g_kbm_ly = ky;
    } else {
        g_kbm_acts = 0;
    }
    QueryPerformanceCounter(&now);
    if (qpc_last.QuadPart)
        ticks = clampd((double)(now.QuadPart - qpc_last.QuadPart) / qpc_freq.QuadPart, 0, 0.1) * 60.0;
    qpc_last = now;

    /* toggle: F5 or either stick click, edge-triggered */
    f5 = g_kbm ? 0 : (GetAsyncKeyState(VK_F5) & 0x8000) != 0;   /* with KB+M on: key_orbit (default F5) */
    click = ACT(actions_of(&p) | (unsigned)g_kbm_acts, ACT_ORBIT) != 0;
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

    /* shoulder swap (R3 by default): the over-the-shoulder lock-on camera's side, remembered */
    {
        static int prev_sh;
        int sh = ACT(actions_of(&p) | (unsigned)g_kbm_acts, ACT_SHOULDER) != 0;
        if (g_modern && (g_sh_mode || g_aim_free || g_sh_always) && sh && !prev_sh) {
            g_sh_side = -g_sh_side;
            if (g_ini_path[0])
                WritePrivateProfileStringA("controls", "shoulder", g_sh_side > 0 ? "right" : "left", g_ini_path);
            log_printf("lock-on camera: %s shoulder", g_sh_side > 0 ? "right" : "left");
        }
        prev_sh = sh;
    }

    /* the orbit only drives the plain follow camera with the player in control;
     * cutscenes, scripted cameras, talk/boss framing get the game camera back */
    {
        /* coming back, the game camera keeps driving for a moment: right after a
         * load, a scene or a talk it is still blending in, so its angle (and Mega
         * Man's facing) isn't settled yet; then the orbit takes over from it.
         * The controls don't wait for it: the player has control again, so the
         * buttons and the movement are the modern ones right away (moving relative
         * to the game camera, which the orbit follows meanwhile). Before, the raw
         * pad reached the game in that window (about 0.7 s after every room load):
         * the shoot trigger turned Mega Man (the original layout's turn button).
         * Turning the camera, aiming or moving takes it over at once (moving relative
         * to a camera that still swings walked him in a curve).
         * After a room load (the camera updates paused, cam_fresh) the game camera
         * swings in from where the last room's was, while the screen fades in: the
         * orbit takes over on the first update instead, straight behind Mega Man. A
         * talk or a script before control returns (a scene on arrival) clears that:
         * then the game camera framed something on purpose and settles as before. */
        static int settle;
        int allowed = orbit_allowed(), own, cam_input;
        if (!allowed)
            settle = 0;
        else if (settle < ORBIT_SETTLE)
            settle++;
        if (!allowed && ((g_game_cam_age == 0 && (RIG8(1) || RIG8(5) || RIG8(6))) ||
                         (g_game_cam_age > 4 && g_game_cam_age < 1000)))   /* (another camera: a scene) */
            cam_fresh = 0;
        cam_input = stick_norm(p.sThumbRX) != 0 || stick_norm(p.sThumbRY) != 0 || mdx || mdy || mwheel ||
                    ACT(actions_of(&p) | (unsigned)g_kbm_acts, ACT_AIM) ||
                    sqrt((double)p.sThumbLX * p.sThumbLX + (double)p.sThumbLY * p.sThumbLY) > g_stick_dz;
        own = allowed && (settle >= ORBIT_SETTLE || cam_owned || cam_input || cam_fresh);
        if (st_active && own && !cam_owned) {
            if (cam_fresh) {   /* after a load: behind him (the follow camera's rest: facing + 0x800) */
                st_yaw = (double)((((RD32(G_PLAYER_FACING) >> 16) & 0xfff) + 0x800) & 0xfff);
                st_pitch = clampd(game_pitch(), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
            } else {
                st_yaw = game_yaw();
                st_pitch = clampd(game_pitch(), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
            }
            cam_dist_cur = -1;
            log_printf("orbit: back from the game camera (yaw %d pitch %d%s)", (int)st_yaw, (int)st_pitch,
                       cam_fresh ? ", behind him after a load" : "");
        } else if (st_active && !allowed && st_allowed) {
            log_printf("orbit: handing over to the game camera (hold %d mode %d script %d input %02X)",
                       RIG8(1), RIG8(5), RIG8(6), P8(PL_INPUT_FLAGS));
        }
        if (st_active && allowed && !own) {   /* settling: the movement follows the game camera */
            st_yaw = game_yaw();
            st_pitch = clampd(game_pitch(), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
        }
        st_allowed = allowed;
        cam_owned = own;
        if (own)
            cam_fresh = 0;
        cam_follow = st_active && allowed && !own;
    }

    /* aiming with a target = lock-on (Z-targeting): Mega Man faces the target
     * and moves freely, the camera swings behind him; without a target, aiming
     * hands the camera back to the game (stock aim, rooted) and we keep
     * tracking it so release doesn't snap */
    aiming = ACT(actions_of(&p) | (unsigned)g_kbm_acts, ACT_AIM) != 0;
    {
        int hold = st_active && st_allowed && aiming, was = lock_active;
        /* a lock starts only where the game allows targeting (combat HUD up);
         * once it happened, the hold stays a lock-on hold until aim is released */
        /* (free aim: the aim button never locks on) */
        lockon_want(!g_aim_free && hold && (lockon_had() || g_lock_test ||
                                            (GVAR(uint8_t, G_TARGET_FLAGS) & 2)));
        lock_active = !g_aim_free && hold && (lockon_locked() || g_lock_test);
        lock_free = !g_aim_free && hold && !lock_active && lockon_had();
        if (lock_active) {
            int tx, ty, tz;
            double want, d;
            if (g_lock_test) {
                tx = GVAR(int16_t, G_SUPPORT_OBJ + 0x14);
                ty = GVAR(int16_t, G_SUPPORT_OBJ + 0x16);
                tz = GVAR(int16_t, G_SUPPORT_OBJ + 0x18);
            } else {
                lockon_pos(&tx, &ty, &tz);
            }
            lock_face = face_toward(tx, tz);
            lock_tx = tx, lock_ty = ty, lock_tz = tz;
            want = (double)((lock_face - FACE_OFFSET) & 0xfff);
            if (g_sh_mode) {
                /* over the shoulder: look from the shoulder point to the enemy (the view point
                   sits beside Mega Man, so aim the camera from there to keep the enemy centred),
                   and tilt toward the enemy's height */
                double a = fmod(st_yaw, 4096.0) * (PI / 2048.0), lat = sh_side_cur * SH_SIDE * sh_blend;
                double sx = S16(0x14) + cos(a) * lat, sz = S16(0x18) - sin(a) * lat;
                double sy = S16(0x16) - SH_CHEST - SH_LIFT * sh_blend;
                double h = sqrt((tx - sx) * (tx - sx) + (tz - sz) * (tz - sz)), pt;
                want = fmod(atan2(tx - sx, tz - sz) * (2048.0 / PI) + 4096.0, 4096.0);
                pt = clampd(atan2(sy - ty, h) * (2048.0 / PI), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
                st_pitch += (pt - st_pitch) * sh_blend * (1.0 - pow(1.0 - SH_PITCH_EASE, ticks));
            }
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
        /* shoulder aim: nothing locked (no target, or the lock was lost) with the shoulder camera on;
           with free aim, every time the aim button is held */
        {
            int was_sh = sh_aim;
            sh_aim = (g_sh_mode || g_aim_free) && hold && !lock_active;
            sh_aim_age = sh_aim && was_sh ? sh_aim_age + ticks : 0;
            if (sh_aim && !was_sh)
                log_printf("shoulder aim: on");
        }
    }
    /* camera aim: firing turns Mega Man to where the camera looks and he keeps moving freely
     * around that direction (like lock-on), for a moment after the last shot too; otherwise
     * movement stays all-round. The aim button keeps lock-on / the stock aim. */
    {
        unsigned acts = actions_of(&p) | (unsigned)g_kbm_acts;
        /* (not while the game camera still settles after a load or a talk: it is still swinging
           around, so shots go where Mega Man faces, as in the original) */
        int ok = g_aim_camera && st_active && st_allowed && !cam_follow && !aiming, was = cam_aim;
        int pst = P8(PL_STATE), can_shoot = !(P8(PL_INPUT_FLAGS) & 0x60) &&
                  (pst <= 2 || pst == 6 || pst == 7 || pst == 8);
        /* a fire button only counts where Mega Man can shoot: the same buttons advance text
           and answer prompts (Circle is the special weapon by default) */
        firing = ok && can_shoot && (ACT(acts, ACT_SHOOT) || ACT(acts, ACT_SPECIAL));
        /* (any mode, also locked on: the backpedal's burst below; the buster only: the special
           weapons keep Mega Man planted, as in the original) */
        fire_btn = st_active && st_allowed && can_shoot && ACT(acts, ACT_SHOOT);
        /* the crosshair follows real shots: the game's weapon-firing flags (+0x174 & 0xC0, set
           by the shot code), not presses */
        since_fire = ok && (P8(PL_FIRE_FLAGS) & 0xC0) ? 0 : since_fire < 1e9 ? since_fire + ticks : since_fire;
        if (firing)
            aim_hold = AIM_HOLD;
        else if (!ok)
            aim_hold = 0;
        else if (aim_hold > 0)
            aim_hold -= ticks;
        cam_aim = aim_hold > 0;
        if (cam_aim && !was)
            aim_since = frame_tick;
        face_dir = lock_active ? lock_face : aim_face;   /* (toward the aim point, aim_point_update) */
    }
    /* mouse look (raw counts), wheel = next lock-on target */
    /* shoulder aim: a slower camera for fine aiming ([controls] aim_sensitivity) */
    aim_scale = sh_aim ? g_aim_sens / 100.0 : 1.0;
    /* aim assist: while the crosshair is over an enemy (aiming over the shoulder, or shooting where the
       camera looks), the camera turns slower, eased in and out */
    {
        static double assist = 1.0;
        int on = g_aim_assist && aim_on_enemy && (sh_aim || (g_aim_camera && (cam_aim || g_crosshair == 2)));
        assist += ((on ? AIM_ASSIST_SLOW : 1.0) - assist) * (1.0 - pow(1.0 - AIM_ASSIST_EASE, ticks));
        aim_scale *= assist;
    }
    if (st_active && st_allowed && (mdx || mdy || mwheel)) {
        if (lock_active) {
            if (mwheel)
                lockon_switch(mwheel < 0 ? 1 : -1);
        } else if (!aiming || lock_free || sh_aim) {
            st_yaw = fmod(st_yaw + mdx * MOUSE_YAW_PER_COUNT * g_mouse_scale * aim_scale + 65536.0, 65536.0);
        }
        if (!aiming || lock_active || lock_free || sh_aim)
            st_pitch = clampd(st_pitch - mdy * MOUSE_PITCH_PER_COUNT * g_mouse_scale * aim_scale *
                              (g_mouse_invert_y ? -1 : 1), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
        else {   /* stock aim (no target): the mouse turns and tilts Mega Man, see on_player_update */
            InterlockedExchangeAdd(&g_aim_mdx, mdx);
            InterlockedExchangeAdd(&g_aim_mdy, mdy);
        }
    }
    if (lock_active) {
        n = stick_norm(p.sThumbRY);
        if (n != 0)
            st_pitch = clampd(st_pitch + g_pitch_speed * n * ticks, PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
    } else if (st_active && aiming && !lock_free && !sh_aim) {
        st_yaw = game_yaw();
        st_pitch = clampd(game_pitch(), PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
    } else {
        n = stick_norm(p.sThumbRX);
        if (n != 0) {
            st_yaw = fmod(st_yaw + g_yaw_speed * n * ticks * aim_scale, 65536.0);
            if (st_yaw < 0)
                st_yaw += 65536.0;
        }
        n = stick_norm(p.sThumbRY);   /* stick up tilts the view up */
        if (n != 0)
            st_pitch = clampd(st_pitch + g_pitch_speed * n * ticks * aim_scale, PITCH_LIMIT_DOWN, PITCH_LIMIT_UP);
        else if (sh_aim && !g_sh_always && sh_aim_age < SH_LEVEL_TIME && !mdy)   /* shoulder aim starts level, at the horizon
                                                                                     (not where the camera is already there) */
            st_pitch += (0.0 - st_pitch) * (1.0 - pow(1.0 - 2 * SH_PITCH_EASE, ticks));
    }

    /* camera-relative movement on the left stick */
    if (st_active && st_allowed && (!aiming || lock_active || lock_free || sh_aim)) {
        double mag = sqrt((double)p.sThumbLX * p.sThumbLX + (double)p.sThumbLY * p.sThumbLY);
        if (mag > g_stick_dz) {
            double theta = atan2((double)p.sThumbLX, (double)p.sThumbLY);   /* 0 = up, + = right */
            int units = (int)(theta * 4096.0 / (2.0 * PI));
            facing = ((int)st_yaw + FACE_OFFSET + FACE_SIGN * units) & 0xffff;
            fwd = (int)(clampd((mag - g_stick_dz) / (32767.0 - g_stick_dz), 0, 1) * 32767);
            moving = 1;
        }
    }
    /* In a burst, a run goes on through a stick gap of a few frames (a left-right flick passes the
       centre): stopping and starting again played the run's stop and start-up animations, side-on
       and without the shot's twist toward the target. */
    {
        static int last_face, last_fwd, gap = COAST_FRAMES;
        if (moving) {
            last_face = facing, last_fwd = fwd, gap = 0;
        } else if (combat && face_hold && P8(PL_STATE) == 2 && gap < COAST_FRAMES && last_fwd > 0) {
            gap++;
            facing = last_face, fwd = last_fwd, moving = 1;
        } else {
            gap = COAST_FRAMES;
        }
    }
    move_face = facing;
    move_fwd = fwd;
    move_walk = moving && fwd < (int)(g_walk_frac * 32767);
    move_active = moving;
    /* Moving away from where he faces in a face hold during a burst (the fire button held or a
     * shot still out): the backward run turns his body around and never shoots, so he
     * backpedals instead (the walk back, at any tilt), which shoots on the move with
     * walk_shoot (on_pre_handler). Outside a burst nothing changes: locked on, a light tilt
     * steps back and a full one runs back; with camera aim, moving toward the camera turns him
     * to run that way. Without walk_shoot the walk back stops for every shot: camera aim then
     * turns him to run toward the camera and shoot where he runs. */
    {
        static int fled;   /* the last shots went out running toward the camera */
        static int was_back;
        int pedal = g_walk_shoot && ws_hooked;
        int burst = fire_btn || (P8(PL_FIRE_FLAGS) & 0x80);
        /* in a burst the backpedal takes over from RUN_BACK_EDGE (100 deg), otherwise the back
           sector starts at 135 deg (the backward run) */
        int edge = pedal && burst ? RUN_BACK_EDGE + (was_back ? -BACK_HYST : BACK_HYST) : 1536;
        int rel_back = moving && abs(((facing - face_dir + 2048) & 0xfff) - 2048) > edge;
        was_back = rel_back;
        int back = cam_aim && !lock_active && rel_back && !(pedal && burst);
        if (firing)
            fled = back;
        else if (fled || back) {   /* ...then they don't turn him around afterwards */
            aim_hold = 0, cam_aim = 0;
            fled = 0;
        }
        /* camera aim only on the ground (idle, walk, run, sidestep) and in jumps: talking,
           checking, kicking, ladders, knockback, Jet Skates keep their own facing (with the
           camera turning, the hold turned Mega Man in the middle of a conversation) */
        int st = P8(PL_STATE), aim_state = st <= 2 || st == 6 || st == 7 || st == 8;
        face_hold = (lock_active && st != 3) || (cam_aim && !back && aim_state) || (sh_aim && aim_state);
        backpedal = pedal && burst && face_hold && rel_back && st != 7 && st != 8;
        combat = pedal && burst && face_hold;
    }
    /* Jet Skates button: held, or a press toggles them ([controls] skates_toggle=1) */
    {
        static int prev_sk, sk_on;
        int sk = ACT(actions_of(&p) | (unsigned)g_kbm_acts, ACT_SKATES) != 0, live = st_active && st_allowed;
        if (sk && !prev_sk)
            sk_on = !sk_on;
        if (!live)
            sk_on = 0;
        prev_sk = sk;
        skate_want = live && (g_skate_toggle ? sk_on : sk);
    }
    cam_yaw = (int)st_yaw & 0xffff;
    cam_active = st_active && st_allowed;
    cam_suppress = cam_active && aiming && !lock_active && !lock_free && !sh_aim;
    /* the cursor stays put and hidden while the orbit drives gameplay */
    kbm_capture(g_kbm && g_modern && (cam_active || rail_live()));
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

/* cam_apply_angles places the camera at target - d * dir(pitch), so to keep the camera where an
 * orbit of `orbit` puts it while the view looks along the steeper `pitch`, hand it a virtual
 * target d along that view from there (pitches positive up, 4096 = 360 degrees) */
static void view_past_orbit(regs_t *r, const int32_t *t, int d, int yaw, double orbit, int pitch)
{
    double a = (yaw & 0xfff) * (PI / 2048.0);
    double bo = -orbit * (PI / 2048.0), bv = -pitch * (PI / 2048.0);
    double cx = t[0] - d * sin(a) * cos(bo), cy = t[1] - d * sin(bo), cz = t[2] - d * cos(a) * cos(bo);
    if (!g_view_target)
        g_view_target = (int32_t *)GUEST(guest_alloc(16));
    g_view_target[0] = iround(cx + d * sin(a) * cos(bv));
    g_view_target[1] = iround(cy + d * sin(bv));
    g_view_target[2] = iround(cz + d * cos(a) * cos(bv));
    g_view_target[3] = t[3];
    r->eax = GADDR(g_view_target);
}

/* Camera collision and floor-safe look-up for the call in progress: r holds
 * cam_apply_angles' target (EAX), distance (EDX), yaw (EBX) and -pitch (ECX).
 * The camera orbits the target only up to LOOK_ORBIT_MAX; looking further up
 * keeps it there and tilts the view through a virtual target. `mode` keeps the
 * eased distance per camera (0 orbit, 1 stock aim). */
static void place_camera(regs_t *r, int yaw, int pitch, int mode)
{
    static int last_mode = -1;
    const int32_t *t = (const int32_t *)GUEST(r->eax);
    int orbit = pitch > LOOK_ORBIT_MAX ? LOOK_ORBIT_MAX : pitch;
    int want, d;
    if (mode != last_mode)
        cam_dist_cur = -1;
    last_mode = mode;
    /* pull in at once, ease back out */
    want = cam_collide(t, (int)r->edx, yaw, -orbit);
    if (cam_dist_cur < 0 || want < cam_dist_cur)
        cam_dist_cur = want;
    else
        cam_dist_cur += (want - cam_dist_cur) * CAM_EASE_OUT;
    d = iround(cam_dist_cur);
    r->edx = (uint32_t)d;
    r->ecx = (uint32_t)(-pitch & 0xffff);   /* cam_apply_angles negates ECX */
    if (pitch > orbit)
        view_past_orbit(r, t, d, yaw, orbit, pitch);
}

/* The boat rides: the game's camera swings down behind Mega Man as he aims up, and at the top
 * of the aim (+0x108 = -0x200, the stock clamp too) it ends up under the water, which then
 * covers the sky (vanilla does the same). The camera stops RAIL_CAM_LOW below the deck (the
 * water is ~625 below it: a camera at deck + 623 still sees the sky, at + 639 the water) and
 * the view keeps tilting from there, so the whole aim range stays. ECX = -pitch. */
#define RAIL_CAM_LOW  470
static void rail_camera(regs_t *r)
{
    const int32_t *t = (const int32_t *)GUEST(r->eax);
    int d = (int)r->edx, yaw = (int)r->ebx & 0xfff, pitch = -signed12((int)r->ecx & 0xfff);
    double low = (double)S16(0x16) + RAIL_CAM_LOW, s;
    if (d <= 0 || t[1] + d * sin(pitch * (PI / 2048.0)) <= low)
        return;
    s = (low - t[1]) / d;   /* the steepest orbit that keeps the camera at or above `low` */
    if (s < 0)
        s = 0;
    view_past_orbit(r, t, d, yaw, asin(s > 1 ? 1 : s) * (2048.0 / PI), pitch);
}

/* Over-the-shoulder lock-on camera: while locked on, cam_apply_angles gets a view point slid
 * beside Mega Man (and up a little) and a shorter distance, so the camera looks past his shoulder
 * along the line to the enemy (the yaw and pitch toward the enemy from that point are set with
 * the lock, input_update). Eased in and out, and across on a swap. The slide stops in front of a
 * wall beside him; place_camera then collides the camera line from the view point as usual. */
static void shoulder_apply(regs_t *r)
{
    static LARGE_INTEGER last;
    LARGE_INTEGER now;
    int aiming_frame = (g_sh_mode && lock_active) || ((g_sh_mode || g_aim_free || g_sh_always) && sh_aim);
    double ticks = 0, a, lat, rx, rz, want = aiming_frame || g_sh_always ? 1.0 : 0.0, dist, zw;
    const int32_t *t;
    int s, step = 24;
    QueryPerformanceCounter(&now);
    if (last.QuadPart)
        ticks = clampd((double)(now.QuadPart - last.QuadPart) / qpc_freq.QuadPart, 0, 0.1) * 60.0;
    last = now;
    sh_blend += (want - sh_blend) * (1.0 - pow(1.0 - SH_EASE, ticks));
    sh_close += ((aiming_frame ? 1.0 : 0.0) - sh_close) * (1.0 - pow(1.0 - SH_EASE, ticks));
    sh_side_cur += (g_sh_side - sh_side_cur) * (1.0 - pow(1.0 - SH_SWAP_EASE, ticks));
    /* camera_shoulder: aiming zooms the view in */
    zw = g_sh_always && sh_aim ? ADS_ZOOM : 1.0;
    if (ads_zoom != zw) {
        ads_zoom += (zw - ads_zoom) * (1.0 - pow(1.0 - ADS_EASE, ticks));
        if (fabs(ads_zoom - zw) < 0.002)
            ads_zoom = zw;
        mml_zoom((float)ads_zoom);
    }
    if (sh_blend < 0.002) {
        sh_blend = want > 0 ? sh_blend : 0;
        if (!want)
            return;
    }
    t = (const int32_t *)GUEST(r->eax);
    a = (cam_yaw & 0xfff) * (PI / 2048.0);
    rx = cos(a), rz = -sin(a);   /* the view's right, horizontal */
    lat = sh_side_cur * SH_SIDE * sh_blend;
    for (s = step; s <= (int)fabs(lat) + CAM_MARGIN; s += step) {   /* a wall beside him: stop short */
        double k = lat < 0 ? -s : s;
        if (cam_probe(t[0] + rx * k, t[1] - SH_LIFT * sh_blend, t[2] + rz * k) != 0) {
            double room = s - CAM_MARGIN > 0 ? s - CAM_MARGIN : 0;
            if (room < fabs(lat))
                lat = lat < 0 ? -room : room;
            break;
        }
    }
    /* closer: always aiming's distance, or with camera_shoulder from the hip distance to it as he aims */
    dist = g_sh_always ? SH_DIST_HIP + (SH_DIST - SH_DIST_HIP) * sh_close : SH_DIST;
    dist = (int)r->edx * (1.0 - sh_blend * (1.0 - dist));
    /* Tight spaces (his back to a wall, a hallway): the camera line from the shoulder view runs into
       the wall at once, and the camera ended up in his shoulder. With little room behind the view, it
       eases back toward the middle and up over his head, so the camera looks past him. */
    {
        int32_t probe[4];
        int pitch = iround(st_pitch) > LOOK_ORBIT_MAX ? LOOK_ORBIT_MAX : iround(st_pitch), room;
        double tw;
        probe[0] = iround(t[0] + rx * lat);
        probe[1] = iround(t[1] - SH_LIFT * sh_blend);
        probe[2] = iround(t[2] + rz * lat);
        probe[3] = t[3];
        room = cam_collide(probe, iround(dist), cam_yaw, -pitch);
        tw = dist > 0 ? clampd(1.0 - room / (dist * TIGHT_ROOM), 0, 1) : 0;
        sh_tight += (tw - sh_tight) * (1.0 - pow(1.0 - SH_EASE, ticks));
        lat *= 1.0 - TIGHT_CENTER * sh_tight * sh_blend;
    }
    if (!g_sh_target)
        g_sh_target = (int32_t *)GUEST(guest_alloc(16));
    g_sh_target[0] = iround(t[0] + rx * lat);
    g_sh_target[1] = iround(t[1] - (SH_LIFT + TIGHT_LIFT * sh_tight) * sh_blend);
    g_sh_target[2] = iround(t[2] + rz * lat);
    g_sh_target[3] = t[3];
    r->eax = GADDR(g_sh_target);
    r->edx = (uint32_t)iround(dist);
}

/* the aiming zoom off (the orbit camera isn't placing the view: talks, cutscenes, menus) */
static void ads_zoom_reset(void)
{
    if (ads_zoom != 1.0) {
        ads_zoom = 1.0;
        mml_zoom(1.0f);
    }
}

/* The point the camera looks at (the camera as the game last placed it: the same framing the
 * reticle projects through): the first thing the view ray meets past Mega Man, an enemy (its
 * hit volume, AIM_ENEMY_R around the centre) or the level (the collision test the camera uses:
 * walls, and the floor from below), else AIM_DIST out. Shots and the facing aim at it, so they
 * converge on whatever is under the crosshair at any range, also with the camera beside him
 * (over the shoulder). Cast only while some aiming is on. */
/* Mega Man fades out as the camera comes close to him (a wall at his back, a narrow hall), so he
 * doesn't fill the view; drawn see-through by ddshim.c (packets_drawing_player). Only while the
 * orbit places the camera. */
static void player_fade_update(void)
{
    static double cur = 1.0;
    static int n;
    double cx = RD32(G_CAM_PLACED), cy = RD32(G_CAM_PLACED + 4), cz = RD32(G_CAM_PLACED + 8);
    double dx = cx - S16(0x14), dy = cy - (S16(0x16) - AIM_CHEST), dz = cz - S16(0x18);
    double d = sqrt(dx * dx + dy * dy + dz * dz), want = 1.0;
    if (g_fade_on && cam_active && !rail_live())   /* (whoever placed the camera: also while it settles) */
        want = FADE_MIN + (1.0 - FADE_MIN) * clampd((d - FADE_NEAR) / (FADE_FAR - FADE_NEAR), 0, 1);
    cur += (want - cur) * FADE_EASE;
    if (fabs(cur - want) < 0.004)
        cur = want;
    InterlockedExchange(&g_player_alpha, (LONG)iround(cur * 255));
    if (g_fade_log && ++n % 10 == 0)
        log_printf("fade: camera %.0f from his chest, alpha %.2f", d, cur);
}

int controls_player_alpha(void)
{
    return (int)g_player_alpha;
}

void controls_camera_fade(int on)
{
    char v[8] = "";
    g_fade_on = on != 0;
    GetEnvironmentVariableA("MML_FADE_LOG", v, sizeof v);
    g_fade_log = v[0] == '1';
    log_printf("controls: Mega Man fades when the camera is close: %s", g_fade_on ? "on" : "off");
}

static void aim_point_update(void)
{
    double yaw = (RD16(G_CAM_YAW_NOW) & 0xfff) * (PI / 2048.0);
    int pr = RD16(G_CAM_PITCH_NOW) & 0xfff;
    double p = -(pr > 2048 ? pr - 4096 : pr) * (PI / 2048.0);
    double fx = sin(yaw) * cos(p), fy = sin(p), fz = cos(yaw) * cos(p);
    double cx = RD32(G_CAM_PLACED), cy = RD32(G_CAM_PLACED + 4), cz = RD32(G_CAM_PLACED + 8);
    double px = S16(0x14), py = S16(0x16) - AIM_CHEST, pz = S16(0x18), dist = AIM_DIST, dx, dz, h;
    int on_enemy = 0;
    if (cam_active && (cam_aim || sh_aim || since_fire < 600)) {
        int vol[64][3], n = lockon_volumes(vol, 64), i;
        double s0 = (px - cx) * fx + (py - cy) * fy + (pz - cz) * fz + AIM_RAY_SKIP, s;
        if (s0 < AIM_RAY_SKIP)
            s0 = AIM_RAY_SKIP;
        dist = AIM_RAY_MAX;
        for (i = 0; i < n; i++) {   /* enemies: the nearest whose volume the ray passes through */
            double rx = vol[i][0] - cx, ry = vol[i][1] - cy, rz = vol[i][2] - cz;
            double t = rx * fx + ry * fy + rz * fz, d2 = rx * rx + ry * ry + rz * rz - t * t;
            if (t > s0 && d2 < AIM_ENEMY_R * AIM_ENEMY_R) {
                t -= sqrt(AIM_ENEMY_R * AIM_ENEMY_R - d2);
                if (t < dist) {
                    dist = t < s0 ? s0 : t;
                    on_enemy = 1;
                }
            }
        }
        for (s = s0; s < dist; s += AIM_RAY_STEP) {   /* the level: march, then narrow it down */
            if (cam_probe(cx + fx * s, cy + fy * s, cz + fz * s) != 0) {
                double lo = s - AIM_RAY_STEP < s0 ? s0 : s - AIM_RAY_STEP, hi = s;
                for (i = 0; i < 4; i++) {
                    double mid = (lo + hi) / 2;
                    if (cam_probe(cx + fx * mid, cy + fy * mid, cz + fz * mid) != 0)
                        hi = mid;
                    else
                        lo = mid;
                }
                dist = lo;
                on_enemy = 0;   /* a wall in front of the enemy */
                break;
            }
        }
    }
    aim_px = cx + fx * dist;
    aim_py = cy + fy * dist;
    aim_pz = cz + fz * dist;
    aim_on_enemy = on_enemy;
    /* the facing that sends shots there: toward the point, unless it's so close (or beside him)
       that the direction would swing around: then the camera's own heading */
    dx = aim_px - px, dz = aim_pz - pz, h = sqrt(dx * dx + dz * dz);
    if (h > AIM_FACE_MIN && dx * fx + dz * fz > 0)
        aim_face = (iround(atan2(dx, dz) * 2048.0 / PI) + 0x800) & 0xfff;
    else
        aim_face = (cam_yaw + FACE_OFFSET) & 0xfff;
}

/* the shots' pitch from Mega Man's chest to a world point */
static int pitch_to(double x, double y, double z)
{
    double dx = x - S16(0x14), dz = z - S16(0x18), dy = y - (S16(0x16) - AIM_CHEST);
    int pitch = iround(SHOT_PITCH_SIGN * atan2(dy, sqrt(dx * dx + dz * dz)) * (2048.0 / PI));
    if (pitch > AIM_PITCH_MAX) pitch = AIM_PITCH_MAX;
    if (pitch < -AIM_PITCH_MAX) pitch = -AIM_PITCH_MAX;
    return pitch;
}

/* what FUN_0042A85C returns now: the game's own value, the camera-aimed pitch, or (locked on) the
   pitch to the target. The buster finds a locked enemy by itself (the game's own targeting, which
   lock-on feeds), but the special weapons only take this pitch: level, without it (issue #13). */
static void shot_pitch_sync(void)
{
    if (!g_shot_flag)
        return;
    if (lock_active) {
        *g_shot_pitch = pitch_to(lock_tx, lock_ty, lock_tz);
        *g_shot_flag = 1;
    } else if ((cam_aim || sh_aim) && face_hold) {
        *g_shot_pitch = pitch_to(aim_px, aim_py, aim_pz);
        *g_shot_flag = 1;
    } else {
        *g_shot_pitch = GVAR(int32_t, G_AIMCAM_PITCH);
        *g_shot_flag = GVAR(uint8_t, G_AIMCAM_ON);
    }
}

int controls_aim_point(double *x, double *y, double *z)
{
    if (rail_live()) {
        *x = rail_px;
        *y = rail_py;
        *z = rail_pz;
        return 1;
    }
    *x = aim_px;
    *y = aim_py;
    *z = aim_pz;
    return g_shot_flag != NULL;
}

static void on_cam_apply(regs_t *r)
{
    int32_t px, pz;
    int gate, ma, is_gameplay_cam, is_rail_cam, lock_stand, ready;

    frame_tick = (frame_tick + 1) & 0x3fffffff;
    {
        /* the first camera update after a pause in them (a room load, a menu): the aim point
           (aim_point_update, at the end of each update) is from before, in another room or
           view; until this update refreshes it, aim the camera's way (Mega Man snapped to a
           stray facing when firing at once after a load) */
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (qpc_freq.QuadPart && (now.QuadPart - g_last_cam_qpc.QuadPart) * 10 > qpc_freq.QuadPart) {
            aim_face = (((int)r->ebx & 0xfff) + FACE_OFFSET) & 0xfff;
            cam_fresh = 1;
        }
        g_last_cam_qpc = now;
    }
    {
        /* only the gameplay camera (camera_update's common tail) is "the game's
         * camera"; cam_follow_lookat & co. also come through here every frame
         * with angles of their own */
        uint32_t ret = GVAR(uint32_t, r->esp + 4);
        is_gameplay_cam = ret == G_RET_CAM_COMMON_BLEND || ret == G_RET_CAM_COMMON;
        is_rail_cam = ret == G_RET_CAM_SCRIPTED && rail_mode();
        if (is_gameplay_cam) {
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
    ready = 1;
    skate_turn = 0;
    if (face_hold && !roll_latch && !(P8(PL_INPUT_FLAGS) & 0x60)) {
        int cur = (RD32(G_PLAYER_FACING) >> 16) & 0xfff, step = lock_active ? LOCK_TURN_STEP : AIM_TURN_STEP;
        int d = ((face_dir - cur + 2048) & 0xfff) - 2048;
        if (d > step) d = step;
        if (d < -step) d = -step;
        if (d)
            write_facing(cur + d);
        /* camera aim: the shot goes out once he faces the camera's way (or after a moment) */
        d = ((face_dir - cur - d + 2048) & 0xfff) - 2048;
        ready = !(cam_aim || sh_aim) || lock_active || (d <= AIM_FIRE_TOL && d >= -AIM_FIRE_TOL) ||
                (LONG)(frame_tick - aim_since) >= AIM_FIRE_WAIT;
    } else if (move_active && !roll_latch && (gate > 0 || pos_move_credit > 0)) {
        if (P8(PL_STATE) == 3 && !(P8(PL_INPUT_FLAGS) & 0x60)) {
            /* on skates he carves: the facing turns toward the stick at a limited rate */
            int cur = (RD32(G_PLAYER_FACING) >> 16) & 0xfff, step = SKATE_TURN_STEP * frame_step() / 2;
            int d = ((move_face - cur + 2048) & 0xfff) - 2048;
            if (d > step) d = step;
            if (d < -step) d = -step;
            if (d)
                write_facing(cur + d);
            skate_turn = d;
        } else {
            write_facing(move_face);
        }
    }
    aim_ready = ready;
    if (gate > 0)
        move_gate = gate - 1;

    /* forward injection only in gameplay (menus never move the player) */
    ma = move_active;
    if (ma && !move_prev)
        move_start_grace = MOVE_START_GRACE;
    move_prev = ma;
    /* in a face hold (lock-on, camera aim) the raw stick never reaches the game (it would turn
     * him), so a closed gate means no input at all: a shot that roots Mega Man (special weapons)
     * lets the gate run out while the stick is held, and with no direction he never starts
     * moving again to reopen it; a run's first frames don't move him either, and losing the
     * direction there ends the run before it starts. Standing, walking or running in control
     * in a face hold is gameplay: keep the direction coming. */
    lock_stand = face_hold && !(P8(PL_INPUT_FLAGS) & 0x60) &&
                 (P8(PL_STATE) == 0 || P8(PL_STATE) == 1 || P8(PL_STATE) == 2 || P8(PL_STATE) == 6);
    inject_fwd = ma && (move_gate > 0 || pos_move_credit > 0 || move_start_grace > 0 || lock_stand);
    if (move_start_grace > 0)
        move_start_grace--;


    if (is_rail_cam) {
        /* the boat rides (scripted rig, any controls): the camera stays above the water */
        rail_camera(r);
        cam_dist_cur = -1;
    } else if (cam_active && !cam_suppress && !cam_follow) {
        r->ebx = (uint32_t)cam_yaw;
        true_orbit();
        shoulder_apply(r);
        place_camera(r, cam_yaw, iround(st_pitch), 0);
    } else if (cam_active && cam_suppress && is_gameplay_cam) {
        /* stock aim (no target): the game's own, closer aim camera; same wall
         * stop and floor-safe look-up as the orbit (classic camera stays vanilla) */
        ads_zoom_reset();
        place_camera(r, (int)r->ebx & 0xfff, -signed12((int)r->ecx & 0xfff), 1);
    } else {
        ads_zoom_reset();
        cam_dist_cur = -1;
    }
    aim_point_update();
    shot_pitch_sync();
    player_fade_update();
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
/* While shooting in a face hold: the speed by direction, % of the full speed, from the angle off the
 * facing: full up to RUN_BACK_EASE, a smoothstep down to the backpedal's speed at RUN_BACK_EDGE,
 * that speed beyond (the backpedal, and the drift of jumps that way). */
static double back_curve(int rel)
{
    double bp = g_backjog ? BACKJOG_PCT : 0x50 * BACKPEDAL_PCT / 100.0 / 0x200 * 100, e;
    rel = abs(rel);
    if (rel <= RUN_BACK_EASE)
        return 100;
    e = clampd((double)(rel - RUN_BACK_EASE) / (RUN_BACK_EDGE - RUN_BACK_EASE), 0, 1);
    return 100 + (bp - 100) * e * e * (3 - 2 * e);
}

/* One pose of the body animation forward, as anim_tick does when a hold runs out (+0x98 the entry,
 * +0x94 the script; an entry's flags byte & 0x80 = jump to entry & 0x7F, 0xFF = the end, which
 * holds). A footstep (event & 0x80) on the skipped pose is kept on the new one. 0 at the end. */
static int anim_skip_pose(void)
{
    uint32_t cur = GVAR(uint32_t, PL(0x98)), next;
    uint8_t flags, ev;
    if (!cur)
        return 0;
    flags = GVAR(uint8_t, cur + 3);
    if (flags == 0xFF)
        return 0;
    next = (flags & 0x80) ? GVAR(uint32_t, PL(0x94)) + (flags & 0x7Fu) * 8 : cur + 8;
    ev = P8(0xA6) & 0x80;
    GVAR(uint32_t, PL(0x98)) = next;
    GVAR(uint32_t, PL(0xA4)) = GVAR(uint32_t, next);
    P8(0xA6) |= ev;
    return 1;
}

static void locomotion_tune(void)
{
    int st = P8(PL_STATE), id = P8(PL_ANIM_ID), pct, rate = -1;   /* rate: % step rate (default = speed) */
    double n = move_fwd / 32767.0, w = g_walk_frac, t;
    loco_pct = 100;
    if ((st == 1 && P8(PL_SUBSTATE) >= 1 && (P8(PL_VARIANT) == 0 || face_hold)) ||
        (st == 6 && face_hold)) {
        t = w > 0 ? n / w : 1;
        if (t > 1) t = 1;
        pct = g_walk_min + (int)((100 - g_walk_min) * t);
        /* in a face hold, the walk-mode steps are how Mega Man moves while facing
         * the target / the camera's way; stock they crawl (the sidestep at 1/16 of a run) */
        if (face_hold)
            pct = pct * (st == 6 ? LOCK_SIDESTEP_PCT : LOCK_STEP_PCT) / 100;
        if (backpedal && st == 1 && n > w && w < 1)   /* past the walk range: up to the backpedal's top speed */
            pct += (int)(((g_backjog ? BACKJOG_PCT * 0x200 / 0x50 : BACKPEDAL_PCT) - LOCK_STEP_PCT) *
                         clampd((n - w) / (1 - w), 0, 1));
        loco_pct = pct;
        if (id < 0x0C || id > 0x0F)
            return;
    } else if (st == 2 && P8(PL_SUBSTATE) >= 2 && (P8(PL_RUN_DIR) == 0 || face_hold)) {
        t = w < 1 ? (n - w) / (1 - w) : 1;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        pct = g_run_min + (int)((100 - g_run_min) * t);
        if (combat && face_hold)   /* moving backward while shooting: slower toward the back */
            pct = (int)(pct * back_curve(signed12((move_face - face_dir) & 0xfff)) / 100);
        loco_pct = pct;
        /* slowed while shooting, the run's long strides at a slow step rate looked like low gravity:
           its steps slow only by the square root of its speed (feet slide a little instead) */
        if (combat && face_hold && pct < 100)
            rate = (int)(100 * sqrt(pct / 100.0));
        if (id != 0x01 && id != 0x04)
            return;
    } else {
        loco_anim_acc = 0;
        return;
    }
    /* animation rate follows the speed: slower = extra hold ticks, faster = shaved ones, and
     * where a pose already lasts a single update (all of the walk, step and run poses) whole
     * poses skipped, so the legs keep up with the ground (anim_tick steps once more on its own); for
     * now only the backpedal skips (the lock-on steps would shuffle at up to 5x) */
    loco_anim_acc += 100 - (rate >= 0 ? rate : pct);
    while (loco_anim_acc >= 100) {
        loco_anim_acc -= 100;
        if (P8(PL_ANIM_HOLD) < 0x7F)
            P8(PL_ANIM_HOLD)++;
    }
    {
        int skips = 0;
        while (loco_anim_acc <= -100) {
            loco_anim_acc += 100;
            if (P8(PL_ANIM_HOLD) > 1)
                P8(PL_ANIM_HOLD)--;
            else if (backpedal && st == 1 && skips < ANIM_RATE_MAX - 1 && anim_skip_pose())   /* (the backpedal only) */
                skips++;
        }
    }
}

/* ---- Jet Skates ----
 *
 * Stock, the skates start from standing only: the walk button (Y here, shared with talk) held for 8
 * updates (player_try_jetskates, from pre_00), then a start-up animation (state 3 subs 0-2) before
 * he moves; state 3 never checks jump; it skates while the walk button stays held (pre_03 / sub 4),
 * and letting go brakes to a stop (sub 5). Sub 3 waits for the current animation to end, plays the
 * jet sound and falls into sub 4 in the same update, so entering sub 3 with the animation marked
 * ended skates at once from any grounded state. A jump is the game's running jump (state 8); its
 * landing (sub 6) goes back into sub 3 the same way. The skates button stands in for the walk
 * button: injected while wanted, kept from the game otherwise. */

static void inject_walk(void)
{
    uint16_t walk = P16(PL_MASK_WALK);
    walk_injected = 1;
    P16(PL_HELD) |= walk;
    /* walk and talk share a button: an injected walk must not open a conversation with whoever
       stands in front (the real button still does) */
    if (!talk_hidden && !(GVAR(uint16_t, G_PAD_HELD) & walk) && (P16(PL_MASK_TALK) & walk)) {
        talk_saved = P16(PL_MASK_TALK);
        P16(PL_MASK_TALK) = (uint16_t)(talk_saved & ~walk);
        talk_hidden = 1;
    }
}

static int skate_try_start(void)
{
    int st = P8(PL_STATE), speed = 0, cur;
    if (st != 0 && st != 1 && st != 2 && st != 4 && st != 5 && st != 6 && !(st == 8 && P8(PL_SUBSTATE) >= 6))
        return 0;
    if (P8(PL_AIRBORNE) || (P8(PL_INPUT_FLAGS) & 0x60) || !(P8(PL_ITEMS) & 2) || P8(PL_SKATE_BLOCK) ||
        (P8(PL_FIRE_FLAGS) & 0xC0) || lock_active || roll_ours)
        return 0;
    if (st == 2)
        speed = 0x200 * loco_pct / 100;          /* from a run: keep its speed */
    else if (st == 8)
        speed = skate_air ? skate_speed : S16(PL_VEL_FWD);   /* landing: keep the jump's */
    if (speed < 0) speed = 0;
    /* travel along where he was going (a turned run has its body off the facing) */
    cur = (RD32(G_PLAYER_FACING) >> 16) & 0xfff;
    write_facing(cur - S16(PL_MOVE_ANGLE));
    P8(PL_FIRE_FLAGS) |= 4;                       /* (set by the stock start, sub 0) */
    P8(PL_STATE) = 3;
    P8(PL_SUBSTATE) = 3;
    P8(PL_VARIANT) = 0;
    P8(PL_RUN_DIR) = 0;
    S16(PL_MOVE_ANGLE) = 0;
    P16(0xB4) = 0;
    S16(PL_VEL_SIDE) = 0;
    S16(PL_VEL_FWD) = (int16_t)speed;
    P8(PL_ANIM_FLAGS) = 0xFF;                     /* "current animation finished": sub 3 goes on at once */
    P8(PL_SKATE_COUNT) = 0x20;
    skate_speed = speed;
    skate_air = 0;
    skate_lean = skate_lean_hold = skate_brake = 0;
    return 1;
}

/* after the walk-injection bookkeeping of the update; returns 1 while skates own the walk button */
static int skate_update(void)
{
    int st = P8(PL_STATE), sub = P8(PL_SUBSTATE);
    uint16_t walk = P16(PL_MASK_WALK);
    static int prev_st;
    int new_lean = 0;
    /* take back the stock buttons handed over last update (the pad copy may not have run since) */
    P16(PL_HELD) &= (uint16_t)~(skate_inj_held & ~GVAR(uint16_t, G_PAD_HELD));
    P16(PL_PRESSED) &= (uint16_t)~skate_inj_pressed;
    skate_inj_held = skate_inj_pressed = 0;
    if (st == 3) {
        /* carving: the facing turned this much on the last camera update */
        int lean = skate_turn <= -SKATE_LEAN_MIN ? -1 : skate_turn >= SKATE_LEAN_MIN ? 1 : 0;
        if (lean) {
            new_lean = lean != skate_lean;
            skate_lean = lean;
            skate_lean_hold = SKATE_LEAN_HOLD;
        } else if (skate_lean_hold > 0 && --skate_lean_hold == 0) {
            skate_lean = 0;
        }
    } else {
        skate_lean = skate_lean_hold = skate_brake = 0;
    }
    if (prev_st == 3 && (st == 7 || st == 8))
        skate_air = 1;                 /* skated off a ledge: the fall keeps the speed too */
    prev_st = st;
    if (skate_air && st != 7 && st != 8)
        skate_air = 0;
    if (st == 0)
        P8(PL_SKATE_COUNT) = 0;        /* no stock start from holding Y: the skates button does it */
    if (!skate_want || (P8(PL_INPUT_FLAGS) & 0x60)) {
        if ((st == 3 || skate_air) && !(GVAR(uint16_t, G_PAD_HELD) & walk))
            P16(PL_HELD) &= (uint16_t)~walk;   /* (a light tilt's walk must not keep them going) */
        return st == 3 || skate_air;
    }
    if (st == 3 && (sub == 3 || sub == 4) && !P8(PL_AIRBORNE) && (P16(PL_PRESSED) & P16(PL_MASK_JUMP))) {
        /* jump: the running jump (state 8 sub 0 takes off: anim, height, sound), at skating speed */
        P8(PL_STATE) = 8;
        P8(PL_SUBSTATE) = 0;
        P8(PL_VARIANT) = 0;
        P8(PL_RUN_DIR) = 0;
        S16(PL_MOVE_ANGLE) = 0;
        P16(0xB4) = 0;
        S16(PL_VEL_SIDE) = 0;
        S16(PL_VEL_FWD) = (int16_t)skate_speed;
        skate_air = 1;
    } else if (st != 3) {
        skate_try_start();
    }
    if (P8(PL_STATE) == 3 && (P8(PL_SUBSTATE) == 3 || P8(PL_SUBSTATE) == 4)) {
        /* Our turns write the facing, so the stock turn and brake buttons never reach the skates
           and neither do their effects: the turn button held picks the lean animation (0x3C/0x3D)
           and keeps the turn's skid sound going (going straight plays 0x8F over it), its press
           puffs smoke (effect 0x89), and back held brakes with smoke at both feet (effect 0x83).
           Hand them over: the stock turn adds a little (+-0x10 per update) to ours. */
        uint16_t m = 0;
        if (skate_lean) {
            m = P16(skate_lean > 0 ? PL_MASK_TURN_R : PL_MASK_TURN_L);
            if (new_lean)
                skate_inj_pressed = m;
        }
        if (skate_brake) {
            /* (sub 4 checks forward before back: a held forward would skip the brake) */
            m |= P16(PL_MASK_BACK);
            P16(PL_HELD) &= (uint16_t)~P16(PL_MASK_FWD);
        }
        skate_inj_held = (uint16_t)(m & ~GVAR(uint16_t, G_PAD_HELD));
        P16(PL_HELD) |= m;
        P16(PL_PRESSED) |= skate_inj_pressed;
    }
    if (P8(PL_STATE) == 3 || skate_air) {
        inject_walk();
        return 1;
    }
    return 0;
}

/* player_apply_velocity on skates: analog speed, carving; a skate jump keeps its speed */
static void skate_velocity(regs_t *r, int st)
{
    int sub = P8(PL_SUBSTATE);
    if (st == 3 && (sub == 3 || sub == 4)) {
        int top = SKATE_STOCK * g_skate_max / 100, low = SKATE_STOCK * g_skate_min / 100, want = 0;
        int fs = frame_step(), sharp = 0;
        if (move_active) {
            want = low + (int)((top - low) * (move_fwd / 32767.0));
            sharp = ang_dist(move_face, (RD32(G_PLAYER_FACING) >> 16) & 0xfff) > SKATE_SHARP;
            if (sharp && want > low)
                want = low;   /* turning around: carve at the low speed */
        }
        /* turning around at speed skids for a moment: slowing down, with the stock brake handed over
           from the next update */
        if (sharp && want < skate_speed - SKATE_DECEL && !skate_brake)
            skate_brake = SKATE_BRAKE_HOLD;
        else if (skate_brake > 0)
            skate_brake--;
        if (skate_brake && want > low)
            want = low;
        if (skate_speed < want) {
            skate_speed += SKATE_ACCEL * fs / 2;
            if (skate_speed > want) skate_speed = want;
        } else if (skate_speed > want) {
            skate_speed -= SKATE_DECEL * fs / 2;
            if (skate_speed < want) skate_speed = want;
        }
        r->edx = (uint32_t)skate_speed;
        S16(PL_VEL_FWD) = (int16_t)skate_speed;
        /* the skating loops (0x39 straight, 0x3C/0x3D leaning) step a pose every update, a small
           jet shake: it follows the speed, from still (no shake) to the stock rate at the stock top
           speed and above, by holding poses longer (anim_tick counts +0xA5 down after this) */
        {
            int id = P8(PL_ANIM_ID), pct = skate_speed * 100 / SKATE_STOCK;
            if (pct > 100) pct = 100;
            if (id == 0x39 || id == 0x3C || id == 0x3D) {
                skate_anim_acc += 100 - pct;
                while (skate_anim_acc >= 100) {
                    skate_anim_acc -= 100;
                    if (P8(PL_ANIM_HOLD) < 0x7F)
                        P8(PL_ANIM_HOLD)++;
                }
            } else {
                skate_anim_acc = 0;
            }
        }
    } else if (st == 3 && sub == 5) {
        /* braking to a stop (-0x28 per update until its animation ends): never into reverse */
        if ((int16_t)r->edx < 0) {
            r->edx = 0;
            S16(PL_VEL_FWD) = 0;
        }
    } else if (skate_air && (st == 7 || st == 8) && P8(PL_SUBSTATE) < 6) {
        /* the air control caps a held forward at 0x280 */
        r->edx = (uint32_t)skate_speed;
        S16(PL_VEL_FWD) = (int16_t)skate_speed;
    }
}

/* ---- walking while shooting ----
 * The game shoots on the move only while running. The run's shot (player_try_shoot mode 1) sets
 * +0x174 & 4: the shot animation plays on the second animation channel (+0x104: arms and torso)
 * while the body channel keeps the run. The walk (state 1, forward and back) and the sidestep
 * (6) shoot standing instead (mode 0: a whole-body shot animation, then idle), and while a shot
 * is out the walk stands still (state_01) and drops to idle (pre_01). So in those states the
 * shot is started here the run's way, right before the state's input handler, and the walk's
 * two handlers don't see it (+0x174's firing bits, the pending and buffered shots, the fire
 * buttons): he keeps walking and the shot plays on his arms. A weapon that can't fire on the
 * move stops him (state 0) as when running, and the idle state fires it the stock way. */
#define G_FN_TRY_SHOOT      GA(0x4ACF9Du)   /* player_try_shoot(EAX p, EDX mode): 0 standing, 1 on the move */
#define G_SITE_PRE_CALL     GA(0x4A9B3Bu)   /* player_update: xor edx,edx; mov dl,[ebp+9]; call pre[dl] */
#define G_SITE_STATE_CALL   GA(0x4A9B49u)   /* the same before call state[dl] */
#define G_SITE_AFTER_STATE  GA(0x4A9B59u)   /* call player_apply_local_velocity */
static int ws_hidden;
static uint8_t ws_fire, ws_pending, ws_buffer;
static uint16_t ws_held, ws_pressed;

#ifndef MML_RUNNER
static uint32_t g_fn_try_shoot;   /* G_FN_TRY_SHOOT in the running program (controls_install) */
typedef uint32_t(__cdecl *try_shoot_fn)(uint32_t p, uint32_t mode);
static __declspec(naked) void try_shoot_thunk(void)   /* called as try_shoot_fn */
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax, [esp + 20]
        mov edx, [esp + 24]
        mov esi, g_fn_try_shoot
        call esi
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}
#endif

static void try_shoot(int mode)
{
    if (recomp_in_shim()) {
        uint32_t regs[4];
        regs[0] = G_PLAYER, regs[1] = (uint32_t)mode, regs[2] = regs[3] = 0;
        recomp_call_watcall(G_FN_TRY_SHOOT, regs, 0, NULL);
        return;
    }
#ifndef MML_RUNNER
    ((try_shoot_fn)(void *)try_shoot_thunk)(G_PLAYER, (uint32_t)mode);
#endif
}

static int ws_firing(void)
{
    return (P8(PL_FIRE_FLAGS) & 0xC0) || P8(PL_FIRE_PENDING);
}

/* The special weapons stay out of the walk-shoot: they plant Mega Man (balance, and their
 * animations are made for standing), so with one pressed, buffered (+0x176 & 3 = 1), queued
 * (+0x177 = 2) or out (+0x174 & 0x40) the game's own states handle it as in the original: the
 * walks stop for it, the idle fires it standing, and only the weapons the game lets fire running
 * (the weapon table's byte 0) do so in the run. Also the game's FUN_004ad3a7 (pre_00): a shot
 * out whose weapon may not move (table byte 1, +0x170 special id, +0x171 buster id) roots him. */
#define G_WEAPON_TABLE GA(0x550F7Cu)

static int ws_special(void)
{
    uint16_t m = P16(PL_MASK_SPECIAL);
    int f = P8(PL_FIRE_FLAGS) & 0xC0;
    return (f & 0x40) || P8(PL_FIRE_PENDING) == 2 || (P8(PL_FIRE_BUFFER) & 3) == 1 ||
           ((P16(PL_HELD) | P16(PL_PRESSED)) & m) ||
           (f == 0x80 && !GVAR(uint8_t, G_WEAPON_TABLE + 1 + P8(0x171) * 12u));
}

static void ws_show(void)
{
    if (!ws_hidden)
        return;
    ws_hidden = 0;
    P8(PL_FIRE_FLAGS) |= ws_fire;
    P8(PL_FIRE_PENDING) = ws_pending;
    P8(PL_FIRE_BUFFER) = ws_buffer;
    P16(PL_HELD) |= ws_held;
    P16(PL_PRESSED) |= ws_pressed;
}

/* before the pre-handler call (EBP = the object) */
static void ws_hide(void)
{
    uint16_t fire = P16(PL_MASK_BUSTER) | P16(PL_MASK_SPECIAL);
    ws_hidden = 1;
    ws_fire = P8(PL_FIRE_FLAGS) & 0xC0;
    P8(PL_FIRE_FLAGS) &= (uint8_t)~0xC0;
    ws_pending = P8(PL_FIRE_PENDING);
    P8(PL_FIRE_PENDING) = 0;
    ws_buffer = P8(PL_FIRE_BUFFER);
    P8(PL_FIRE_BUFFER) = 0;
    ws_held = P16(PL_HELD) & fire;
    P16(PL_HELD) &= (uint16_t)~fire;
    ws_pressed = P16(PL_PRESSED) & fire;
    P16(PL_PRESSED) &= (uint16_t)~fire;
}

/* Standing with a shot out, the idle (pre_00) won't walk or sidestep until the shot is over and
 * only runs once its animation passed the weapon's cooldown frame (FUN_004ad2ca), so a move
 * pressed mid-shot waited for it. With a move held, the idle's handler doesn't see the shot
 * either. A run's start (state 2 sub 0) clears the shot (player_save_anim: +0x174, +0x177, the
 * arm channel to the body's animation), which never happens mid-shot in the original: so the
 * shot's state is kept from before and put back after the new state's handler, on the arms. */
static int ws_from_idle;
static uint8_t ws_s174, ws_s177, ws_s104, ws_s105;
static uint32_t ws_s100, ws_sf8, ws_sfc;

static int ws_move_held(void)
{
    return (P16(PL_HELD) & (P16(PL_MASK_FWD) | P16(PL_MASK_BACK) | P16(PL_MASK_RUN_L) | P16(PL_MASK_RUN_R) |
                            P16(PL_MASK_STEP_L) | P16(PL_MASK_STEP_R))) != 0;
}

/* Straight into the run's stride (state 2 sub 4) in direction dir, as its sub 0 and sub 2 would leave it
 * (speed, the arms flag, +0x54D, +0x175 bit 0 that the buster needs) but without the start-up
 * animation; from the forward walk the run keeps the walk's leg phase (player_set_anim's mapping). */
static void ws_enter_run(int dir)
{
    int entry = 0;
    if (P8(PL_ANIM_ID) == 0x0C) {
        entry = P8(PL_ANIM_ENTRY) - 4;
        if (entry < 0)
            entry += 0x20;
        entry >>= 1;
    }
    P8(PL_STATE) = 2;
    P8(PL_SUBSTATE) = 4;
    P8(PL_VARIANT) = 0;
    P8(PL_RUN_DIR_PREV) = 0;
    P8(PL_RUN_DIR) = (uint8_t)dir;
    S16(PL_MOVE_ANGLE) = (int16_t)(dir == 1 ? 0x400 : dir == 3 ? -0x400 : 0);   /* (re-aimed at apply time) */
    S16(PL_VEL_FWD) = 0x200;
    P8(PL_FIRE_FLAGS) |= 4;
    P8(0x54D) = 1;
    P8(0x175) |= 1;
    P8(PL_ANIM_ID) = 1;
    P8(PL_ANIM_ENTRY) = (uint8_t)entry;
}

/* The walk-mode states don't lead into each other: the walk's pre_01 knows no sidestep and the
 * sidestep's pre_06 no run, so each change went through the idle for an update (with a shot out,
 * in its planted shot pose), and a run from them through the run's start-up animation. In a face
 * hold (and with a shot out in any mode) the state the held buttons ask for is set right away, in
 * pre_00's order: walk + forward = walk, walk + strafe = sidestep, walk + back = walk back; in a
 * face hold, a direction without walk from a walk-mode state = the run in full stride. */
static void ws_director(void)
{
    int st = P8(PL_STATE), want, var = 0, hold = cam_active && face_hold;
    uint16_t held = P16(PL_HELD);
    if (st != 1 && st != 6)
        return;
    if (!hold && !ws_firing())
        return;
    if (held & P16(PL_MASK_WALK)) {
        if (held & P16(PL_MASK_FWD))
            want = 1;
        else if (held & (P16(PL_MASK_STEP_L) | P16(PL_MASK_STEP_R)))
            want = 6;
        else if (held & P16(PL_MASK_BACK))
            want = 1, var = 1;
        else
            return;
        if (want == st && (st == 6 || var == P8(PL_VARIANT)))
            return;
        P8(PL_STATE) = (uint8_t)want;
        P8(PL_SUBSTATE) = 0;
        P8(PL_VARIANT) = (uint8_t)var;
        S16(PL_MOVE_ANGLE) = 0;
        return;
    }
    if (!hold)
        return;
    if (held & P16(PL_MASK_FWD))
        ws_enter_run(0);
    else if (held & P16(PL_MASK_RUN_L))
        ws_enter_run(1);
    else if (held & P16(PL_MASK_RUN_R))
        ws_enter_run(3);
}

/* before the pre-handler call (EBP = the object) */
static void on_pre_handler(regs_t *r)
{
    int st;
    ws_from_idle = 0;
    if (r->ebp == G_PLAYER)
        ws_update++;
    if (r->ebp != G_PLAYER || !g_walk_shoot || !ws_hooked || (P8(PL_INPUT_FLAGS) & 0x60) || ws_special())
        return;
    ws_director();
    st = P8(PL_STATE);
    if (st == 0) {
        if (!ws_move_held())
            return;
        if (!(P8(PL_FIRE_FLAGS) & 0xC0))
            try_shoot(1);   /* moving off while firing: the shot on the arms from the start */
        if (P8(PL_STATE) != 0 || !ws_firing())
            return;   /* (a weapon that can't fire on the move: the idle fires it standing) */
        ws_s174 = P8(PL_FIRE_FLAGS), ws_s177 = P8(PL_FIRE_PENDING);
        ws_s104 = P8(0x104), ws_s105 = P8(0x105);
        ws_s100 = GVAR(uint32_t, PL(0x100)), ws_sf8 = GVAR(uint32_t, PL(0xF8)), ws_sfc = GVAR(uint32_t, PL(0xFC));
        ws_from_idle = 1;
        ws_hide();
        return;
    }
    if (st != 1 && st != 6)
        return;
    /* +0x175 bit 0 = may fire (try_shoot): the idle sets it when no shot is out (state_00 sub 0),
       a run's start, a stop and a jump clear it until they're done. The walk-mode states never set
       it: stock they are only entered from the idle. Entered here from a run's start or a jump's
       landing, he backpedaled with no shots until he stood still. */
    if (!ws_firing())
        P8(0x175) |= 1;
    if (!(P8(PL_FIRE_FLAGS) & 0xC0))
        try_shoot(1);   /* (does nothing without a shot to fire; fires a queued follow-up shot) */
    if (P8(PL_STATE) != 1 || !ws_firing())
        return;   /* the sidestep keeps going by itself; a weapon that can't fire walking stopped him */
    ws_hide();
}

/* between the two calls: the walk's state handler is the only one that must not see the shot */
static void on_state_handler(regs_t *r)
{
    (void)r;
    ws_in_state = 1;
    if (ws_hidden && P8(PL_STATE) != 1)
        ws_show();
}

/* backpedal=jog: past a light tilt the backpedal plays the run (anim 1, a 16-pose loop) backward,
 * a backward jog with the run's long stride instead of the walk back's short one. The walk state
 * picks its own animation every update (0x0D), so after its handler the body is put on the run at
 * our pose: the entry (+0x98, +0xA4) set directly with a long hold (+0xA5) so anim_tick doesn't step
 * it; on the update it switches, anim_start begins the run at that pose (+0xA4 = entry index). The
 * steps go at the square root of the speed ratio, like the slowed run. */
static int jog_on;
static double jog_phase;

static void jog_drive(void)
{
    uint32_t tbl;
    int idx;
    if (!(g_backjog && backpedal && face_hold && !move_walk && P8(PL_STATE) == 1 && P8(PL_VARIANT) == 1 &&
          !(P8(PL_INPUT_FLAGS) & 0x60))) {
        if (jog_on) {
            jog_on = 0;
            if (P8(PL_ANIM_ID) == 1)
                P8(PL_ANIM_HOLD) = 1;   /* (the run goes on from this pose, forward) */
        }
        return;
    }
    tbl = GVAR(uint32_t, G_ANIM_SCRIPTS + 1 * 4);
    if (!tbl)
        return;
    if (!jog_on) {
        /* from the run (anim 1 last update, the strafe run or its stride): on from its pose, so the
           legs just turn round; from the walk back, from the start */
        uint32_t cur = GVAR(uint32_t, PL(0x98));
        jog_on = 1;
        jog_phase = 0;
        if (P8(PL_ANIM_PREV) == 1 && cur >= tbl && cur < tbl + 16 * 8)
            jog_phase = (cur - tbl) / 8 + 0.5;
    }
    jog_phase -= sqrt(clampd(loco_pct * 0x50 / 100.0 / 0x200, 0.05, 1.0));
    while (jog_phase < 0)
        jog_phase += 16;
    idx = (int)jog_phase & 15;
    P8(PL_ANIM_ID) = 1;
    if (P8(PL_ANIM_PREV) != 1) {
        P8(PL_ANIM_ENTRY) = (uint8_t)idx;
    } else {
        GVAR(uint32_t, PL(0x98)) = tbl + (uint32_t)idx * 8;
        GVAR(uint32_t, PL(0xA4)) = GVAR(uint32_t, tbl + (uint32_t)idx * 8);
        P8(PL_ANIM_HOLD) = 0x7F;
    }
}

/* A shot on the run plays an arm animation made for the run's direction (FUN_004b5741 / 57b5 by
 * +0x112: 0x61/0x41 forward, 0x63/0x43 the left strafe, 0x62/0x42 the right), each twisting the torso
 * back toward the target by that much. It is picked when a shot starts, while the body turn (+0x116)
 * now eases: after a left-right reversal the body had swung across while the arms still twisted for
 * the old side, and the twists added up instead of cancelling: he faced the camera for a moment. So
 * the arm animation follows the body's actual turn (beyond +-45 degrees, with some hysteresis: the
 * strafe's; else the forward one), swapped at the same pose so the shot keeps its timing (the three
 * have the same length; a weapon without them is left alone). The blend smooths each swap. */
static uint32_t arms_table(int id)
{
    uint32_t base;
    if ((id & 0xF0) == 0x60)
        base = P8(0x171) ? 0x70DAACu : 0x70D62Cu;   /* (as FUN_004de8ee picks them) */
    else
        base = P8(0x170) ? 0x7500ACu : 0x75052Cu;
    return GVAR(uint32_t, base + (uint32_t)id * 4);
}

static void arms_follow_body(void)
{
    int id = P8(0x104), fam = id & 0xF0, side, body = S16(PL_MOVE_ANGLE), want;
    uint32_t tbl, old, idx, i;
    if (P8(PL_STATE) != 2 || (P8(PL_FIRE_FLAGS) & 0x04) == 0 || (P8(PL_FIRE_FLAGS) & 0xC0) == 0 ||
        (fam != 0x60 && fam != 0x40) || (id & 0x0F) < 1 || (id & 0x0F) > 3)
        return;
    side = (id & 0x0F) == 3 ? 1 : (id & 0x0F) == 2 ? -1 : 0;   /* +1: turned +0x400 (dir 1) */
    if (side == 0 || body * side < 448)
        side = body > 576 ? 1 : body < -576 ? -1 : 0;
    want = fam | (side > 0 ? 3 : side < 0 ? 2 : 1);
    if (want == id || !(tbl = arms_table(want)))
        return;
    if (P8(0x105) != (uint8_t)id) {   /* the channel restarts this update (a new shot): with the new one */
        P8(0x104) = (uint8_t)want;
        return;
    }
    old = GVAR(uint32_t, PL(0xF8));
    idx = (GVAR(uint32_t, PL(0xFC)) - old) / 8;
    for (i = 0; i < idx; i++)   /* (the new one must reach that pose) */
        if (GVAR(uint8_t, tbl + i * 8 + 3) & 0x80)
            return;
    {
        uint8_t hold = P8(0x101);
        P8(0x104) = P8(0x105) = (uint8_t)want;
        GVAR(uint32_t, PL(0xF8)) = tbl;
        GVAR(uint32_t, PL(0xFC)) = tbl + idx * 8;
        GVAR(uint32_t, PL(0x100)) = GVAR(uint32_t, tbl + idx * 8);
        P8(0x101) = hold;
    }
}

void controls_backpedal_style(int jog)
{
    g_backjog = jog != 0;
    log_printf("controls: backpedal while shooting: %s", g_backjog ? "backward jog (experimental)" : "walk back");
}

static void on_after_state(regs_t *r)
{
    int st;
    (void)r;
    ws_in_state = 0;
    ws_show();
    jog_drive();
    arms_follow_body();
    /* a jump in a face hold drifts by its turned speed (on_apply_velocity), so the body turn the game
       gives a jump's takeoff (+0x116 = 0x800 backward) would only flip his body for a frame */
    if (face_hold && ws_hooked && (P8(PL_STATE) == 7 || P8(PL_STATE) == 8))
        S16(PL_MOVE_ANGLE) = 0;
    if (!ws_from_idle)
        return;
    ws_from_idle = 0;
    st = P8(PL_STATE);
    if (st != 1 && st != 2 && st != 6)
        return;
    /* moved off mid-shot: the shot goes on, on the arms (the new state's animation on the body) */
    P8(PL_FIRE_FLAGS) = (uint8_t)(ws_s174 | 4);
    P8(PL_FIRE_PENDING) = ws_s177;
    P8(0x104) = ws_s104, P8(0x105) = ws_s105;
    GVAR(uint32_t, PL(0x100)) = ws_s100, GVAR(uint32_t, PL(0xF8)) = ws_sf8, GVAR(uint32_t, PL(0xFC)) = ws_sfc;
}

void controls_walk_shoot(int on)
{
    g_walk_shoot = on != 0;
    log_printf("controls: walking while shooting %s", g_walk_shoot ? "on" : "off");
}

static int g_style_notice;   /* no combat style chosen yet: a notice once, when play starts */

void controls_style_notice(int show)
{
    g_style_notice = show != 0;
}

static void on_player_update(regs_t *r)
{
    if (r->eax != G_PLAYER)
        return;
    if (g_style_notice && controls_in_gameplay()) {
        /* (for whoever skips the start menu, which asks; Later there brings it here too) */
        char how[96];
        g_style_notice = 0;
        pad_expand("Pick one in the settings: " PT_BACK " + " PT_START ", or F1", how, sizeof how);
        toast_post(TOAST_ICON_PAD, TOAST_GOOD, "COMBAT STYLES", "NEW", how);
    }
    shot_pitch_sync();   /* (shots spawn during the object updates) */
    if (air_steered && P8(PL_STATE) != 7 && P8(PL_STATE) != 8) {
        /* landed from a jump steered in a face hold: body back on the facing. A run in full
           swing sets its own heading; the landing's brief run start (sub < 3) may still drop
           to idle, so wait for it to go either way */
        if (P8(PL_STATE) != 2) {
            S16(PL_MOVE_ANGLE) = 0;
            air_steered = 0;
        } else if (P8(PL_SUBSTATE) >= 3) {
            if (!face_hold)   /* (in a face hold the run's heading is set at apply time) */
                S16(PL_MOVE_ANGLE) = 0;
            air_steered = 0;
        }
    }
    if (talk_hidden) {
        P16(PL_MASK_TALK) = talk_saved;
        talk_hidden = 0;
    }
    if (rail_mode()) {   /* the boat rides: analog aim (the orbit isn't driving here) */
        rail_apply();
        rail_crosshair();
        return;
    }
    rail_zoom_reset();
    if (!cam_active) {   /* orbit off: stock controls, but let a roll in flight finish tuned */
        if (roll_ours)
            roll_tune();
        roll_latch = roll_ours;
        return;
    }
    /* a press that can't start yet (mid-roll, mid-air) stays buffered for a few updates */
    /* stock aim with the mouse: turn (facing) and tilt (the aim angle +0x108,
     * which the aim state also feeds to its camera) */
    {
        int dx = (int)InterlockedExchange(&g_aim_mdx, 0), dy = (int)InterlockedExchange(&g_aim_mdy, 0);
        if ((dx || dy) && P8(PL_STATE) == 14 && !(P8(PL_INPUT_FLAGS) & 0x60)) {
            int cur = (RD32(G_PLAYER_FACING) >> 16) & 0xfff, pitch;
            write_facing(cur + iround(dx * MOUSE_YAW_PER_COUNT * g_mouse_scale));
            pitch = S16(PL_AIM_PITCH) - iround(dy * MOUSE_PITCH_PER_COUNT * g_mouse_scale * AIM_PITCH_SIGN *
                                                (g_mouse_invert_y ? -1 : 1));
            if (pitch > AIM_PITCH_LIMIT) pitch = AIM_PITCH_LIMIT;
            if (pitch < -AIM_PITCH_LIMIT) pitch = -AIM_PITCH_LIMIT;
            S16(PL_AIM_PITCH) = (int16_t)pitch;
        }
    }
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

    /* A run can't change direction: pre_02 drops to idle for it and the run restarts (a shot in
     * between goes out standing and roots him through the weapon's cooldown). In a face hold the
     * direction relative to the facing changes whenever the facing turns, so while running it is
     * switched in place: the run animation is the same for all four (anim 1; +0x116, set at apply
     * time, turns the body), +0x112 only picks the rules (which runs may shoot). Leaving the face
     * hold mid-run goes back to the forward run, travel unchanged. */
    /* backpedal: a run (or its start) becomes the walk back, facing as held (the run's body
     * turn +0x116 and direction back to straight) */
    if (cam_active && backpedal && move_active && inject_fwd && !roll_ours && !(P8(PL_INPUT_FLAGS) & 0x60) &&
        P8(PL_STATE) == 2) {
        P8(PL_STATE) = 1;
        P8(PL_SUBSTATE) = 0;
        P8(PL_VARIANT) = 1;   /* (player_try_walk_back: sub 0, variant 1) */
        P8(PL_RUN_DIR_PREV) = P8(PL_RUN_DIR);
        P8(PL_RUN_DIR) = 0;
        S16(PL_MOVE_ANGLE) = 0;
    }
    if (cam_active && move_active && inject_fwd && !roll_ours && !(P8(PL_INPUT_FLAGS) & 0x60) &&
        P8(PL_STATE) == 2 && (P8(PL_SUBSTATE) == 3 || P8(PL_SUBSTATE) == 4)) {
        int want = 0;
        if (face_hold) {
            int rel = ((move_face - face_dir + 2048) & 0xfff) - 2048;
            want = rel >= -512 && rel <= 512 ? 0 : rel > 512 && rel <= 1536 ? 3 : rel < -512 && rel >= -1536 ? 1 : 2;
        } else if (P8(PL_RUN_DIR) != 0) {
            write_facing(move_face);   /* facing = travel, body where it was */
            S16(PL_MOVE_ANGLE) = 0;
        }
        if (want != P8(PL_RUN_DIR)) {
            P8(PL_RUN_DIR_PREV) = P8(PL_RUN_DIR);
            P8(PL_RUN_DIR) = (uint8_t)want;
        }
    }

    loco_pct = 100;
    if (move_active && inject_fwd && !roll_ours)
        locomotion_tune();

    /* analog walk: a light tilt adds the walk modifier (Triangle) to what the
     * player holds, so the game picks its own walk state */
    if (walk_injected && !(GVAR(uint16_t, G_PAD_HELD) & P16(PL_MASK_WALK)))
        P16(PL_HELD) &= (uint16_t)~P16(PL_MASK_WALK);   /* in case held wasn't refreshed yet */
    walk_injected = 0;
    if (skate_update())
        return;
    /* in a face hold, a light tilt walks too (sidestep / backstep facing the target or the
     * camera's way), but only from standing or walking: walk + strafe while running turns in place.
     * The backpedal walks at any tilt. */
    if ((move_walk || backpedal) && move_active && inject_fwd && !roll_ours && !(P8(PL_INPUT_FLAGS) & 0x60) &&
        (!face_hold || P8(PL_STATE) == 0 || P8(PL_STATE) == 1 || P8(PL_STATE) == 6))
        inject_walk();
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
    if (cam_active && (st == 3 || skate_air))
        skate_velocity(r, st);
    /* in a face hold the run state only knows 4 directions; steer the travel to
     * the exact stick heading (travel = facing - +0x116) */
    if (face_hold && move_active && st == 2) {
        int f = (RD32(G_PLAYER_FACING) >> 16) & 0xfff;
        S16(PL_MOVE_ANGLE) = (int16_t)(((f - move_face + 2048) & 0xfff) - 2048);
    }
    /* jumps move along the facing too (travel = facing - +0x116), so turning him to shoot in
     * the air swung the drift to the target / the camera's way. In a face hold the drift
     * follows the stick, or keeps the heading it had (from before the hold) when the stick
     * is let go; outside one it is just tracked. */
    if (st == 7 || st == 8) {
        int f = (RD32(G_PLAYER_FACING) >> 16) & 0xfff;
        if (!face_hold) {
            air_travel = (f - S16(PL_MOVE_ANGLE)) & 0xfff;
        } else {
            /* the drift goes along the stick (or keeps its heading when the stick is let go), the
               body stays on the facing: the speed itself is turned (as for the steps), not the body
               (+0x116, which turned his back to the target when jumping backward); while shooting
               the drift is slower toward the back, like moving on the ground */
            double fwd = (int16_t)r->edx, side = (int16_t)r->ecx, sp = sqrt(fwd * fwd + side * side), a;
            if (move_active)
                air_travel = move_face & 0xfff;
            if (sp > 0 && ws_in_state) {
                if (combat)
                    sp = sp * back_curve(signed12((air_travel - face_dir) & 0xfff)) / 100;
                a = signed12((air_travel - f) & 0xfff) * (PI / 2048.0);
                r->edx = (uint32_t)(int32_t)iround(sp * cos(a));
                r->ecx = (uint32_t)(int32_t)iround(-sp * sin(a) * WS_SIDE_SIGN);
                S16(PL_MOVE_ANGLE) = 0;
            } else {
                S16(PL_MOVE_ANGLE) = (int16_t)(((f - air_travel + 2048) & 0xfff) - 2048);
            }
            air_steered = 1;
        }
    }
    if (loco_pct != 100) {
        if (st == 1 || st == 2)
            r->edx = (uint32_t)(int32_t)((int16_t)r->edx * loco_pct / 100);
        else if (st == 6)   /* sidestep moves on the side axis */
            r->ecx = (uint32_t)(int32_t)((int16_t)r->ecx * loco_pct / 100);
    }
    /* Face-hold movement (the walk-mode steps and the run): the steps only go straight forward,
     * back or sideways, so their speed goes along the exact stick heading instead, the body still
     * facing the target / the camera's way (+0x116 = 0); the run turns its body with +0x116 toward
     * where it goes. And with momentum: the velocity eases toward the stick's (MOVE_EASE per
     * update) instead of jumping, across changes of direction, speed and state. Only the state
     * handler's own step (the weapon's recoil push is left alone). */
    if (face_hold && move_active && ws_in_state && (st == 1 || st == 6 || st == 2)) {
        static double vx, vz;            /* the eased velocity, world angle frame (x = sin, z = cos) */
        static unsigned last_update;
        static double body;              /* the run's body turn (+0x116), eased */
        static int body_ok;              /* (it follows a run that is going on) */
        double fwd = (int16_t)r->edx, side = (int16_t)r->ecx, sp = sqrt(fwd * fwd + side * side), a, va, vs;
        int f = (RD32(G_PLAYER_FACING) >> 16) & 0xfff, fresh = ws_update - last_update > 2;
        if (fresh)
            vx = vz = 0;                 /* (starting off: from standing) */
        last_update = ws_update;
        a = (move_face & 0xfff) * (PI / 2048.0);
        vx += (sp * sin(a) - vx) * MOVE_EASE;
        vz += (sp * cos(a) - vz) * MOVE_EASE;
        vs = sqrt(vx * vx + vz * vz);
        va = atan2(vx, vz);
        if (st == 2) {
            /* the run: the body turns toward the travel (+0x116), eased so a left-right reversal swings
               it across the front instead of flipping it in one update; the speed goes along the exact
               travel in the body's frame meanwhile */
            double want = signed12((f - iround(va * (2048.0 / PI))) & 0xfff), d, rel;
            if (fresh || !body_ok)
                body = want;
            body_ok = 1;
            d = want - body;
            if (d > 2304)
                d -= 4096;
            else if (d < -2304)
                d += 4096;
            body += d * BODY_EASE;
            if (body > 2048)
                body -= 4096;
            else if (body < -2048)
                body += 4096;
            S16(PL_MOVE_ANGLE) = (int16_t)iround(body);
            rel = va - (f - body) * (PI / 2048.0);
            r->edx = (uint32_t)(int32_t)iround(vs * cos(rel));
            r->ecx = (uint32_t)(int32_t)iround(-vs * sin(rel) * WS_SIDE_SIGN);
        } else {
            double rel = va - f * (PI / 2048.0);
            body_ok = 0;
            r->edx = (uint32_t)(int32_t)iround(vs * cos(rel));
            r->ecx = (uint32_t)(int32_t)iround(-vs * sin(rel) * WS_SIDE_SIGN);
            S16(PL_MOVE_ANGLE) = 0;
        }
    }
}

static void on_cam_set_b(regs_t *r)
{
    if (cam_active && !cam_follow)
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
    if (face_hold || P8(PL_STATE) == 3)   /* (on skates the camera hook turns him at its own rate) */
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
    if (rail_live()) {   /* the boat rides: modern buttons, analog aim */
        rail_filter(p);
        return;
    }
    if (!orbit_live()) {
        /* menus and the classic camera keep the game's own keys; the bound Menu and
         * Select keys still work there (the key that opened a menu also closes it) and
         * the movement keys move menu cursors */
        if (g_kbm && g_modern && kbm_foreground()) {
            int x = (slot_down(KS_RIGHT) ? 1 : 0) - (slot_down(KS_LEFT) ? 1 : 0);
            int y = (slot_down(KS_UP) ? 1 : 0) - (slot_down(KS_DOWN) ? 1 : 0);
            if (slot_down(KS_MENU))
                p->wButtons |= BTN_START;
            if (slot_down(KS_SELECT))
                p->wButtons |= BTN_BACK;
            if (x || y) {   /* the movement keys move menu cursors too */
                p->sThumbLX = (SHORT)(x * 32767);
                p->sThumbLY = (SHORT)(y * 32767);
            }
        }
        return;
    }

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
    acts = actions_of(p) | (unsigned)g_kbm_acts;
    roll = ACT(acts, ACT_ROLL) != 0;
    aiming = ACT(acts, ACT_AIM) != 0;
    p->bLeftTrigger = 0;
    p->bRightTrigger = 0;
    out = p->wButtons & (BTN_DPAD_UP | BTN_DPAD_DOWN | BTN_DPAD_LEFT | BTN_DPAD_RIGHT);
    if (ACT(acts, ACT_JUMP))    out |= BTN_A;      /* Cross */
    /* camera aim: a shot waits (briefly) until Mega Man has turned to the camera's way */
    if (ACT(acts, ACT_SHOOT) && aim_ready)   out |= BTN_X;      /* Square: buster */
    if (ACT(acts, ACT_SPECIAL) && aim_ready) out |= BTN_B;      /* Triangle: special weapon */
    if (ACT(acts, ACT_TALK))    out |= BTN_Y;      /* Circle: talk / check / walk */
    if (ACT(acts, ACT_MENU))    out |= BTN_START;
    if (ACT(acts, ACT_SELECT))  out |= BTN_BACK;
    if (aiming && !lock_active && !lock_free && !sh_aim)   /* locked on / shoulder aim: no stock aim state, it roots Mega Man */
        out |= BTN_LB;                             /* R2 */

    if (roll && !prev_roll && (!aiming || lock_active || lock_free || sh_aim)) {
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

    if (face_hold && (P8(PL_STATE) == 7 || P8(PL_STATE) == 8)) {
        /* in a jump: forward, as without a face hold (left/right would be the game's own air
           steering, relative to the turned facing); the heading is set at apply time */
        p->sThumbLX = 0;
        p->sThumbLY = (SHORT)((fresh && move_active && inject_fwd) ? 32767 : 0);
    } else if (face_hold) {
        /* the stick picks the game's run direction relative to the held facing
         * (forward / strafe / back); the exact heading is set at apply time */
        p->sThumbLX = p->sThumbLY = 0;
        if (fresh && move_active && inject_fwd) {
            int rel = ((move_face - face_dir + 2048) & 0xfff) - 2048;
            if (backpedal)   /* (from 100 degrees in a burst; its heading is set at apply time) */
                p->sThumbLY = -32767;
            else if (rel >= -512 && rel <= 512)
                p->sThumbLY = 32767;
            else if (rel > 512 && rel <= 1536)
                p->sThumbLX = 32767;
            else if (rel < -512 && rel >= -1536)
                p->sThumbLX = -32767;
            else
                p->sThumbLY = -32767;
        }
    } else if (aiming && !lock_free && !sh_aim) {
        SHORT lx = p->sThumbLX, ly = p->sThumbLY;
        p->sThumbLX = p->sThumbRX;
        p->sThumbLY = p->sThumbRY;
        p->sThumbRX = lx;
        p->sThumbRY = ly;
        if (!p->sThumbLX && !p->sThumbLY && (g_kbm_lx || g_kbm_ly)) {   /* keyboard: A/D turn, W/S tilt */
            p->sThumbLX = (SHORT)g_kbm_lx;
            p->sThumbLY = (SHORT)g_kbm_ly;
        }
    } else if (fresh && move_active && inject_fwd) {
        p->sThumbLX = 0;
        p->sThumbLY = 32767;   /* walk vs run is decided in the player hook */
    }
}

/* The game's button configuration (its Turn option, Rotational or Directional, saved with the game; one
 * of two preset tables in the player's action masks +0x124...) decides which PS1 button does what. The
 * modern layer speaks the default one (Rotational): D-pad Up/Down = forward/back, Left/Right = run (strafe) left/
 * right, Cross jump, Square buster, Triangle special, Circle talk, R2 stock aim. With another
 * configuration the same buttons mean something else: with Directional, Left/Right turn (L1/R1 strafe):
 * every strafe while firing became a turn in place (state 4), which dropped the camera aim's face
 * hold, which sent a forward run, which the strafe dropped again: a loop in place with no shots
 * (issue #11). So each default button is handed to the game as the button its own mask names. */
unsigned short controls_remap_bits(unsigned short bits)
{
    static const struct { uint16_t off, def; int dir; } map[] = {
        { PL_MASK_FWD, 0x0010, 1 }, { PL_MASK_BACK, 0x0040, 1 }, { PL_MASK_RUN_L, 0x0080, 1 },
        { PL_MASK_RUN_R, 0x0020, 1 }, { PL_MASK_JUMP, 0x4000, 0 }, { PL_MASK_BUSTER, 0x8000, 0 },
        { PL_MASK_SPECIAL, 0x1000, 0 }, { PL_MASK_TALK, 0x2000, 0 }, { PL_MASK_AIM, 0x0200, 0 },
    };
    uint16_t moved = 0, to = 0;
    int i;
    if (!orbit_live() || rail_live())   /* menus, the classic camera, the boat rides: the game's own layout */
        return bits;
    for (i = 0; i < (int)(sizeof map / sizeof map[0]); i++) {
        uint16_t m;
        if (!(bits & map[i].def) || (map[i].dir && cam_suppress))   /* (the stock aim turns with the D-pad as is) */
            continue;
        m = map[i].off == PL_MASK_TALK && talk_hidden ? talk_saved : P16(map[i].off);
        if (m && m != map[i].def) {
            moved |= map[i].def;
            to |= m;
        }
    }
    return (unsigned short)((bits & ~moved) | to);
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
        int playing = GVAR(uint8_t, G_INTRO_PLAYING);
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
                GVAR(uint8_t, G_INTRO_PLAYING) = 0;
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
    /* RB had no action before the Jet Skates got one, and older settings wrote that default
       (btn_rb=none) into mml.ini: hand it over once, unless the skates are on a button already */
    if (!GetPrivateProfileIntA("controls", "skates_rb", 0, ini)) {
        int taken = 0;
        for (i = 0; i < PH_COUNT; i++) {
            GetPrivateProfileStringA("controls", g_ph_keys[i], "", v, sizeof v, ini);
            taken |= _stricmp(v, "skates") == 0;
        }
        GetPrivateProfileStringA("controls", "btn_rb", "none", v, sizeof v, ini);
        if (!taken && _stricmp(v, "none") == 0) {
            WritePrivateProfileStringA("controls", "btn_rb", "skates", ini);
            log_printf("controls: RB (unbound) is now the Jet Skates button");
        }
        WritePrivateProfileStringA("controls", "skates_rb", "1", ini);
    }
    /* L3 and R3 both toggled modern controls by default; R3 is now the shoulder swap (once, and
       only where both still had that old default) */
    strcpy_s(g_ini_path, sizeof g_ini_path, ini);
    if (!GetPrivateProfileIntA("controls", "shoulder_r3", 0, ini)) {
        char l3[32];
        GetPrivateProfileStringA("controls", "btn_l3", "orbit", l3, sizeof l3, ini);
        GetPrivateProfileStringA("controls", "btn_r3", "orbit", v, sizeof v, ini);
        if (_stricmp(l3, "orbit") == 0 && _stricmp(v, "orbit") == 0) {
            WritePrivateProfileStringA("controls", "btn_r3", "shoulder", ini);
            log_printf("controls: R3 is now the shoulder swap (L3 toggles modern controls)");
        }
        WritePrivateProfileStringA("controls", "shoulder_r3", "1", ini);
    }
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

void controls_load_keys(const char *ini)
{
    char v[64], line[512] = "";
    int i;
    for (i = 0; i < KS_COUNT; i++) {
        char *comma;
        g_slot_vk[i][0] = g_slot_info[i].def0;
        g_slot_vk[i][1] = g_slot_info[i].def1;
        GetPrivateProfileStringA("controls", g_slot_info[i].key, "", v, sizeof v, ini);
        if (!v[0])
            continue;
        comma = strchr(v, ',');
        if (comma)
            *comma++ = 0;
        g_slot_vk[i][0] = kbm_vk_from_name(v);
        g_slot_vk[i][1] = comma ? kbm_vk_from_name(comma) : 0;
        if (!g_slot_vk[i][0] && !g_slot_vk[i][1])
            log_printf("controls: %s=\"%s\" not understood, unbound", g_slot_info[i].key, v);
    }
    for (i = 0; i < KS_COUNT; i++)
        sprintf_s(line + strlen(line), sizeof line - strlen(line), "%s%s=%02X/%02X", i ? " " : "",
                  g_slot_info[i].key + 4, g_slot_vk[i][0], g_slot_vk[i][1]);
    log_printf("controls: keys %s", line);
}

/* while the orbit drives gameplay with KB+M on, the game's own keyboard
 * bindings (arrows, Z X C V, ...) are muted so remapped keys can't double up */
/* normal play: the follow camera running, no script, menu or conversation */
int controls_in_gameplay(void)
{
    return orbit_allowed();
}

int controls_mute_game_keys(void)
{
    return g_kbm && (orbit_live() || rail_live()) && kbm_foreground();
}

/* every pad poll (menus too): let the cursor go once the orbit (or the rail aim) isn't driving */
void controls_poll_idle(void)
{
    if (rail_live())
        kbm_capture(g_kbm);
    else if (!orbit_live())
        kbm_capture(0);
}

void controls_mouse(int enabled, int sens_pct, int invert_y)
{
    g_kbm = enabled != 0;
    if (sens_pct > 0)
        g_mouse_scale = sens_pct / 100.0;
    g_mouse_invert_y = invert_y != 0;
    log_printf("controls: keyboard + mouse %s, mouse speed %d%%%s", g_kbm ? "on" : "off",
               (int)(g_mouse_scale * 100 + 0.5), g_mouse_invert_y ? ", inverted" : "");
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
        cam_owned = 0;
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

void controls_skates(int max_pct, int min_pct, int toggle)
{
    if (max_pct >= 50 && max_pct <= 300)
        g_skate_max = max_pct;
    if (min_pct >= 0 && min_pct <= g_skate_max)
        g_skate_min = min_pct;
    g_skate_toggle = toggle != 0;
    log_printf("controls: Jet Skates %d%% of stock at full tilt, %d%% at a light tilt, button %s", g_skate_max,
               g_skate_min, g_skate_toggle ? "toggles" : "held");
}

void controls_aim_sensitivity(int pct)
{
    if (pct >= 10 && pct <= 200)
        g_aim_sens = pct;
    log_printf("controls: aim sensitivity %d%%", g_aim_sens);
}

int controls_aim_on_enemy(void)
{
    return rail_live() ? rail_on_enemy != 0 : aim_on_enemy != 0;
}

void controls_lockon_camera(int shoulder, int left)
{
    g_sh_mode = shoulder != 0;
    g_sh_side = left ? -1 : 1;
    sh_side_cur = g_sh_side;
    log_printf("controls: lock-on camera %s (%s shoulder)", g_sh_mode ? "over the shoulder" : "classic",
               left ? "left" : "right");
}

void controls_aim(int camera, int crosshair)
{
    g_aim_camera = camera != 0;
    g_crosshair = crosshair < 0 ? 0 : crosshair > 2 ? 2 : crosshair;
    log_printf("controls: shots aim %s, crosshair %s", g_aim_camera ? "where the camera looks" : "where Mega Man faces",
               g_crosshair == 2 ? "always" : g_crosshair ? "while aiming" : "off");
}

void controls_aim_button(int free_aim)
{
    g_aim_free = free_aim != 0;
    log_printf("controls: aim button %s", g_aim_free ? "free aim over the shoulder (no lock-on)" : "lock-on");
}

void controls_camera_shoulder(int on)
{
    g_sh_always = on != 0;
    log_printf("controls: camera %s", g_sh_always ? "over the shoulder all the time (aiming zooms in)" : "behind Mega Man");
}

void controls_aim_assist(int on)
{
    g_aim_assist = on != 0;
    log_printf("controls: aim assist %s", g_aim_assist ? "on" : "off");
}

int controls_aim_assist_on(void)
{
    return g_aim_assist;
}

/* called once per presented frame: the game's own menus and pauses stop the camera hook, which
   leaves the flags above as they were, so the camera must also have moved on recently. Shown
   from the first shot, it fades out XH_SHOW after the last one. */
#define XH_SHOW  (4.0 * 60)   /* 1/60 s */
#define XH_FADE  (0.4 * 60)
float controls_crosshair(void)
{
    static LONG last_ft;
    static int idle;
    if (frame_tick != last_ft) {
        last_ft = frame_tick;
        idle = 0;
    } else if (idle < 1000) {
        idle++;
    }
    if (rail_live()) {   /* the boat rides: while zoomed and after shots, where they go (rail_crosshair) */
        if (!g_crosshair)
            return 0;
        if (rail_zoom > 1.0)
            return 1.0f;
        if (rail_since_fire >= XH_SHOW + XH_FADE)
            return 0;
        return rail_since_fire <= XH_SHOW ? 1.0f : (float)(1.0 - (rail_since_fire - XH_SHOW) / XH_FADE);
    }
    if (!(g_crosshair && cam_active && !cam_suppress && !lock_active && idle < 6 && controls_in_gameplay()))
        return 0;
    if (sh_aim)   /* shoulder aim: shown the whole time you aim */
        return 1.0f;
    if (g_crosshair == 2 && g_aim_camera)   /* always (shots go where the camera looks) */
        return 1.0f;
    if (!g_aim_camera || since_fire >= XH_SHOW + XH_FADE)
        return 0;
    return since_fire <= XH_SHOW ? 1.0f : (float)(1.0 - (since_fire - XH_SHOW) / XH_FADE);
}

/* ---- install ---- */

void controls_install(int modern, int intro_pad_skip)
{
#ifndef MML_RUNNER
    g_fn_world_collision = G_FN_WORLD_COLLISION;
#endif
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
        log_printf("controls: %s (F5 / L3 toggles modern controls)", modern ? "modern" : "vanilla");
    else
        log_printf("controls: game hooks failed");
    {
        static const uint8_t handler_call[] = { 0x31, 0xD2, 0x8A, 0x55, 0x09 };   /* xor edx,edx; mov dl,[ebp+9] */
        static const uint8_t apply_local_call[] = { 0xE8, 0xF8, 0x42, 0xF8, 0xFF };   /* call player_apply_local_velocity */
#ifndef MML_RUNNER
        g_fn_try_shoot = G_FN_TRY_SHOOT;
#endif
        if (!hook_mid(G_SITE_PRE_CALL, sizeof handler_call, handler_call, on_pre_handler) ||
            !hook_mid(G_SITE_STATE_CALL, sizeof handler_call, handler_call, on_state_handler) ||
            !hook_mid(G_SITE_AFTER_STATE, sizeof apply_local_call, apply_local_call, on_after_state)) {
            log_printf("controls: walk shooting hooks failed");
        } else {
            ws_hooked = 1;
        }
    }

    /* vertical aim: FUN_0042A85C reads our flag and pitch instead of the aim camera's */
    {
        static const uint8_t cmp_flag[] = { 0x80, 0x3D, 0x58, 0xE9, 0x77, 0x00, 0x00 };   /* cmp byte [0x77E958],0 */
        static const uint8_t mov_pitch[] = { 0xA1, 0x64, 0xE9, 0x77, 0x00 };              /* mov eax,[0x77E964] */
        uint32_t flag_at = guest_alloc(8), pitch_at = flag_at + 4;
        if (flag_at && mem_expect(G_FN_SHOT_PITCH, cmp_flag, sizeof cmp_flag) &&
            mem_expect(G_FN_SHOT_PITCH + 0xC, mov_pitch, sizeof mov_pitch)) {
            g_shot_flag = (volatile uint8_t *)GUEST(flag_at);
            g_shot_pitch = (volatile int32_t *)GUEST(pitch_at);
            shot_pitch_sync();
            if (mem_write(G_FN_SHOT_PITCH + 2, &flag_at, 4) && mem_write(G_FN_SHOT_PITCH + 0xD, &pitch_at, 4))
                log_printf("controls: shots take their pitch from us (camera aim)");
            else
                log_printf("controls: shot pitch patch failed");
        } else {
            log_printf("controls: shot pitch getter unexpected, no vertical aim");
        }
    }

    if (intro_pad_skip)
        CloseHandle(CreateThread(NULL, 0, intro_skip_thread, NULL, 0, NULL));
}
