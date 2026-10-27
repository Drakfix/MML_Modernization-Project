/* Z-targeting: which enemy is locked on, and the reticle on it.
 *
 * Every frame target_and_hits_update (0x4E0405) collects the hit volumes of
 * everything in play into a table at 0x750DAC (12 bytes each: x, y, z,
 * flags, owner) and target_pick_nearest returns the nearest enemy, which the
 * game stores in 0x5505E8. We hook the store: while a lock is wanted we keep
 * our own choice instead (the locked enemy as long as it's in the table,
 * the nearest one when it's gone, the next one left/right on request) and
 * hand that to the game, so its own aiming agrees with ours. A first lock
 * only considers enemies well inside the view (not behind Mega Man or at a
 * corner of the screen), the nearest of those; switching cycles through the
 * ones on screen.
 *
 * Table entry (from target_pick_nearest): s16 x, y, z; u16 flags (bits
 * 13-15 = side class, tested against the player's side mask 0x5505E2; byte
 * +7 & 6 = this volume and the next form a pair, use their midpoint); u32
 * owner: low 23 bits -> a record whose +4 bit 0x80 = disabled and bit 0 =
 * targetable by the player, top byte = object id (< 0x80), which
 * FUN_004E1877 turns into the object pointer.
 *
 * The reticle is drawn like the orbit toast: translucent flat triangles in
 * the game's HUD list, at the enemy projected through the current camera. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "display.h"
#include "game.h"
#include "hook.h"
#include "lockon.h"
#include "log.h"
#include "mml.h"

#define G_SITE_TARGET_STORE  0x4E041Du   /* mov [0x5505E8],eax right after target_pick_nearest */
#define G_FN_OBJ_FROM_ID     0x4E1877u   /* object* FUN_004E1877(EAX = id) */
#define G_VOLUMES            0x750DACu
#define G_VOLUME_COUNT       0x77E94Cu
#define G_PLAYER_SIDE_MASK   0x5505E2u
#define G_CAM_POS            0x77E9E8u   /* s32 x, y, z: where cam_apply_angles put the camera */
#define G_CAM_PITCH_ANGLE    0x77EA08u
#define G_CAM_YAW_ANGLE      0x77EA0Au
#define G_GTE_H              0x77EBDCu
#define G_GTE_OFX            0x77EBCCu
#define G_GTE_OFY            0x77EBD0u
#define RANGE                2000
#define MAX_CAND             64
#define PI 3.14159265358979323846

typedef struct { int id; int x, y, z; int seen, central; } cand_t;

static cand_t g_cand[MAX_CAND];
static int g_ncand;
static int g_lock_id = -1, g_had;
static int g_lx, g_ly, g_lz;
static volatile LONG g_want, g_switch;
static int g_test_reticle;

static int16_t PS16(uint32_t o) { return *(volatile int16_t *)(uintptr_t)(G_PLAYER + o); }

typedef uint32_t(__cdecl *obj_from_id_fn)(uint32_t id);
static __declspec(naked) void obj_from_id_thunk(void)   /* called as obj_from_id_fn */
{
    __asm {
        push ebx
        push ecx
        push edx
        push esi
        push edi
        push ebp
        mov eax, [esp + 28]
        mov esi, G_FN_OBJ_FROM_ID
        call esi
        pop ebp
        pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        ret
    }
}

static void collect(void)
{
    uint32_t n = *(volatile uint32_t *)(uintptr_t)G_VOLUME_COUNT, i;
    uint8_t side = *(volatile uint8_t *)(uintptr_t)G_PLAYER_SIDE_MASK;
    int px = PS16(0x14), py = PS16(0x16), pz = PS16(0x18);
    g_ncand = 0;
    if (n > 512)
        return;
    for (i = 0; i < n && g_ncand < MAX_CAND; i++) {
        const uint8_t *e = (const uint8_t *)(uintptr_t)(G_VOLUMES + 12 * i);
        uint16_t fl = *(const uint16_t *)(e + 6);
        uint32_t own = *(const uint32_t *)(e + 8), rec = own & 0x7fffff, rflags;
        int id = (int)(own >> 24), x = *(const int16_t *)e, y = *(const int16_t *)(e + 2), z = *(const int16_t *)(e + 4);
        int k, dx, dy, dz;
        if (e[7] & 6) {   /* paired volume: midpoint, and skip the partner */
            const uint8_t *f = e + 12;
            if (i + 1 >= n)
                break;
            x = (x + *(const int16_t *)f) / 2;
            y = (y + *(const int16_t *)(f + 2)) / 2;
            z = (z + *(const int16_t *)(f + 4)) / 2;
            i++;
        }
        if (!(((unsigned)side << (fl >> 13)) & 0x80) || id >= 0x80 || rec < 0x400000)
            continue;
        rflags = *(const uint32_t *)(uintptr_t)(rec + 4);
        if ((rflags & 0x80) || !(rflags & 1))
            continue;
        dx = x - px; dy = y - py; dz = z - pz;
        if (dx < -RANGE || dx > RANGE || dy < -RANGE || dy > RANGE || dz < -RANGE || dz > RANGE ||
            dx * dx + dy * dy + dz * dz >= RANGE * RANGE)
            continue;
        for (k = 0; k < g_ncand && g_cand[k].id != id; k++)
            ;
        if (k < g_ncand)
            continue;   /* one entry per enemy (bodies have several volumes) */
        g_cand[g_ncand].id = id;
        g_cand[g_ncand].x = x;
        g_cand[g_ncand].y = y;
        g_cand[g_ncand].z = z;
        g_ncand++;
    }
}

static int project(double wx, double wy, double wz, double *sx, double *sy);

/* where each candidate is on screen: seen = anywhere in view, central = well
 * inside it (not hugging an edge or a corner) */
static void classify(void)
{
    /* the projector's space is 320 wide around OFX; native widescreen shows
     * more of it on both sides (display.c shifts the picture into the wider
     * surface) */
    double cx = *(volatile int32_t *)(uintptr_t)G_GTE_OFX, h = 2.0 * *(volatile int32_t *)(uintptr_t)G_GTE_OFY;
    double w = display_native_width() > 640 ? display_native_width() / 2.0 : 2.0 * cx, x0 = cx - w / 2;
    int k;
    for (k = 0; k < g_ncand; k++) {
        double sx, sy;
        g_cand[k].seen = g_cand[k].central = 0;
        if (!project(g_cand[k].x, g_cand[k].y, g_cand[k].z, &sx, &sy))
            continue;
        sx -= x0;
        g_cand[k].seen = sx >= 0 && sx <= w && sy >= 0 && sy <= h;
        g_cand[k].central = sx >= w * 0.12 && sx <= w * 0.88 && sy >= h * 0.08 && sy <= h * 0.92;
    }
}

/* the first lock: the nearest enemy that's well inside the view (one behind
 * Mega Man or peeking in at a corner isn't what you meant); none = free aim */
static int nearest(void)
{
    int px = PS16(0x14), pz = PS16(0x18), k, best = -1;
    double bd = 1e30;
    for (k = 0; k < g_ncand; k++) {
        double dx = g_cand[k].x - px, dz = g_cand[k].z - pz, d = dx * dx + dz * dz;
        if (!g_cand[k].central)
            continue;
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

/* the next enemy clockwise (dir +1, to the right on screen) or counter-
 * clockwise from the current one, as seen from Mega Man, among those in
 * view; no wrap-around */
static int next_to(int cur, int dir)
{
    int px = PS16(0x14), pz = PS16(0x18), k, best = -1;
    double a0 = atan2((double)g_cand[cur].x - px, (double)g_cand[cur].z - pz), bd = 1e30;
    for (k = 0; k < g_ncand; k++) {
        double d;
        if (k == cur || !g_cand[k].seen)
            continue;
        d = atan2((double)g_cand[k].x - px, (double)g_cand[k].z - pz) - a0;
        while (d <= -PI) d += 2 * PI;
        while (d > PI) d -= 2 * PI;
        d *= dir;
        if (d > 0 && d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

static void on_target_store(regs_t *r)
{
    int k = -1, sw;
    if (!g_want) {
        g_lock_id = -1;
        g_had = 0;
        g_switch = 0;
        return;
    }
    collect();
    classify();
    if (g_lock_id >= 0)
        for (k = 0; k < g_ncand && g_cand[k].id != g_lock_id; k++)
            ;
    if (k < 0 || k >= g_ncand)
        k = nearest();   /* first lock, or the locked enemy is gone: the nearest */
    sw = (int)InterlockedExchange(&g_switch, 0);
    if (k >= 0 && sw) {
        int j = next_to(k, sw > 0 ? 1 : -1);
        if (j >= 0)
            k = j;
    }
    if (k < 0) {
        g_lock_id = -1;
        return;
    }
    if (g_cand[k].id != g_lock_id)
        log_printf("lock-on: enemy %02X (%d candidates)", g_cand[k].id, g_ncand);
    g_lock_id = g_cand[k].id;
    g_had = 1;
    g_lx = g_cand[k].x;
    g_ly = g_cand[k].y;
    g_lz = g_cand[k].z;
    r->eax = ((obj_from_id_fn)(void *)obj_from_id_thunk)((uint32_t)g_lock_id);
}

void lockon_want(int want)
{
    g_want = want;
}

void lockon_switch(int dir)
{
    g_switch = dir;
}

int lockon_locked(void)
{
    return g_want && g_lock_id >= 0;
}

int lockon_had(void)
{
    return g_want && g_had;
}

void lockon_pos(int *x, int *y, int *z)
{
    *x = g_lx;
    *y = g_ly;
    *z = g_lz;
}

/* ---- reticle ---- */

#define RET_TRIS     4
#define RET_IN       10.0    /* tip distance from the centre, 320-space px */
#define RET_OUT      20.0
#define RET_HALF     5.5     /* half width of a triangle's base */
#define RET_EDGE     1.6     /* dark outline thickness */
#define RET_RIM      0       /* dark outline under the fill */
#define RET_ALPHA    0x8C    /* see-through amount (renderer side, 0xFF = solid) */

static __declspec(align(8)) uint8_t g_tri[2 * RET_TRIS][0x40];
static unsigned g_spin;
static int g_ret_rgb[3] = { 0xFF, 0xD8, 0x40 };   /* warm yellow; [controls] reticle_color */

static void put16(uint8_t *p, int off, double v) { *(int16_t *)(p + off) = (int16_t)floor(v + 0.5); }

static void prepend_hud(uint8_t *pkt)
{
    uint8_t *ctx = *(uint8_t **)G_GFX_CUR_CTX;
    uint32_t *bucket = (uint32_t *)(ctx + 0x78);
    *(uint32_t *)pkt = *bucket;
    *bucket = (uint32_t)(uintptr_t)pkt;
}

/* world point -> 320x240 game screen, the way cam_apply_angles framed it:
 * forward = (sin yaw cos p, sin p, cos yaw cos p) with p = -pitch */
static int project(double wx, double wy, double wz, double *sx, double *sy)
{
    double yaw = (*(volatile uint16_t *)(uintptr_t)G_CAM_YAW_ANGLE & 0xfff) * (PI / 2048.0);
    int pr = *(volatile uint16_t *)(uintptr_t)G_CAM_PITCH_ANGLE & 0xfff;
    double p = -(pr > 2048 ? pr - 4096 : pr) * (PI / 2048.0);
    double fx = sin(yaw) * cos(p), fy = sin(p), fz = cos(yaw) * cos(p);
    double rx = cos(yaw), rz = -sin(yaw);                       /* right, horizontal */
    double ux = fy * rz, uy = fz * rx - fx * rz, uz = -fy * rx;  /* f x r: screen down */
    double dx = wx - *(volatile int32_t *)(uintptr_t)G_CAM_POS;
    double dy = wy - *(volatile int32_t *)(uintptr_t)(G_CAM_POS + 4);
    double dz = wz - *(volatile int32_t *)(uintptr_t)(G_CAM_POS + 8);
    double cx = dx * rx + dz * rz, cy = dx * ux + dy * uy + dz * uz, cz = dx * fx + dy * fy + dz * fz;
    if (cz < 32.0)
        return 0;
    *sx = *(volatile int32_t *)(uintptr_t)G_GTE_OFX + mml_h_x() * cx / cz;
    *sy = *(volatile int32_t *)(uintptr_t)G_GTE_OFY + *(volatile float *)(uintptr_t)G_GTE_H * cy / cz;
    return 1;
}

int lockon_owns(const void *pkt)
{
    const uint8_t *p = (const uint8_t *)pkt;
    return p >= g_tri[0] && p < g_tri[0] + sizeof g_tri;
}

/* one inward-pointing triangle of the reticle; outline = the same shape
 * grown by RET_EDGE in dark red */
static void reticle_tri(uint8_t *t, double sx, double sy, double a, double pulse, int outline)
{
    static const uint8_t w01[4] = { 0xCD, 0xCC, 0xCC, 0x3D };   /* 0.1f */
    double ca = cos(a), sa = sin(a), e = outline ? RET_EDGE : 0.0;
    double tip = RET_IN * pulse - e, base = RET_OUT * pulse + e, half = RET_HALF + e * 1.6;
    double ax = sx + ca * tip, ay = sy + sa * tip, bx = sx + ca * base, by = sy + sa * base;
    double b1x = bx - sa * half, b1y = by + ca * half, b2x = bx + sa * half, b2y = by - ca * half;
    int j;
    if ((b1x - ax) * (b2y - ay) - (b1y - ay) * (b2x - ax) < 0) {   /* clockwise, like the toast's quad */
        double tx = b1x, ty = b1y;
        b1x = b2x; b1y = b2y; b2x = tx; b2y = ty;
    }
    t[4] = (uint8_t)(outline ? g_ret_rgb[0] / 4 : g_ret_rgb[0]);   /* the renderer makes it see-through */
    t[5] = (uint8_t)(outline ? g_ret_rgb[1] / 4 : g_ret_rgb[1]);
    t[6] = (uint8_t)(outline ? g_ret_rgb[2] / 4 : g_ret_rgb[2]);
    t[7] = 0x28;   /* flat opaque quad (0x2a would blend with whatever PS1 mode was set last), strip order */
    put16(t, 0x08, ax);  put16(t, 0x0a, ay);
    put16(t, 0x0c, b1x); put16(t, 0x0e, b1y);
    put16(t, 0x10, b2x); put16(t, 0x12, b2y);
    put16(t, 0x14, b2x); put16(t, 0x16, b2y);
    for (j = 0; j < 4; j++)
        memcpy(t + 0x18 + 4 * j, w01, 4);
    prepend_hud(t);
}

void lockon_frame(void)
{
    double sx, sy, spin, pulse;
    int i, j;
    g_spin++;
    if (!*(void **)G_GFX_CUR_CTX)
        return;
    if (g_test_reticle) {   /* dev: on Mega Man's chest, to check the projection */
        if (!project(PS16(0x14), PS16(0x16) - 100, PS16(0x18), &sx, &sy))
            return;
    } else if (!lockon_locked() || !project(g_lx, g_ly, g_lz, &sx, &sy)) {
        return;
    }
    spin = g_spin * (PI / 90.0);
    pulse = 1.0 + 0.12 * sin(g_spin * 0.25);
    /* fill first, outline last: the list is walked from the head, so the
     * outlines (prepended last) are drawn first, under the fill */
    for (j = 0; j < (RET_RIM ? 2 : 1); j++)
        for (i = 0; i < RET_TRIS; i++)
            reticle_tri(g_tri[j * RET_TRIS + i], sx, sy, spin + i * (PI / 2), pulse, j);
}

void lockon_set_color(unsigned rgb)
{
    g_ret_rgb[0] = (rgb >> 16) & 0xFF;
    g_ret_rgb[1] = (rgb >> 8) & 0xFF;
    g_ret_rgb[2] = rgb & 0xFF;
    log_printf("lock-on: reticle colour %06X", rgb & 0xFFFFFF);
}

unsigned lockon_alpha(void)
{
    return RET_ALPHA;
}

void lockon_test_reticle(int on)
{
    g_test_reticle = on;
}

int lockon_install(void)
{
    static const uint8_t store[] = { 0xA3, 0xE8, 0x05, 0x55, 0x00 };
    if (!hook_mid(G_SITE_TARGET_STORE, sizeof store, store, on_target_store)) {
        log_printf("lock-on: target hook failed");
        return 0;
    }
    return 1;
}
