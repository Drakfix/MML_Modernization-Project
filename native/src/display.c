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
 *   - anchor to the left/right screen edge: sprites built by the game's HUD
 *     emitter below the top row (life/weapon gauges; side from the emitter's
 *     own X, see packets.c), and our orbit toast. Other 2D in the same
 *     bucket (title text, the boss gauge row) stays centred, so composite
 *     pieces can't be pulled apart.
 *
 * Needs DDrawCompat to allow the mode: SupportedResolutions must list WxH. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "display.h"
#include "hook.h"
#include "log.h"
#include "packets.h"
#include "toast.h"

#define G_D3D_DEVICE        0x551D10u   /* IDirect3DDevice3* */
#define G_SITE_MODE_WIDTH   0x4F7EF3u   /* push 0x280 -> SetDisplayMode width (flip-chain path) */
#define G_SITE_VIEWPORT_W   0x4F81C1u   /* mov [esp+0xd8],0x280 -> D3DVIEWPORT2.dwWidth */
#define G_SITE_CLEAR_W      0x4F7F8Du   /* mov eax,0x280 -> width of the Z-buffer FUN_004F134B creates */
#define G_CLEAR_RECT        0x551D24u   /* D3DRECT {0,0,640,480} the walker clears each frame */
#define VT_DRAW_PRIMITIVE           28
#define VT_DRAW_INDEXED_PRIMITIVE   29
#define D3DFVF_XYZRHW 0x004
#define MAX_VERTS 4096

typedef HRESULT(WINAPI *draw_prim_t)(void *dev, DWORD type, DWORD fvf, void *verts, DWORD count, DWORD flags);
typedef HRESULT(WINAPI *draw_indexed_t)(void *dev, DWORD type, DWORD fvf, void *verts, DWORD vcount,
                                         WORD *idx, DWORD icount, DWORD flags);

enum { PLACE_SHIFT, PLACE_STRETCH, PLACE_LEFT, PLACE_RIGHT };
#define BUCKET_WORLD 4

static int g_width = 640;
static float g_offset_x;
static int g_hud_anchor = 1;
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
        return g_hud_anchor ? PLACE_LEFT : PLACE_SHIFT;
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
    if (g_hud_anchor && g_cur_pkt) {
        switch (hud_side_of((const void *)g_cur_pkt)) {
        case HUD_SIDE_LEFT:  return PLACE_LEFT;
        case HUD_SIDE_RIGHT: return PLACE_RIGHT;
        default: break;
        }
    }
    return PLACE_SHIFT;
}

/* Copy (the game may draw the same staged vertices twice) and place X. */
static void *shifted(DWORD fvf, const void *verts, DWORD count)
{
    DWORD stride = fvf_stride(fvf), i;
    float scale = 1.0f, add = g_offset_x;
    if (!stride || count > MAX_VERTS || stride * count > sizeof g_vbuf) {
        g_other_fvf++;
        return NULL;
    }
    census_draw((const float *)verts, stride, count);
    switch (place((const uint8_t *)verts, stride, count)) {
    case PLACE_STRETCH: scale = (float)g_width / 640.0f; add = 0.0f; break;
    case PLACE_LEFT:    add = 0.0f; break;
    case PLACE_RIGHT:   add = 2.0f * g_offset_x; break;
    default:            break;
    }
    memcpy(g_vbuf, verts, stride * count);
    for (i = 0; i < count; i++) {
        float *x = (float *)(g_vbuf + i * stride);
        *x = *x * scale + add;
    }
    return g_vbuf;
}

static HRESULT WINAPI hooked_draw_prim(void *dev, DWORD type, DWORD fvf, void *verts, DWORD count, DWORD flags)
{
    void *v = shifted(fvf, verts, count);
    g_draw_calls++;
    return g_draw_prim(dev, type, fvf, v ? v : verts, count, flags);
}

static HRESULT WINAPI hooked_draw_indexed(void *dev, DWORD type, DWORD fvf, void *verts, DWORD vcount,
                                          WORD *idx, DWORD icount, DWORD flags)
{
    void *v = shifted(fvf, verts, vcount);
    g_indexed_calls++;
    return g_draw_indexed(dev, type, fvf, v ? v : verts, vcount, idx, icount, flags);
}

int display_native_width(void)
{
    return g_active ? g_width : 640;
}

int display_install(int screen_w, int screen_h, int width, int hud_anchor)
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
    g_active = 1;
    packets_install();
    log_printf("display: native widescreen %dx480, 2D/3D shifted by %.1f px (DDrawCompat must allow %dx480)",
               g_width, g_offset_x, g_width);
    return 1;
}

void display_frame(void)
{
    static unsigned frames;
    void **vt;
    void *dev;

    if (!g_active)
        return;
    census_frame();
    packets_frame_start();
    dev = *(void **)G_D3D_DEVICE;
    if (dev && !g_draw_prim) {
        void *mine1 = (void *)hooked_draw_prim, *mine2 = (void *)hooked_draw_indexed;
        vt = *(void ***)dev;
        g_draw_prim = (draw_prim_t)vt[VT_DRAW_PRIMITIVE];
        g_draw_indexed = (draw_indexed_t)vt[VT_DRAW_INDEXED_PRIMITIVE];
        mem_write((uintptr_t)&vt[VT_DRAW_PRIMITIVE], &mine1, sizeof mine1);
        mem_write((uintptr_t)&vt[VT_DRAW_INDEXED_PRIMITIVE], &mine2, sizeof mine2);
        log_printf("display: DrawPrimitive hooked (device %p, vtable %p)", dev, (void *)vt);
    }
    if (++frames == 600)
        log_printf("display: after 600 frames: DrawPrimitive %u, DrawIndexedPrimitive %u, non-XYZRHW %u",
                   g_draw_calls, g_indexed_calls, g_other_fvf);
}
