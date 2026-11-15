#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "guest.h"
#include "hook.h"
#include "log.h"
#include "packets.h"

#define G_SITE_WALK_DISPATCH  GA(0x4F7923u)   /* jmp [ecx*4 + 0x4F7624]; packet at [esp+0x64] */
#define G_SITE_WALK_LINK      GA(0x4F7ABCu)   /* following a link node (bit 0 set) in [esp+0x64] */
#define G_SITE_HUD_EMIT_LINK  GA(0x4365DAu)   /* hud_emit_sprite, right before it links EDX = packet into ctx+0x78 */
#define G_SITE_MAP_START      GA(0x436CFDu)   /* minimap (FUN_00436BA0): call to the tile drawer */
#define G_SITE_MAP_END        GA(0x436D0Bu)   /* minimap exit, after the panel */
#define G_GFX_ARENA_CURSOR    0x77E6DCu       /* packet bump allocator */
#define G_SITE_BOSS_START     GA(0x435C73u)   /* HUD draw: body of the boss slot loop */
#define G_SITE_BOSS_END       GA(0x435CB5u)   /* after it */
#define G_SITE_GAUGE2_START   GA(0x435CC2u)   /* call setup of hud_draw_gauge_4363b5 (Roll on the boat) */
#define G_SITE_GAUGE2_END     GA(0x435CCCu)   /* the jump after it */
#define G_SITE_DEFERRED_DRAW  GA(0x4F7AE8u)   /* call 0x4F44C7 for each deferred packet (EAX), after the walk */
#define WALK_PKT_SLOT         0x64
#define RECENT                1024
#define BUCKET_WORLD_DEFAULT  4
#define HUD_TAGS              256
#define HUD_ROW_MIN_Y         100         /* 320 space; the boss gauge row (y=16) stays centred */

volatile const uint8_t *g_cur_pkt;
volatile int g_cur_bucket = 7;

/* esp as it was at the hooked instruction (the stub pushed eflags first) */
static uint32_t site_esp(const regs_t *r) { return r->esp + 4; }

/* one-shot walk trace for debugging bucket tracking ([debug] census=1) */
#define TRACE_FRAME 900
#define TRACE_MAX 400
static uint32_t g_trace[TRACE_MAX][3];   /* kind (1 link, 2 dispatch), address, bucket/op */
static int g_trace_n;
static unsigned g_frame;
static int g_census;

static void trace(uint32_t kind, uint32_t a, uint32_t b)
{
    if (g_census && g_frame == TRACE_FRAME && g_trace_n < TRACE_MAX) {
        g_trace[g_trace_n][0] = kind;
        g_trace[g_trace_n][1] = a;
        g_trace[g_trace_n][2] = b;
        g_trace_n++;
    }
}

/* Recently dispatched packets and their buckets: some handlers defer a
 * packet to a list that's drawn after the whole walk (0x77EBE4), and those
 * draws need the bucket the packet was really in. */
static const void *g_recent_pkt[RECENT];
static uint8_t g_recent_bucket[RECENT];
static unsigned g_recent_next;

static void on_dispatch(regs_t *r)
{
    g_cur_pkt = (const uint8_t *)GUEST(GVAR(uint32_t, site_esp(r) + WALK_PKT_SLOT));
    g_recent_pkt[g_recent_next % RECENT] = (const void *)g_cur_pkt;
    g_recent_bucket[g_recent_next % RECENT] = (uint8_t)g_cur_bucket;
    g_recent_next++;
    if ((g_cur_pkt[7] & 0xFC) == 0x2C)
        trace(2, GADDR(g_cur_pkt), g_cur_pkt[7] | ((uint32_t)g_cur_bucket << 8));
}

static void on_link(regs_t *r)
{
    uint32_t node = GVAR(uint32_t, site_esp(r) + WALK_PKT_SLOT) & ~1u;
    uint32_t root = GVAR(uint32_t, G_GFX_CUR_CTX) + 0x70;
    if (node >= root && node < root + 32)
        g_cur_bucket = (int)((node - root) / 4);
    if (node >= root && node < root + 32)
        trace(1, node, (uint32_t)g_cur_bucket);
    else if ((node < 0xCB7000u || node >= 0xCD7000u) && (node < 0x760000u || node >= 0x77E4C4u))   /* skip depth tables */
        trace(3, node, (uint32_t)g_cur_bucket);
}

static void on_deferred_draw(regs_t *r)
{
    const void *pkt = (const void *)GUEST(r->eax);
    unsigned i;
    g_cur_pkt = (const uint8_t *)pkt;
    g_cur_bucket = BUCKET_WORLD_DEFAULT;
    for (i = 1; i <= RECENT && i <= g_recent_next; i++)
        if (g_recent_pkt[(g_recent_next - i) % RECENT] == pkt) {
            g_cur_bucket = g_recent_bucket[(g_recent_next - i) % RECENT];
            break;
        }
}

/* Packets built by hud_emit_sprite, with the screen side each belongs to.
 * Arena addresses get reused every frame, so entries carry the frame they
 * were made in and only the last two frames' tags count. */
typedef struct { const void *pkt; unsigned frame; int side; uint32_t ctx; } hud_tag_t;
static hud_tag_t g_tags[HUD_TAGS];
static unsigned g_tag_next;

/* Tagged where the emitter links the finished packet (it can return early
 * without allocating, so its entry isn't a reliable point). The packet is a
 * 4-vertex sprite: X at +8 (left) / +0x10 (right), Y at +0xA. */
static void on_hud_emit_link(regs_t *r)
{
    const uint8_t *pkt = (const uint8_t *)GUEST(r->edx);
    int xl = *(const int16_t *)(pkt + 0x8), xr = *(const int16_t *)(pkt + 0x10);
    int y = *(const int16_t *)(pkt + 0xA);
    hud_tag_t *t;
    if (y >= 0 && y < HUD_ROW_MIN_Y)
        return;
    t = &g_tags[g_tag_next++ % HUD_TAGS];
    t->pkt = pkt;
    t->ctx = GVAR(uint32_t, G_GFX_CUR_CTX);
    t->frame = g_frame;
    t->side = (xl + xr) / 2 < 160 ? HUD_SIDE_LEFT : HUD_SIDE_RIGHT;
}

/* HUD elements drawn by code of their own, told apart by where their packets
 * lie: everything a piece of HUD code allocates sits between the packet arena
 * cursor at its start and at its end, so that range is the element.
 *   - the minimap (FUN_00436BA0): tiles, arrow and panel, from its call to the
 *     tile drawer (0x436CFD) to its exit (0x436D0B, also where its early outs
 *     jump). The full-screen map view shares the tile drawer, not these sites.
 *   - the boss gauges: the loop over the three boss slots in the HUD draw
 *     (0x4358E8), from its body (0x435C73, first pass) to after it (0x435CB5).
 *     Frame, fill (hud_emit_sprite_b) and whatever else a slot draws.
 *   - the second top gauge, Roll's on the boat rides (hud_draw_gauge_4363b5):
 *     its call (0x435CC2) to the jump after it (0x435CCC).
 * Arenas alternate per frame, so the last two ranges of each are kept, each
 * honoured for the frame it was built in and the next, in the same gfx ctx. */
typedef struct { uint32_t lo, hi; unsigned frame; uint32_t ctx; } hud_range_t;
static struct {
    hud_range_t r[2];
    unsigned next;
    uint32_t lo;
    int open;
} g_group[HUD_GROUPS];

static void group_start(int g)
{
    if (g_group[g].open)   /* the boss loop's body runs once per slot */
        return;
    g_group[g].lo = GVAR(uint32_t, G_GFX_ARENA_CURSOR);
    g_group[g].open = 1;
}

static void group_end(int g)
{
    hud_range_t *m;
    if (!g_group[g].open)   /* jumped to without passing the start */
        return;
    g_group[g].open = 0;
    m = &g_group[g].r[g_group[g].next++ & 1];
    m->lo = g_group[g].lo;
    m->hi = GVAR(uint32_t, G_GFX_ARENA_CURSOR);
    m->frame = g_frame;
    m->ctx = GVAR(uint32_t, G_GFX_CUR_CTX);
}

static void on_map_start(regs_t *r) { (void)r; group_start(HUD_GROUP_MAP); }
static void on_map_end(regs_t *r) { (void)r; group_end(HUD_GROUP_MAP); }
static void on_boss_start(regs_t *r) { (void)r; group_start(HUD_GROUP_BOSS); }
static void on_boss_end(regs_t *r) { (void)r; group_end(HUD_GROUP_BOSS); }
static void on_gauge2_start(regs_t *r) { (void)r; group_start(HUD_GROUP_GAUGE2); }
static void on_gauge2_end(regs_t *r) { (void)r; group_end(HUD_GROUP_GAUGE2); }

int hud_group_of(const void *pkt)
{
    uint32_t a, ctx;
    int g, i;
    if (g_cur_bucket != 2)   /* all of them draw into the HUD bucket */
        return HUD_GROUP_NONE;
    a = GADDR(pkt);
    ctx = GVAR(uint32_t, G_GFX_CUR_CTX);
    for (g = 0; g < HUD_GROUPS; g++)
        for (i = 0; i < 2; i++) {
            const hud_range_t *m = &g_group[g].r[i];
            if (a >= m->lo && a < m->hi && g_frame - m->frame <= 1 && m->ctx == ctx)
                return g;
        }
    return HUD_GROUP_NONE;
}

int hud_side_of(const void *pkt)
{
    static int logged;
    unsigned i;
    for (i = 0; i < HUD_TAGS; i++) {
        if (g_tags[i].pkt != pkt || g_frame - g_tags[i].frame > 1)
            continue;
        /* hud_emit_sprite only ever links into the HUD bucket (ctx+0x78);
         * a match anywhere else is a reused arena address */
        if (g_cur_bucket != 2) {
            if (logged < 6) {
                logged++;
                log_printf("packets: HUD tag %p (frame %u, now %u, built ctx %08X, walking ctx %08X) matched in bucket %d op %02X xy %d,%d, ignored",
                           pkt, g_tags[i].frame, g_frame, g_tags[i].ctx, GVAR(uint32_t, G_GFX_CUR_CTX), g_cur_bucket,
                           ((const uint8_t *)pkt)[7], *(const int16_t *)((const uint8_t *)pkt + 8), *(const int16_t *)((const uint8_t *)pkt + 0xA));
            }
            return HUD_SIDE_NONE;
        }
        return g_tags[i].side;
    }
    return HUD_SIDE_NONE;
}

/* [debug] hud_probe=1: who links what into OT buckets 2/3 (HUD, dialogue), with the
 * first X/Y of each packet, summed every 300 frames. Hooks gfx_add_prim, which isn't
 * a recompiler patch site: run with [debug] recomp=off. */
#define PROBE_SITES 128
typedef struct { uint32_t ret; uint8_t op, bucket; unsigned n; int x0, x1, y0, y1; int dumped; } probe_t;
static probe_t g_probe[PROBE_SITES];
static int g_probe_on;

static void on_add_prim_probe(regs_t *r)
{
    uint32_t ctx = GVAR(uint32_t, G_GFX_CUR_CTX) + 0x70;
    uint32_t ret = GVAR(uint32_t, site_esp(r));
    const uint8_t *pkt;
    int b, i, x, y;
    if (r->eax < ctx || r->eax >= ctx + 32)
        return;
    b = (int)((r->eax - ctx) / 4);
    if (b != 3 && b != 2)
        return;
    pkt = (const uint8_t *)GUEST(r->edx);
    for (i = 0; i < PROBE_SITES; i++)
        if (!g_probe[i].ret || (g_probe[i].ret == ret && g_probe[i].op == pkt[7] && g_probe[i].bucket == b))
            break;
    if (i == PROBE_SITES)
        return;
    if (!g_probe[i].ret) {
        g_probe[i].ret = ret; g_probe[i].op = pkt[7]; g_probe[i].bucket = (uint8_t)b;
        g_probe[i].x0 = g_probe[i].y0 = 99999; g_probe[i].x1 = g_probe[i].y1 = -99999;
    }
    x = *(const int16_t *)(pkt + 8); y = *(const int16_t *)(pkt + 0xA);
    if (x < g_probe[i].x0) g_probe[i].x0 = x;
    if (x > g_probe[i].x1) g_probe[i].x1 = x;
    if (y < g_probe[i].y0) g_probe[i].y0 = y;
    if (y > g_probe[i].y1) g_probe[i].y1 = y;
    g_probe[i].n++;
    if (g_probe[i].dumped < 2) {
        char hex[0x30 * 3 + 1];
        int k;
        g_probe[i].dumped++;
        for (k = 0; k < 0x30; k++)
            sprintf_s(hex + k * 3, 4, "%02X ", pkt[k]);
        log_printf("probe new b%d ret %08X op %02X pkt %08X: %s", b, ret, pkt[7], r->edx, hex);
    }
}

static void probe_frame(void)
{
    int i;
    if (!g_probe_on || g_frame % 300)
        return;
    log_printf("probe summary frame %u:", g_frame);
    for (i = 0; i < PROBE_SITES && g_probe[i].ret; i++) {
        log_printf("  b%d ret %08X op %02X n %u x %d..%d y %d..%d", g_probe[i].bucket, g_probe[i].ret,
                   g_probe[i].op, g_probe[i].n, g_probe[i].x0, g_probe[i].x1, g_probe[i].y0, g_probe[i].y1);
        g_probe[i].n = 0;
        g_probe[i].x0 = g_probe[i].y0 = 99999; g_probe[i].x1 = g_probe[i].y1 = -99999;
    }
}

void packets_probe_enable(void)
{
    static const uint8_t add_prim_bytes[] = { 0x51, 0x8B, 0x08, 0x89, 0x0A };
    if (hook_mid(GA(0x4F87A6u), sizeof add_prim_bytes, add_prim_bytes, on_add_prim_probe))
        g_probe_on = 1;
    log_printf("probe: add_prim hook %s", g_probe_on ? "on" : "FAILED");
}

void packets_frame_start(void)
{
    probe_frame();
    if (g_census && g_frame == TRACE_FRAME) {
        int i;
        log_printf("walk trace (frame %u, %d events):", g_frame, g_trace_n);
        for (i = 0; i < g_trace_n; i++) {
            if (g_trace[i][0] == 1)
                log_printf("  root link %08X -> bucket %u", g_trace[i][1], g_trace[i][2]);
            else if (g_trace[i][0] == 3)
                log_printf("  other link %08X (bucket stays %u)", g_trace[i][1], g_trace[i][2]);
            else
                log_printf("  pkt %08X op %02X in bucket %u", g_trace[i][1], g_trace[i][2] & 0xff, g_trace[i][2] >> 8);
        }
    }
    g_frame++;
    g_cur_bucket = 7;
    g_cur_pkt = NULL;
}

int packets_install(void)
{
    static const uint8_t dispatch_bytes[] = { 0xFF, 0x24, 0x8D, 0x24, 0x76, 0x4F, 0x00 };
    static const uint8_t link_bytes[] = { 0x88, 0xF9, 0x80, 0xE1, 0xFE, 0x88, 0x4C, 0x24, 0x64 };
    static const uint8_t hud_link_bytes[] = { 0xA1, 0x74, 0xEA, 0x77, 0x00, 0x83, 0xC0, 0x78 };
    static const uint8_t deferred_bytes[] = { 0xE8, 0xDA, 0xC9, 0xFF, 0xFF };
    static const uint8_t map_start_bytes[] = { 0xE8, 0xCE, 0x02, 0x00, 0x00 };   /* call map_draw_tiles */
    static const uint8_t map_end_bytes[] = { 0x83, 0xC4, 0x24, 0x5E, 0x5A };     /* add esp,0x24; pop esi; pop edx */
    static const uint8_t boss_start_bytes[] = { 0x8D, 0x4A, 0x02, 0x89, 0xC8 };  /* lea ecx,[edx+2]; mov eax,ecx */
    static const uint8_t boss_end_bytes[] = { 0xF6, 0x05, 0x10, 0x9A, 0x55, 0x00, 0x02 };   /* test byte [0x559A10],2 */
    static const uint8_t gauge2_start_bytes[] = { 0xB8, 0x10, 0x9A, 0x55, 0x00 };   /* mov eax,0x559A10 */
    static const uint8_t gauge2_end_bytes[] = { 0xE9, 0xA9, 0x03, 0x00, 0x00 };     /* jmp to the HUD draw's exit */
    if (!hook_mid(G_SITE_WALK_DISPATCH, sizeof dispatch_bytes, dispatch_bytes, on_dispatch) ||
        !hook_mid(G_SITE_WALK_LINK, sizeof link_bytes, link_bytes, on_link) ||
        !hook_mid(G_SITE_HUD_EMIT_LINK, sizeof hud_link_bytes, hud_link_bytes, on_hud_emit_link) ||
        !hook_mid(G_SITE_DEFERRED_DRAW, sizeof deferred_bytes, deferred_bytes, on_deferred_draw)) {
        log_printf("packets: walker hooks failed");
        return 0;
    }
    if (!hook_mid(G_SITE_MAP_START, sizeof map_start_bytes, map_start_bytes, on_map_start) ||
        !hook_mid(G_SITE_MAP_END, sizeof map_end_bytes, map_end_bytes, on_map_end))
        log_printf("packets: minimap hooks failed, the map stays in the 4:3 picture");
    if (!hook_mid(G_SITE_BOSS_START, sizeof boss_start_bytes, boss_start_bytes, on_boss_start) ||
        !hook_mid(G_SITE_BOSS_END, sizeof boss_end_bytes, boss_end_bytes, on_boss_end) ||
        !hook_mid(G_SITE_GAUGE2_START, sizeof gauge2_start_bytes, gauge2_start_bytes, on_gauge2_start) ||
        !hook_mid(G_SITE_GAUGE2_END, sizeof gauge2_end_bytes, gauge2_end_bytes, on_gauge2_end))
        log_printf("packets: top gauge hooks failed, the boss/Roll gauges stay in place");
    log_printf("packets: tracking walker dispatch, buckets and HUD sprites");
    return 1;
}

/* ---- census ---- */

typedef struct {
    unsigned prims, verts, flat2d;     /* flat2d: every vertex has RHW == 1 (screen-space 2D) */
    float x0, x1, y0, y1;
} stat_t;

static stat_t g_stats[8][256];
static unsigned g_census_frames;

void census_enable(void)
{
    g_census = 1;
    log_printf("census: on (dump every 150 frames)");
}

void census_draw(const float *v, unsigned stride, unsigned count)
{
    stat_t *s;
    unsigned i, flat = 1;
    int b = g_cur_bucket & 7, op = g_cur_pkt ? g_cur_pkt[7] : 0;

    if (!g_census || g_census_frames % 150 != 149)
        return;
    s = &g_stats[b][op];
    if (!s->prims) {
        s->x0 = s->y0 = FLT_MAX;
        s->x1 = s->y1 = -FLT_MAX;
    }
    s->prims++;
    s->verts += count;
    for (i = 0; i < count; i++) {
        const float *p = (const float *)((const char *)v + i * stride);
        if (p[0] < s->x0) s->x0 = p[0];
        if (p[0] > s->x1) s->x1 = p[0];
        if (p[1] < s->y0) s->y0 = p[1];
        if (p[1] > s->y1) s->y1 = p[1];
        if (p[3] != 1.0f) flat = 0;
    }
    s->flat2d += flat;
}

void census_frame(void)
{
    int b, op;
    if (!g_census)
        return;
    if (g_census_frames % 150 == 149) {   /* the frame we just sampled */
        log_printf("census frame %u (device coords before the shift; 2D = RHW 1):", g_census_frames);
        for (b = 7; b >= 0; b--)
            for (op = 0; op < 256; op++) {
                stat_t *s = &g_stats[b][op];
                if (!s->prims)
                    continue;
                log_printf("  b%d op%02X prims %4u verts %5u 2D %4u  x %7.1f..%7.1f  y %6.1f..%6.1f",
                           b, op, s->prims, s->verts, s->flat2d, s->x0, s->x1, s->y0, s->y1);
            }
        memset(g_stats, 0, sizeof g_stats);
    }
    g_census_frames++;
}
