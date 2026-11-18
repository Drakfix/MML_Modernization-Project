/* Close actors stay in the shot (cutscene close-ups, the camera right behind someone).
 *
 * Before the game draws an actor or a prop it projects the model's origin (one RTPS) and skips the
 * whole model when that point lands outside a screen window: x + 352 and y + 136 must both be within
 * 0..0x400 in the game's 320x240 space (FUN_004EAF76, 004EB5D4, 004EC000, 004EE1A3; widecull.c moves
 * the x window out past 16:9).
 *
 * The PC game's software GTE (FUN_004EEED0) saturates the perspective divide the way the PS1's GTE
 * does: a point nearer than the near distance projects as if it were at that distance. On the PS1
 * the quotient H / SZ stops at 2.0, so that distance is H / 2, and the vanilla PC game keeps
 * near = H * 0.5 (500 for the default H of 1000). An origin close to the camera therefore never
 * lands far off-screen there, and the model stays drawn and cut by the near plane.
 *
 * The exe patch from RE-notes 3.35 made near H / 32, so the polygon clipper (gfx_near_clip_poly) cuts
 * walls and floors next to the camera instead of dropping them. It also took the saturation out of
 * this test: an origin closer than H / 2 is now projected at its real depth, and with the camera at
 * someone's shoulder their origin lands far below or beside the screen. Measured in the Eden control
 * room cutscene (H 330): the girl's origin 63 units in front of the camera and 182 below its axis
 * projects to y 1074 (the window ends at 888), so the whole model was skipped; with the PS1's
 * saturation it lands at y 484 and she fills the shot as on the PS1. Any aspect, backend or frame
 * rate: the cause is the exe patch, not the widescreen view.
 *
 * Fix: right after the RTPS of one of those four tests (the caller's return address says which), the
 * screen point is projected again with the vanilla H / 2 saturation. Only the visibility test reads
 * it (each function copies it into a local that nothing else uses); the model's own vertices keep
 * the exact projection and the H / 32 clipping. Origins at H / 2 or further are untouched. */
#include <windows.h>
#include <stdint.h>

#include "actorcull.h"
#include "addr.h"
#include "gte_capture.h"
#include "guest.h"
#include "hook.h"
#include "log.h"
#include "mml.h"



#define G_GTE_CAM   0x77EB28u   /* float[3]: camera-space position of the vertex just projected */
#define G_GTE_SXY2  0x77EB98u   /* float SX, SY of the vertex just projected */
#define G_GTE_SZ    0x77EBA0u   /* s32 its depth (0 behind the camera); the tests use it >> 2 */
#define G_FAR_DEPTH 0x77EAB2u   /* u16 the tests' far limit */
#define G_DEPTH_SHIFT 0x77EABFu /* u8 the room's scale shift: the near limit is 0x30 >> it */
#define G_GTE_NEAR  0x77EBC0u   /* float near distance (H / 32 with our exe patch, H / 2 in vanilla) */
#define G_GTE_OFX   0x77EBCCu   /* s32 screen centre */
#define G_GTE_OFY   0x77EBD0u
#define G_GTE_H     0x77EBDCu   /* float H (the y projection; x uses mml_h_x) */
#define G_FN_RTPS   GA(0x4EFDC9u)
#define PS1_SATURATION 0.5f     /* the GTE's H / SZ limit of 2.0: points nearer than H / 2 */

/* the four visibility tests: the return address of their RTPS call and their window constants */
static const struct { uint32_t ret, fadd_x, fadd_y; } k_tests[] = {
    { 0x4EAFA2u, 0x50A4B0u, 0x50A4B4u },   /* FUN_004EAF76 */
    { 0x4EB600u, 0x50A4B8u, 0x50A4BCu },   /* FUN_004EB5D4 */
    { 0x4EC02Fu, 0x50A4C8u, 0x50A4CCu },   /* FUN_004EC000 */
    { 0x4EE243u, 0x50A4DCu, 0x50A4E0u },   /* FUN_004EE1A3 */
};
#define NTESTS (sizeof k_tests / sizeof k_tests[0])

static uint32_t g_ret[NTESTS];   /* in the running program */
static int g_on;
static int g_log;                /* MML_ACTORCULL_LOG=1: log the models this keeps */
static unsigned g_logged;

/* the game's test: truncate (FUN_004F915C chops), then a 16-bit unsigned compare with 0x400 */
static int in_window(float v, float add)
{
    return (uint16_t)(int32_t)(v + add) <= 0x400;
}

void actorcull_after_rtps(uint32_t ret)
{
    unsigned i;
    const float *c;
    float h, sat, z, sx, sy;
    if (!g_on)
        return;
    for (i = 0; i < NTESTS && g_ret[i] != ret; i++)
        ;
    if (i == NTESTS)
        return;
    c = (const float *)GUEST(G_GTE_CAM);
    h = GVAR(float, G_GTE_H);
    sat = h * PS1_SATURATION;
    z = c[2];
    if (z >= sat || GVAR(float, G_GTE_NEAR) >= sat)
        return;   /* projected at its real depth on the PS1 as well (or near is already the vanilla one) */
    /* as FUN_004EEED0 does for a point nearer than near, with the PS1's distance */
    sx = (float)GVAR(int32_t, G_GTE_OFX) + c[0] * mml_h_x() / sat;
    sy = (float)GVAR(int32_t, G_GTE_OFY) + c[1] * h / sat;
    if (g_log && g_logged < 200) {
        float ox = GVAR(float, G_GTE_SXY2), oy = GVAR(float, G_GTE_SXY2 + 4);
        float ax = GVAR(float, GA(k_tests[i].fadd_x)), ay = GVAR(float, GA(k_tests[i].fadd_y));
        int was = in_window(ox, ax) && in_window(oy, ay), now = in_window(sx, ax) && in_window(sy, ay);
        /* the first three run the depth tests before the window: too far, or nearer than 0x30 >> shift */
        int depth = GVAR(int32_t, G_GTE_SZ) >> 2, shift = GVAR(uint8_t, G_DEPTH_SHIFT) & 31;
        int depth_ok = i == 3 || (depth <= GVAR(uint16_t, G_FAR_DEPTH) && depth >= (0x30 >> shift));
        if (was != now && depth_ok) {
            g_logged++;
            log_printf("actorcull: test %u kept a model: origin %.0f %.0f %.0f (depth %d, shift %d, near %.1f, H %.1f), "
                       "screen %.0f %.0f -> %.0f %.0f", i, (double)c[0], (double)c[1], (double)z, depth, shift,
                       (double)GVAR(float, G_GTE_NEAR), (double)h, (double)ox, (double)oy, (double)sx, (double)sy);
        }
    }
    GVAR(float, G_GTE_SXY2) = sx;
    GVAR(float, G_GTE_SXY2 + 4) = sy;
}

int actorcull_install(void)
{
    unsigned i;
    char v[8] = "";
    for (i = 0; i < NTESTS; i++) {
        uint32_t ret = GA(k_tests[i].ret), call = ret - 5;
        if (!ret || GVAR(uint8_t, call) != 0xE8 ||
            call + 5 + GVAR(int32_t, call + 1) != G_FN_RTPS) {
            log_printf("actorcull: visibility test %u unexpected, close actors left to the game", i);
            return 0;
        }
        g_ret[i] = ret;
    }
    if (!gte_rtps_hook()) {
        log_printf("actorcull: RTPS hook failed, close actors left to the game");
        return 0;
    }
    GetEnvironmentVariableA("MML_ACTORCULL_LOG", v, sizeof v);
    g_log = v[0] == '1';
    g_on = 1;
    log_printf("actorcull: close actors kept in view (model origins nearer than H / 2 projected as on the PS1)");
    return 1;
}
