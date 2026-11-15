/* True widescreen ("native" mode), experimental.
 *
 * Instead of rendering 4:3 and letting DDrawCompat stretch it, the game asks
 * for a wide display mode (480 high, width from the monitor's aspect, e.g.
 * 854x480 on 16:9) and the Direct3D viewport is widened to match. The game
 * still thinks in its 320x240 space and converts to a 640-wide device by x2,
 * so each primitive is placed on its way into IDirect3DDevice3::DrawPrimitive.
 * Placement is decided per primitive from the ordering-table bucket it came
 * from (see packets.c; layout derived from a per-frame census):
 *   - shift by (W - 640) / 2 (the default): the 3D world gets the extra width
 *     for free (with no anamorphic squeeze the projection already places
 *     geometry beyond the old edges), and dialogue/menus land centred.
 *   - stretch to the full width: background layers (buckets 7 and 6: sky
 *     tiles, menu wallpaper, title art) and anything 2D that spans the whole
 *     old screen (fades). That art was drawn for 4:3.
 *   - anchor to a screen corner: sprites built by the game's HUD emitter below
 *     the top row (life/weapon gauges, bottom left/right; side from the
 *     emitter's own X, see packets.c), the minimap (top left; everything its
 *     own draw code allocates, see packets.c) and our orbit toast. Other 2D
 *     in the same bucket (title text, the boss gauge row) stays centred, so
 *     composite pieces can't be pulled apart.
 *   - hud_scale shrinks (or grows) each anchored element about its own screen
 *     corner. One affine map per corner keeps every piece of an element where
 *     it belongs relative to the others, slide-in animations included.
 *   - the top-row gauges (boss gauges, Roll's on the boat rides) move up to
 *     top_gauge_y, clear of the dialogue boxes that open below the top edge.
 *     Each gauge moves as a whole (by group, see packets.c), stacked boss
 *     gauges keep their spacing. If the minimap would touch one of them (last
 *     frame's box), it steps down below it while that gauge is up.
 *
 * Needs DDrawCompat to allow the mode: SupportedResolutions must list WxH. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "addr.h"
#include "ddshim.h"
#include "display.h"
#include "hook.h"
#include "log.h"
#include "packets.h"
#include "toast.h"

#define G_SITE_MODE_WIDTH   GA(0x4F7EF3u)   /* push 0x280 -> SetDisplayMode width (flip-chain path) */
#define G_SITE_VIEWPORT_W   GA(0x4F81C1u)   /* mov [esp+0xd8],0x280 -> D3DVIEWPORT2.dwWidth */
#define G_SITE_CLEAR_W      GA(0x4F7F8Du)   /* mov eax,0x280 -> width of the Z-buffer FUN_004F134B creates */
#define G_CLEAR_RECT        GA(0x551D24u)   /* D3DRECT {0,0,640,480} the walker clears each frame */
#define VT_DRAW_PRIMITIVE           28
#define VT_DRAW_INDEXED_PRIMITIVE   29
#define D3DFVF_XYZRHW 0x004
#define MAX_VERTS 4096

typedef HRESULT(WINAPI *draw_prim_t)(void *dev, DWORD type, DWORD fvf, void *verts, DWORD count, DWORD flags);
typedef HRESULT(WINAPI *draw_indexed_t)(void *dev, DWORD type, DWORD fvf, void *verts, DWORD vcount,
                                         WORD *idx, DWORD icount, DWORD flags);

enum { PLACE_SHIFT, PLACE_STRETCH, PLACE_LEFT, PLACE_RIGHT, PLACE_TOP_LEFT, PLACE_TOAST, PLACE_TOP_GAUGE };
#define BUCKET_WORLD 4

static int g_width = 640;
static float g_offset_x;
static int g_hud_anchor = 1;
static float g_hud_scale = 1.0f;
static float g_map_dx, g_map_dy;   /* minimap move from its own place, device px (x2 the game's) */
#define MAP_GAME_X 14              /* where the game puts the minimap's panel (320x240) */
#define MAP_GAME_Y 36
#define MAP_SIZE 54                /* its panel, square */
#define BOSS_GAME_Y 16             /* the game's tops of the boss gauges and of Roll's (320x240) */
#define GAUGE2_GAME_Y 24
#define MAP_GAP 4.0f               /* device px between a top gauge and the minimap stepping aside */
static int g_map_x = MAP_GAME_X, g_map_y = MAP_GAME_Y;
static float g_top_dy[HUD_GROUPS];   /* per group: device px the top gauges move down (negative: up) */
static int g_cur_group = HUD_GROUP_NONE;
/* box of the top gauges as drawn (device px): this frame's, and last frame's that the map avoids */
static float g_top_box[4], g_top_prev[4];
static int g_top_seen, g_top_prev_seen;
static int g_active;
static draw_prim_t g_draw_prim;
static draw_indexed_t g_draw_indexed;
static unsigned g_draw_calls, g_indexed_calls, g_other_fvf;
static uint8_t g_vbuf[MAX_VERTS * 64];

static DWORD fvf_stride(DWORD fvf)
{
    DWORD s;
    if ((fvf & 0x00E) != D3DFVF_XYZRHW)
        return 0;
    s = 16;
    if (fvf & 0x010) s += 12;   /* normal */
    if (fvf & 0x020) s += 4;    /* point size */
    if (fvf & 0x040) s += 4;    /* diffuse */
    if (fvf & 0x080) s += 4;    /* specular */
    s += ((fvf >> 8) & 0xF) * 8;
    return s;
}

static int place(const uint8_t *v, DWORD stride, DWORD count)
{
    int bucket = g_cur_bucket;
    float x0 = 1e30f, x1 = -1e30f;
    DWORD i;

    if (g_cur_pkt && toast_owns((const void *)g_cur_pkt))
        return g_hud_anchor ? PLACE_TOAST : PLACE_SHIFT;
    if (bucket == BUCKET_WORLD)
        return PLACE_SHIFT;
    if (bucket >= 6)
        return PLACE_STRETCH;
    for (i = 0; i < count; i++) {
        const float *p = (const float *)(v + i * stride);
        if (p[0] < x0) x0 = p[0];
        if (p[0] > x1) x1 = p[0];
    }
    if (x0 <= 0.0f && x1 >= 640.0f)
        return PLACE_STRETCH;
    g_cur_group = g_cur_pkt ? hud_group_of((const void *)g_cur_pkt) : HUD_GROUP_NONE;
    if (g_cur_group == HUD_GROUP_MAP)
        return g_hud_anchor ? PLACE_TOP_LEFT : PLACE_SHIFT;
    if (g_cur_group == HUD_GROUP_BOSS || g_cur_group == HUD_GROUP_GAUGE2)
        return PLACE_TOP_GAUGE;
    if (g_hud_anchor && g_cur_pkt) {
        switch (hud_side_of((const void *)g_cur_pkt)) {
        case HUD_SIDE_LEFT:  return PLACE_LEFT;
        case HUD_SIDE_RIGHT: return PLACE_RIGHT;
        default: break;
        }
    }
    return PLACE_SHIFT;
}

/* How far the minimap steps down (device px, before hud_scale) so that it clears
 * last frame's top gauges, 0 when it doesn't touch them. */
static float map_step_down(float s)
{
    float x0 = 2.0f * (float)g_map_x * s, x1 = 2.0f * (float)(g_map_x + MAP_SIZE) * s;
    float y0 = 2.0f * (float)g_map_y * s, y1 = 2.0f * (float)(g_map_y + MAP_SIZE) * s;
    if (!g_top_prev_seen || x1 + MAP_GAP <= g_top_prev[0] || x0 >= g_top_prev[2] + MAP_GAP ||
        y1 + MAP_GAP <= g_top_prev[1] || y0 >= g_top_prev[3] + MAP_GAP)
        return 0.0f;
    return (g_top_prev[3] + MAP_GAP) / s - 2.0f * (float)g_map_y;
}

/* Copy (the game may draw the same staged vertices twice) and place X. */
static void *shifted(DWORD fvf, const void *verts, DWORD count)
{
    DWORD stride = fvf_stride(fvf), i;
    float scale = 1.0f, add = g_offset_x;
    float s = 1.0f, px = 0.0f, py = 0.0f;   /* hud_scale about the pivot (px, py), after placing */
    float yadd = 0.0f;
    int where;
    if (!stride || count > MAX_VERTS || stride * count > sizeof g_vbuf) {
        g_other_fvf++;
        return NULL;
    }
    census_draw((const float *)verts, stride, count);
    where = place((const uint8_t *)verts, stride, count);
    switch (where) {
    case PLACE_STRETCH: scale = (float)g_width / 640.0f; add = 0.0f; break;
    case PLACE_TOAST:    add = 0.0f; break;
    case PLACE_LEFT:     add = 0.0f; s = g_hud_scale; py = 480.0f; break;
    case PLACE_RIGHT:    add = 2.0f * g_offset_x; s = g_hud_scale; px = (float)g_width; py = 480.0f; break;
    case PLACE_TOP_LEFT: add = g_map_dx; yadd = g_map_dy + map_step_down(g_hud_scale); s = g_hud_scale; break;
    case PLACE_TOP_GAUGE: yadd = g_top_dy[g_cur_group]; break;
    default:             break;
    }
    memcpy(g_vbuf, verts, stride * count);
    for (i = 0; i < count; i++) {
        float *x = (float *)(g_vbuf + i * stride);
        *x = *x * scale + add;
        x[1] += yadd;
        if (s != 1.0f) {
            x[0] = px + (x[0] - px) * s;
            x[1] = py + (x[1] - py) * s;
        }
        if (where == PLACE_TOP_GAUGE) {
            if (!g_top_seen) {
                g_top_box[0] = g_top_box[2] = x[0];
                g_top_box[1] = g_top_box[3] = x[1];
                g_top_seen = 1;
            }
            if (x[0] < g_top_box[0]) g_top_box[0] = x[0];
            if (x[0] > g_top_box[2]) g_top_box[2] = x[0];
            if (x[1] < g_top_box[1]) g_top_box[1] = x[1];
            if (x[1] > g_top_box[3]) g_top_box[3] = x[1];
        }
    }
    return g_vbuf;
}

/* the game's own vertices for the draw in progress, before any shifting */
const void *g_display_raw_verts;

static HRESULT WINAPI hooked_draw_prim(void *dev, DWORD type, DWORD fvf, void *verts, DWORD count, DWORD flags)
{
    void *v = shifted(fvf, verts, count);
    HRESULT hr;
    g_draw_calls++;
    g_display_raw_verts = verts;
    hr = g_draw_prim(dev, type, fvf, v ? v : verts, count, flags);
    g_display_raw_verts = NULL;
    return hr;
}

static HRESULT WINAPI hooked_draw_indexed(void *dev, DWORD type, DWORD fvf, void *verts, DWORD vcount,
                                          WORD *idx, DWORD icount, DWORD flags)
{
    void *v = shifted(fvf, verts, vcount);
    g_indexed_calls++;
    return g_draw_indexed(dev, type, fvf, v ? v : verts, vcount, idx, icount, flags);
}

float display_world_shift(void)
{
    return g_active ? g_offset_x : 0.0f;
}

void display_set_top_gauge_y(int y)
{
    g_top_dy[HUD_GROUP_BOSS] = y < 0 ? 0.0f : 2.0f * (float)(y - BOSS_GAME_Y);
    g_top_dy[HUD_GROUP_GAUGE2] = y < 0 ? 0.0f : 2.0f * (float)(y - GAUGE2_GAME_Y);
}

void display_set_minimap_pos(int x, int y)
{
    g_map_x = x;
    g_map_y = y;
    g_map_dx = 2.0f * (float)(x - MAP_GAME_X);
    g_map_dy = 2.0f * (float)(y - MAP_GAME_Y);
}

void display_set_hud_scale(float s)
{
    g_hud_scale = s < 0.5f ? 0.5f : s > 1.5f ? 1.5f : s;
}

int display_native_width(void)
{
    return g_active ? g_width : 640;
}

int display_install(int screen_w, int screen_h, int width, int hud_anchor, float hud_scale)
{
    static const uint8_t push_640[] = { 0x68, 0x80, 0x02, 0x00, 0x00 };
    static const uint8_t vp_640[] = { 0xC7, 0x84, 0x24, 0xD8, 0x00, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00 };
    static const uint8_t mov_eax_640[] = { 0xB8, 0x80, 0x02, 0x00, 0x00 };
    uint32_t w;

    /* 480 lines tall, as wide as the monitor's aspect (even, so the 4:3 picture
     * sits on whole pixels); an explicit width from mml.ini wins */
    g_width = width > 0 ? width : (((int)floor(480.0 * screen_w / screen_h + 0.5) + 1) & ~1);
    if (g_width <= 640) {
        log_printf("display: screen isn't wider than 4:3, native mode off");
        return 0;
    }
    if (!mem_expect(G_SITE_MODE_WIDTH, push_640, sizeof push_640) ||
        !mem_expect(G_SITE_VIEWPORT_W, vp_640, sizeof vp_640) ||
        !mem_expect(G_SITE_CLEAR_W, mov_eax_640, sizeof mov_eax_640)) {
        log_printf("display: setup code unexpected, native mode off");
        return 0;
    }
    w = (uint32_t)g_width;
    mem_write(G_CLEAR_RECT + 8, &w, 4);   /* D3DRECT.x2 used by the per-frame viewport Clear */
    mem_write(G_SITE_MODE_WIDTH + 1, &w, 4);
    mem_write(G_SITE_VIEWPORT_W + 7, &w, 4);
    mem_write(G_SITE_CLEAR_W + 1, &w, 4);
    g_offset_x = (float)(g_width - 640) / 2.0f;
    g_hud_anchor = hud_anchor;
    display_set_hud_scale(hud_scale);
    g_active = 1;
    packets_install();
    log_printf("display: native widescreen %dx480, 2D/3D shifted by %.1f px (DDrawCompat must allow %dx480), HUD %s, scale %.2f",
               g_width, g_offset_x, g_width, g_hud_anchor ? "at the edges" : "centred", g_hud_scale);
    return 1;
}

void display_frame(void)
{
    static unsigned frames;
    void **vt;

    if (!g_active)
        return;
    census_frame();
    packets_frame_start();
    memcpy(g_top_prev, g_top_box, sizeof g_top_prev);
    g_top_prev_seen = g_top_seen;
    g_top_seen = 0;
    vt = ddshim_device_vtable();
    if (vt && !g_draw_prim) {
        void *mine1 = (void *)hooked_draw_prim, *mine2 = (void *)hooked_draw_indexed;
        g_draw_prim = (draw_prim_t)vt[VT_DRAW_PRIMITIVE];
        g_draw_indexed = (draw_indexed_t)vt[VT_DRAW_INDEXED_PRIMITIVE];
        host_write(&vt[VT_DRAW_PRIMITIVE], &mine1, sizeof mine1);
        host_write(&vt[VT_DRAW_INDEXED_PRIMITIVE], &mine2, sizeof mine2);
        log_printf("display: DrawPrimitive hooked (vtable %p)", (void *)vt);
    }
    if (++frames == 600)
        log_printf("display: after 600 frames: DrawPrimitive %u, DrawIndexedPrimitive %u, non-XYZRHW %u",
                   g_draw_calls, g_indexed_calls, g_other_fvf);
}
