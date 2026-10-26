/* Smooth background layers (the sky), for our renderer.
 *
 * The game's background-layer system (FUN_004E7ABD) keeps each layer's
 * scroll in 16.16 fixed point (layer +4 = X, +8 = Y, wrapped to the
 * panorama: +0x22 / +0x24 tiles of 32 px), but draws it as a grid of 32x32
 * sprite tiles at whole-pixel positions -(scroll % 32) + 32k, from the
 * integer scroll it stores at +0xC / +0xE. So the sky moved in visible
 * whole-pixel steps, once per game tick.
 *
 * Here: right before a layer's tiles are generated, its exact scroll is
 * recorded and its integer scroll is stepped back one tile (mod the panorama
 * size), and the loop bounds are raised by two tiles, so the grid gets a
 * spare tile on every side. When the tiles are drawn, ddshim asks
 * bglayer_offsets() for the packet: the tiles are moved back by one tile
 * minus the fraction the game dropped (sub-pixel accurate), and for frame
 * interpolation it also gets where they were one tick ago from the real
 * scroll change, which stays correct when the grid wraps around. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "bglayer.h"
#include "hook.h"
#include "log.h"

#define SITE_LAYER_TILES  0x4E7C74u   /* xor edx,edx; mov dl,[ebp+0x31]: layer in EBP, about to emit tiles */
#define SITE_LAYER_LINK   0x4E7DD4u   /* call gfx_add_prim_alt: EDX first packet, EBX last, EBP layer */
#define SITE_COLS_BOUND   0x4E7D3Cu   /* cmp esi,0x140 */
#define SITE_ROWS_BOUND   0x4E7D14u   /* cmp dword [esp+0x18],0xF0 */
#define LAYER_POOL        0x56F80Cu   /* layers, 0x38 bytes each */
#define LAYER_SIZE        0x38
#define MAX_LAYERS        64

typedef struct {
    const uint8_t *first, *last;   /* this frame's tile packets */
    int slot;
    double x, y;                   /* exact scroll, 320x240 pixels */
    int xi, yi;                    /* the whole-pixel scroll the game drew the tiles with */
    double w, h;                   /* panorama size in pixels */
} layer_rec;

/* three sets: being built, being drawn, the one before that */
static layer_rec g_recs[3][MAX_LAYERS];
static int g_n[3], g_build, g_draw = -1, g_prev = -1;
static uint8_t *g_open;            /* layer whose tiles are being generated */
static int16_t g_saved_x, g_saved_y;
static double g_lost_x;            /* scroll the game's integer math dropped, for g_open */

/* The game computes a camera-following layer's scroll as
 *   X = (((speed * counter) >> 8) + (parallax / w^2 + yaw + base) * 8 & 0x7FFF) * w * 64
 * (FUN_004E7ABD; counter 0x77EA70, parallax accumulator 0x77EA78 grows with how
 * far the camera moved). Both divisions throw away their remainder, so while
 * walking the sky advanced in 0.625 px jumps with a stall every few ticks, and
 * the slow cloud drift went 1,1,1,0. Put the remainders back. */
#define G_LAYER_COUNTER  0x77EA70u
#define G_LAYER_PARALLAX 0x77EA78u

static double lost_scroll(const uint8_t *layer, int w)
{
    int32_t speed = *(const int32_t *)(layer + 0x1E) >> 16;
    uint32_t drift = (uint32_t)(speed * *(const int32_t *)G_LAYER_COUNTER);
    int32_t par = *(const int32_t *)G_LAYER_PARALLAX, w2 = w * w;
    double units8;
    if (layer[1] != 0 || w2 <= 0)
        return 0.0;   /* self-scrolling layers keep their own exact position */
    units8 = (double)(drift & 0xFF) / 256.0 + 8.0 * ((double)par / w2 - (double)(par / w2));
    return units8 * w * 64.0 / 65536.0;   /* 320x240 pixels */
}

static void on_layer_tiles(regs_t *r)
{
    uint8_t *layer = (uint8_t *)(uintptr_t)r->ebp;
    int w = *(int32_t *)(layer + 0x22) >> 16, h = *(int32_t *)(layer + 0x24) >> 16;
    int16_t *xi = (int16_t *)(layer + 0xC), *yi = (int16_t *)(layer + 0xE);
    g_open = NULL;
    if (w <= 0 || h <= 0)
        return;
    g_open = layer;
    g_saved_x = *xi;
    g_saved_y = *yi;
    g_lost_x = lost_scroll(layer, w);
    /* one tile back, wrapped so the tile index the loop derives stays in range */
    *xi = (int16_t)(((int)*xi - 32 + w * 32) % (w * 32));
    *yi = (int16_t)(((int)*yi - 32 + h * 32) % (h * 32));
}

static void on_layer_link(regs_t *r)
{
    uint8_t *layer = (uint8_t *)(uintptr_t)r->ebp;
    layer_rec *e;
    if (layer != g_open)
        return;
    g_open = NULL;
    *(int16_t *)(layer + 0xC) = g_saved_x;
    *(int16_t *)(layer + 0xE) = g_saved_y;
    if (g_n[g_build] >= MAX_LAYERS)
        return;
    e = &g_recs[g_build][g_n[g_build]++];
    e->first = (const uint8_t *)(uintptr_t)r->edx;
    e->last = (const uint8_t *)(uintptr_t)r->ebx;
    e->slot = (int)(((uintptr_t)layer - LAYER_POOL) / LAYER_SIZE);
    e->x = (double)*(int32_t *)(layer + 4) / 65536.0 + g_lost_x;
    e->xi = g_saved_x;
    e->yi = g_saved_y;
    e->y = (double)*(int32_t *)(layer + 8) / 65536.0;
    e->w = (double)((*(int32_t *)(layer + 0x22) >> 16) * 32);
    e->h = (double)((*(int32_t *)(layer + 0x24) >> 16) * 32);
}

int g_bglayer_log;   /* debug: log each layer's exact scroll every frame */

void bglayer_frame_end(void)
{
    if (g_bglayer_log) {
        int i;
        for (i = 0; i < g_n[g_build] && i < 2; i++)
        {
            const uint8_t *L = (const uint8_t *)(LAYER_POOL + g_recs[g_build][i].slot * LAYER_SIZE);
            log_printf("bglayer: slot %d x %.4f yaw %d ea78 %d ea70 %d speed %d base %d w %d auto %d",
                       g_recs[g_build][i].slot, g_recs[g_build][i].x, (int)*(int16_t *)0x77EA0Au,
                       *(int32_t *)0x77EA78u, *(int32_t *)0x77EA70u, *(int32_t *)(L + 0x1e) >> 16,
                       *(int32_t *)(L + 0x1a) >> 16, *(int32_t *)(L + 0x22) >> 16, L[1]);
        }
    }
    int old_prev = g_prev, next;
    g_prev = g_draw;
    g_draw = g_build;
    if (old_prev >= 0) {
        next = old_prev;
    } else {
        for (next = 0; next == g_draw || next == g_prev; next++)
            ;
    }
    g_build = next;
    g_n[g_build] = 0;
}

static double wrap(double d, double period)
{
    while (d > period / 2) d -= period;
    while (d < -period / 2) d += period;
    return d;
}

int bglayer_offsets(const void *pkt, float *ox, float *oy, float *px, float *py)
{
    const layer_rec *e = NULL;
    const uint8_t *q = (const uint8_t *)pkt;
    double dx, dy;
    int i;
    if (g_draw < 0 || !q)
        return 0;
    for (i = 0; i < g_n[g_draw]; i++)
        if (q >= g_recs[g_draw][i].first && q <= g_recs[g_draw][i].last) {
            e = &g_recs[g_draw][i];
            break;
        }
    if (!e)
        return 0;
    /* the tiles came out one tile too far right (the spare column/row) and
     * without the fraction of a pixel the integer scroll dropped. Projector
     * space is 320x240, the vertices are in 640x480: x2. */
    *ox = (float)(2.0 * (-32.0 - wrap(e->x - e->xi, e->w)));
    *oy = (float)(2.0 * (-32.0 - wrap(e->y - e->yi, e->h)));
    if (g_prev < 0)
        return 1;
    for (i = 0; i < g_n[g_prev]; i++)
        if (g_recs[g_prev][i].slot == e->slot)
            break;
    if (i == g_n[g_prev])
        return 1;
    /* scrolling forward moves the tiles back: they were d further along */
    dx = wrap(e->x - g_recs[g_prev][i].x, e->w);
    dy = wrap(e->y - g_recs[g_prev][i].y, e->h);
    if (dx < -64.0 || dx > 64.0 || dy < -64.0 || dy > 64.0)
        return 1;   /* a jump (new area, camera cut): don't blend */
    *px = (float)(2.0 * dx);
    *py = (float)(2.0 * dy);
    return 2;
}

int bglayer_install(void)
{
    static const uint8_t tiles_bytes[] = { 0x31, 0xD2, 0x8A, 0x55, 0x31 };
    static const uint8_t link_bytes[] = { 0xE8, 0xD6, 0x09, 0x01, 0x00 };   /* call 0x4F87AF */
    static const uint8_t cols_bytes[] = { 0x81, 0xFE, 0x40, 0x01, 0x00, 0x00 };
    static const uint8_t rows_bytes[] = { 0x81, 0x7C, 0x24, 0x18, 0xF0, 0x00, 0x00, 0x00 };
    static const uint32_t cols = 0x140 + 64, rows = 0xF0 + 64;
    if (!mem_expect(SITE_COLS_BOUND, cols_bytes, sizeof cols_bytes) ||
        !mem_expect(SITE_ROWS_BOUND, rows_bytes, sizeof rows_bytes)) {
        log_printf("bglayer: tile loop unexpected, sky left alone");
        return 0;
    }
    if (!hook_mid(SITE_LAYER_TILES, sizeof tiles_bytes, tiles_bytes, on_layer_tiles) ||
        !hook_mid(SITE_LAYER_LINK, sizeof link_bytes, link_bytes, on_layer_link)) {
        log_printf("bglayer: hooks failed, sky left alone");
        return 0;
    }
    mem_write(SITE_COLS_BOUND + 2, &cols, 4);
    mem_write(SITE_ROWS_BOUND + 4, &rows, 4);
    log_printf("bglayer: smooth background layers ON");
    return 1;
}
