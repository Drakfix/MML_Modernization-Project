/* Animation blending ([video] anim_blend, on by default).
 *
 * The game switches Mega Man's animations from one update to the next: a new animation starts at
 * its first pose, so every change (an arm raised to shoot and lowered after, walk to sidestep, run
 * to backpedal, a landing) snaps. Each update the player's poses are unpacked into two buffers of
 * joint angles (FUN_004def3e: the body animation into 0x560E04, the arm channel's into 0x561604)
 * and the skeleton is built from them (FUN_004debda: under the root's yaw, facing +0x56 minus the
 * body turn +0x116, the root offset, joint 0 and the legs from the body buffer; the upper body,
 * joints 0-7 again, from the arm buffer while a shot is out, +0x106 != 0, else from the body
 * buffer). A joint's matrix is Rx * Ry * Rz of its three 12-bit angles (FUN_004ef5f7), times its
 * parent's.
 *
 * At the skeleton builder's entry the buffers are blended by inertialization: when an animation
 * changes, each joint's rotation from the new pose to what was shown last is kept as an offset
 * that fades out over BLEND_UPDATES updates on a smoothstep, on top of the new animation, which
 * plays on underneath. The upper body is blended as one pose whichever buffer feeds it, so
 * starting and ending a shot fades too. The 60 fps in-between frames then smooth it further.
 *
 * Offsets are rotations (quaternions), not per-angle differences: two sets of angles far apart can
 * be nearly the same rotation, and fading each angle on its own then passes through poses neither
 * side has (the stop that turns him back toward the target after running toward the camera ends
 * with joint 0 spun 180 degrees while the root's yaw flips 180 the other way; per angle, the fade
 * from it to the idle laid him on his side for a few frames). Joint 0 hangs from the root's yaw,
 * so its offset counts the yaw's change too: what was shown keeps facing where it faced. The root
 * offset is a signed 12-bit position, turned the same way. */
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
#define JOINTS          15
#define UPPER_JOINTS    8               /* joints 0-7 */
#define LEG_FROM        8               /* joints 8-14: the legs, hung from joint 0 */
#define BLEND_UPDATES   4               /* updates (30 a second) a change fades over */
#define TURN_RAD        (6.28318530718f / 4096.0f)

typedef struct { float w, x, y, z; } quat;

static int g_on = 1, g_have;
static int16_t prev[POSE_N];            /* what was shown last: root + legs from the body buffer, upper body effective */
static int16_t prev_hip[3];             /* joint 0 as the legs hung from it (the body buffer's, also while a shot is out) */
static int prev_yaw;                    /* the root's yaw then */
static float off_root[3];
static quat off_hip, off_leg[JOINTS], off_up[UPPER_JOINTS];
static int k_lower = BLEND_UPDATES, k_upper = BLEND_UPDATES;
static uint8_t last_body, last_arms;
static uint16_t last_mode;
static int g_log;   /* (testing: MML_BLEND_LOG=1 logs each blended update of a change) */

static quat q_mul(quat a, quat b)
{
    quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

static quat q_axis(int axis, int ang)   /* ang (12-bit) about x (0), y (1) or z (2) */
{
    quat q = { cosf(ang * TURN_RAD * 0.5f), 0, 0, 0 };
    float s = sinf(ang * TURN_RAD * 0.5f);
    if (axis == 0)
        q.x = s;
    else if (axis == 1)
        q.y = s;
    else
        q.z = s;
    return q;
}

static quat q_euler(const int16_t *a)   /* a joint's rotation: Rx(a[0]) * Ry(a[1]) * Rz(a[2]) */
{
    return q_mul(q_mul(q_axis(0, a[0]), q_axis(1, a[1])), q_axis(2, a[2]));
}

static int16_t to_angle(float rad)
{
    return (int16_t)(lroundf(rad / TURN_RAD) & 0xfff);
}

static void q_to_euler(quat q, int16_t *a)   /* back to the x, y, z of Rx * Ry * Rz */
{
    float m00 = 1 - 2 * (q.y * q.y + q.z * q.z), m01 = 2 * (q.x * q.y - q.w * q.z);
    float m02 = 2 * (q.x * q.z + q.w * q.y), m12 = 2 * (q.y * q.z - q.w * q.x);
    float m22 = 1 - 2 * (q.x * q.x + q.y * q.y), m11 = 1 - 2 * (q.x * q.x + q.z * q.z);
    float m21 = 2 * (q.y * q.z + q.w * q.x);
    if (m02 > 1)
        m02 = 1;
    else if (m02 < -1)
        m02 = -1;
    a[1] = to_angle(asinf(m02));
    if (fabsf(m02) < 0.9999f) {
        a[0] = to_angle(atan2f(-m12, m22));
        a[2] = to_angle(atan2f(-m01, m00));
    } else {   /* (y straight up or down: x and z turn about the same axis) */
        a[0] = to_angle(atan2f(m21, m11));
        a[2] = 0;
    }
}

static quat q_offset(quat shown, quat now)   /* the rotation taking now to shown, the short way round */
{
    quat o = q_mul(shown, (quat){ now.w, -now.x, -now.y, -now.z });
    if (o.w < 0)
        o.w = -o.w, o.x = -o.x, o.y = -o.y, o.z = -o.z;
    return o;
}

static quat q_part(quat o, float w)   /* w of the way from no rotation to o (o.w >= 0) */
{
    float s = sqrtf(o.x * o.x + o.y * o.y + o.z * o.z), half, f;
    quat r;
    if (s < 1e-6f)
        return (quat){ 1, 0, 0, 0 };
    half = atan2f(s, o.w) * w;
    f = sinf(half) / s;
    r.w = cosf(half), r.x = o.x * f, r.y = o.y * f, r.z = o.z * f;
    return r;
}

static int wrap(int a)   /* a 12-bit angle, signed */
{
    return ((a & 0xfff) + 2048) % 4096 - 2048;
}

static void blend_joint(int16_t *a, quat off, float w)   /* w of the offset on top of the joint's angles */
{
    q_to_euler(q_mul(q_part(off, w), q_euler(a)), a);
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
    int i, j, yaw, dyaw, new_lower, new_upper;
    float wl, wu;
    quat turn;
    if (r->eax != G_PLAYER)
        return;
    id_body = GVAR(uint8_t, G_PLAYER + PL_ANIM_ID);
    id_arms = GVAR(uint8_t, G_PLAYER + 0x104);
    mode = GVAR(uint16_t, G_PLAYER + 0x106);
    upper = mode ? arms : body;
    yaw = (GVAR(uint16_t, G_PLAYER + 0x56) - GVAR(uint16_t, G_PLAYER + PL_MOVE_ANGLE)) & 0xfff;   /* (as the builder takes it) */
    if (!g_on || !g_have) {
        g_have = 1;
        k_lower = k_upper = BLEND_UPDATES;
        goto remember;
    }
    new_lower = id_body != last_body;
    new_upper = mode != last_mode || (mode ? id_arms != last_arms : id_body != last_body);
    /* what was shown, in the root's new frame: its yaw turned by dyaw since */
    dyaw = wrap(prev_yaw - yaw);
    turn = q_axis(1, dyaw);
    if (new_lower) {
        float c = cosf(dyaw * TURN_RAD), s = sinf(dyaw * TURN_RAD);
        off_root[0] = c * prev[0] + s * prev[2] - body[0];
        off_root[1] = (float)(prev[1] - body[1]);
        off_root[2] = -s * prev[0] + c * prev[2] - body[2];
        off_hip = q_offset(q_mul(turn, q_euler(prev_hip)), q_euler(body + 4));
        for (j = LEG_FROM; j < JOINTS; j++)
            off_leg[j] = q_offset(q_euler(prev + 4 + 4 * j), q_euler(body + 4 + 4 * j));
        k_lower = 0;
    }
    if (new_upper) {
        off_up[0] = q_offset(q_mul(turn, q_euler(prev + 4)), q_euler(upper + 4));
        for (j = 1; j < UPPER_JOINTS; j++)
            off_up[j] = q_offset(q_euler(prev + 4 + 4 * j), q_euler(upper + 4 + 4 * j));
        k_upper = 0;
    }
    /* (the first update of a change already moves a step toward the new pose) */
    if (k_lower < BLEND_UPDATES)
        k_lower++;
    if (k_upper < BLEND_UPDATES)
        k_upper++;
    wl = fade(k_lower);
    wu = fade(k_upper);
    if (wl > 0) {
        for (i = 0; i < 3; i++)
            body[i] = (int16_t)(body[i] + (int)lroundf(off_root[i] * wl));
        if (mode)   /* (with no shot out joint 0 is the upper body's too, blended below) */
            blend_joint(body + 4, off_hip, wl);
        for (j = LEG_FROM; j < JOINTS; j++)
            blend_joint(body + 4 + 4 * j, off_leg[j], wl);
    }
    if (wu > 0)
        for (j = 0; j < UPPER_JOINTS; j++)
            blend_joint(upper + 4 + 4 * j, off_up[j], wu);
    if (g_log)
        log_printf("blend: st %d body %02X arms %02X mode %d k %d/%d turn %5d yaw %03X | j0 %4d %4d %4d j1 %4d %4d %4d",
                   GVAR(uint8_t, G_PLAYER + PL_STATE), id_body, id_arms, mode, k_lower, k_upper,
                   GVAR(int16_t, G_PLAYER + PL_MOVE_ANGLE), yaw,
                   upper[4], upper[5], upper[6], upper[8], upper[9], upper[10]);
remember:
    for (i = 0; i < POSE_N; i++)
        prev[i] = i >= UPPER_FROM && i < UPPER_TO ? upper[i] : body[i];
    memcpy(prev_hip, body + 4, sizeof prev_hip);
    prev_yaw = yaw;
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
