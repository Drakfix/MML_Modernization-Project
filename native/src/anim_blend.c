/* Animation blending ([video] anim_blend, on by default).
 *
 * The game switches Mega Man's animations from one update to the next: a new animation starts at
 * its first pose, so every change (an arm raised to shoot and lowered after, walk to sidestep, run
 * to backpedal, a landing) snaps. Each update the player's poses are unpacked into two buffers of
 * joint angles (FUN_004def3e: the body animation into 0x560E04, the arm channel's into 0x561604)
 * and the skeleton is built from them (FUN_004debda: the root offset and the legs from the body
 * buffer; the upper body, joints 0-7, from the arm buffer while a shot is out, +0x106 != 0, else
 * from the body buffer).
 *
 * At the skeleton builder's entry the buffers are blended by inertialization: when an animation
 * changes, each value's jump from what was shown last is kept as an offset that fades out over
 * BLEND_UPDATES updates on a smoothstep, on top of the new animation, which plays on underneath.
 * The upper body is blended as one pose whichever buffer feeds it, so starting and ending a shot
 * fades too. The 60 fps in-between frames then smooth it further. Angles are 12-bit (4096 a turn,
 * blended the short way round); the root offset is a signed 12-bit position. */
#include <math.h>
#include <string.h>
#include "anim_blend.h"
#include "addr.h"
#include "game.h"
#include "guest.h"
#include "hook.h"
#include "log.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#endif

#define G_FN_SKELETON   GA(0x4DEBDAu)   /* FUN_004debda(EAX p): the player's joints from the unpacked poses */
#define G_POSE_BODY     0x560E04u       /* s16[63]: root x, y, z, pad, then 15 joints of x, y, z, pad */
#define G_POSE_ARMS     0x561604u       /* s16[35]: the same layout, joints 0-7 (the upper body) */
#define POSE_N          63
#define UPPER_FROM      4               /* shorts 4..34: joints 0-7 */
#define UPPER_TO        35
#define BLEND_UPDATES   4               /* updates (30 a second) a change fades over */

static int g_on = 1, g_have;
static int16_t prev[POSE_N];            /* what was shown last: root + legs from the body buffer, upper body effective */
static float off_lower[POSE_N], off_upper[POSE_N];
static int k_lower = BLEND_UPDATES, k_upper = BLEND_UPDATES;
static uint8_t last_body, last_arms;
static uint16_t last_mode;
static int g_log;   /* (testing: MML_BLEND_LOG=1 logs each blended update of a change) */

static int is_angle(int i)
{
    return i >= 4;   /* (0-2: the root offset, a position) */
}

static float diff(int i, int a, int b)   /* a - b */
{
    if (!is_angle(i))
        return (float)(a - b);
    return (float)((((a - b) & 0xfff) + 2048) % 4096 - 2048);
}

static int16_t add(int i, int base, float off)
{
    int v = base + (int)lroundf(off);
    return (int16_t)(is_angle(i) ? (v & 0xfff) : v);
}

static float fade(int k)   /* 1 at the change .. 0 when done */
{
    float t = (float)k / BLEND_UPDATES;
    if (t >= 1.0f)
        return 0.0f;
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

static void on_skeleton(regs_t *r)
{
    int16_t *body = (int16_t *)GUEST(G_POSE_BODY), *arms = (int16_t *)GUEST(G_POSE_ARMS), *upper;
    uint8_t id_body, id_arms;
    uint16_t mode;
    int i, new_lower, new_upper;
    float wl, wu;
    if (r->eax != G_PLAYER)
        return;
    id_body = GVAR(uint8_t, G_PLAYER + PL_ANIM_ID);
    id_arms = GVAR(uint8_t, G_PLAYER + 0x104);
    mode = GVAR(uint16_t, G_PLAYER + 0x106);
    upper = mode ? arms : body;
    if (!g_on || !g_have) {
        g_have = 1;
        k_lower = k_upper = BLEND_UPDATES;
        goto remember;
    }
    new_lower = id_body != last_body;
    new_upper = mode != last_mode || (mode ? id_arms != last_arms : id_body != last_body);
    if (new_lower) {
        for (i = 0; i < POSE_N; i++)
            if (i < UPPER_FROM || i >= UPPER_TO)
                off_lower[i] = diff(i, prev[i], body[i]);
        k_lower = 0;
    }
    if (new_upper) {
        for (i = UPPER_FROM; i < UPPER_TO; i++)
            off_upper[i] = diff(i, prev[i], upper[i]);
        k_upper = 0;
    }
    /* (the first update of a change already moves a step toward the new pose) */
    if (k_lower < BLEND_UPDATES)
        k_lower++;
    if (k_upper < BLEND_UPDATES)
        k_upper++;
    wl = fade(k_lower);
    wu = fade(k_upper);
    if (wl > 0)
        for (i = 0; i < POSE_N; i++)
            if (i < UPPER_FROM || i >= UPPER_TO)
                body[i] = add(i, body[i], off_lower[i] * wl);
    if (wu > 0)
        for (i = UPPER_FROM; i < UPPER_TO; i++)
            upper[i] = add(i, upper[i], off_upper[i] * wu);
    if (g_log)
        log_printf("blend: st %d body %02X arms %02X mode %d k %d/%d turn %5d face %03X | j0 %4d %4d %4d j1 %4d %4d %4d",
                   GVAR(uint8_t, G_PLAYER + PL_STATE), id_body, id_arms, mode, k_lower, k_upper,
                   GVAR(int16_t, G_PLAYER + PL_MOVE_ANGLE), (GVAR(uint32_t, G_PLAYER_FACING) >> 16) & 0xfff,
                   upper[4], upper[5], upper[6], upper[8], upper[9], upper[10]);
remember:
    for (i = 0; i < POSE_N; i++)
        prev[i] = i >= UPPER_FROM && i < UPPER_TO ? upper[i] : body[i];
    last_body = id_body;
    last_arms = id_arms;
    last_mode = mode;
}

int anim_blend_install(void)
{
    static const uint8_t entry[] = { 0x53, 0x51, 0x52, 0x56, 0x57, 0x55 };   /* push ebx/ecx/edx/esi/edi/ebp */
    if (!hook_mid(G_FN_SKELETON, sizeof entry, entry, on_skeleton)) {
        log_printf("anim blend: hook failed");
        return 0;
    }
    return 1;
}

void anim_blend_set(int on)
{
    g_on = on != 0;
    g_have = 0;
#ifdef _WIN32
    {
        char v[8] = "";
        GetEnvironmentVariableA("MML_BLEND_LOG", v, sizeof v);
        g_log = v[0] == '1';
    }
#else
    g_log = getenv("MML_BLEND_LOG") && getenv("MML_BLEND_LOG")[0] == '1';
#endif
    log_printf("anim blend: %s", g_on ? "on (animation changes cross-fade)" : "off");
}
