#include <windows.h>
#include <float.h>
#include <string.h>

#include "game.h"
#include "hook.h"
#include "log.h"
#include "packets.h"

#define G_SITE_WALK_DISPATCH  0x4F7923u   /* jmp [ecx*4 + 0x4F7624]; packet at [esp+0x64] */
#define G_SITE_WALK_LINK      0x4F7ABCu   /* following a link node (bit 0 set) in [esp+0x64] */
#define G_SITE_HUD_EMIT_LINK  0x4365DAu   /* hud_emit_sprite, right before it links EDX = packet into ctx+0x78 */
#define G_SITE_DEFERRED_DRAW  0x4F7AE8u   /* call 0x4F44C7 for each deferred packet (EAX), after the walk */
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
    g_cur_pkt = *(const uint8_t **)(uintptr_t)(site_esp(r) + WALK_PKT_SLOT);
    g_recent_pkt[g_recent_next % RECENT] = (const void *)g_cur_pkt;
    g_recent_bucket[g_recent_next % RECENT] = (uint8_t)g_cur_bucket;
    g_recent_next++;
    if ((g_cur_pkt[7] & 0xFC) == 0x2C)
        trace(2, (uint32_t)(uintptr_t)g_cur_pkt, g_cur_pkt[7] | ((uint32_t)g_cur_bucket << 8));
}

static void on_link(regs_t *r)
{
    uint32_t node = *(uint32_t *)(uintptr_t)(site_esp(r) + WALK_PKT_SLOT) & ~1u;
    uint32_t root = *(uint32_t *)G_GFX_CUR_CTX + 0x70;
    if (node >= root && node < root + 32)
        g_cur_bucket = (int)((node - root) / 4);
    if (node >= root && node < root + 32)
        trace(1, node, (uint32_t)g_cur_bucket);
    else if ((node < 0xCB7000u || node >= 0xCD7000u) && (node < 0x760000u || node >= 0x77E4C4u))   /* skip depth tables */
        trace(3, node, (uint32_t)g_cur_bucket);
}

static void on_deferred_draw(regs_t *r)
{
    const void *pkt = (const void *)(uintptr_t)r->eax;
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
    const uint8_t *pkt = (const uint8_t *)(uintptr_t)r->edx;
    int xl = *(const int16_t *)(pkt + 0x8), xr = *(const int16_t *)(pkt + 0x10);
    int y = *(const int16_t *)(pkt + 0xA);
    hud_tag_t *t;
    if (y >= 0 && y < HUD_ROW_MIN_Y)
        return;
    t = &g_tags[g_tag_next++ % HUD_TAGS];
    t->pkt = pkt;
    t->ctx = *(uint32_t *)G_GFX_CUR_CTX;
    t->frame = g_frame;
    t->side = (xl + xr) / 2 < 160 ? HUD_SIDE_LEFT : HUD_SIDE_RIGHT;
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
                           pkt, g_tags[i].frame, g_frame, g_tags[i].ctx, *(uint32_t *)G_GFX_CUR_CTX, g_cur_bucket,
                           ((const uint8_t *)pkt)[7], *(const int16_t *)((const uint8_t *)pkt + 8), *(const int16_t *)((const uint8_t *)pkt + 0xA));
            }
            return HUD_SIDE_NONE;
        }
        return g_tags[i].side;
    }
    return HUD_SIDE_NONE;
}

void packets_frame_start(void)
{
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
    if (!hook_mid(G_SITE_WALK_DISPATCH, sizeof dispatch_bytes, dispatch_bytes, on_dispatch) ||
        !hook_mid(G_SITE_WALK_LINK, sizeof link_bytes, link_bytes, on_link) ||
        !hook_mid(G_SITE_HUD_EMIT_LINK, sizeof hud_link_bytes, hud_link_bytes, on_hud_emit_link) ||
        !hook_mid(G_SITE_DEFERRED_DRAW, sizeof deferred_bytes, deferred_bytes, on_deferred_draw)) {
        log_printf("packets: walker hooks failed");
        return 0;
    }
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
