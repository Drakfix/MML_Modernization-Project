/* Orbit-mode indicator: a coloured dot plus "ORBIT ON"/"ORBIT OFF" in the
 * game's font, top-left, sliding in and out. Drawn inside the game's display
 * list so it shows up through DDrawCompat's flip presentation, which an
 * overlay window can't draw over.
 *
 * DOT  = flat translucent quad, opcode 0x2a (handler gfx_draw_flat_quad):
 *        RGB at +4/+5/+6, four u16 X/Y pairs at +8..+0x16 in TRIANGLESTRIP
 *        order (TL, TR, BL, BR), W float 0.1 at +0x18..+0x24 (RHW 1.0).
 * TEXT = one 0x10-byte glyph packet per character, opcode 0x6c: RGB, X at +8,
 *        Y at +0xA, font tile code at +0xC, font page at +0xE. Tile codes are
 *        linear: A-Z = 0x15.., a-z = 0x2f.., space = 0x0a. The colour is
 *        doubled by the handler, so 0x80 is white. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "display.h"
#include "game.h"
#include "mml.h"
#include "toast.h"

#define TOAST_DUR 80          /* frames visible (~2.7 s at 30 fps) */
#define TOAST_FADE 12         /* frames of slide in / slide out */
#define TOAST_SLIDE 34        /* px travelled off the top */
#define TOAST_X (-2)          /* anamorphic: tuned against the squeezed HUD */
#define TOAST_X_NATIVE 6      /* native widescreen: anchored to the real screen edge */
#define TOAST_Y 16
#define DOT_W 9
#define DOT_H 9
#define TXT_GAP 5
#define TXT_DY (-2)
#define TXT_COL 0x80
#define FONT_PAGE 0x7f3c
#define MAX_GLYPHS 12

static __declspec(align(8)) uint8_t g_dot[0x40];
static __declspec(align(8)) uint8_t g_glyph[MAX_GLYPHS][0x10];
static volatile LONG g_frames_left;
static volatile LONG g_on;

static void put16(uint8_t *p, int off, int v) { *(uint16_t *)(p + off) = (uint16_t)(v & 0xffff); }

/* match the widescreen HUD proportions (squeeze about the 320-space centre) */
static int sqx(int x)
{
    float r = mml_hud_ratio();
    return (int)floor(160.0 + (x - 160) * (double)r + 0.5);
}

static void prepend_hud(uint8_t *pkt)
{
    uint8_t *ctx = *(uint8_t **)G_GFX_CUR_CTX;
    uint32_t *bucket = (uint32_t *)(ctx + 0x78);
    *(uint32_t *)pkt = *bucket;
    *bucket = (uint32_t)(uintptr_t)pkt;
}

static int tile_of(char c)
{
    if (c >= 'A' && c <= 'Z') return 0x15 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 0x2f + (c - 'a');
    return 0x0a;
}

int toast_owns(const void *pkt)
{
    const uint8_t *p = (const uint8_t *)pkt;
    return p == g_dot || (p >= g_glyph[0] && p < g_glyph[0] + sizeof g_glyph);
}

void toast_show(int on)
{
    g_on = on ? 1 : 0;
    g_frames_left = TOAST_DUR;
}

void toast_frame(void)
{
    int left = g_frames_left, i, y, ty, cx, xl, xr;
    int x = display_native_width() > 640 ? TOAST_X_NATIVE : TOAST_X;
    double slide = 0;
    const char *s;
    static const uint8_t w01[4] = { 0xCD, 0xCC, 0xCC, 0x3D };   /* 0.1f */

    if (left <= 0 || !*(void **)G_GFX_CUR_CTX)
        return;
    if (left > TOAST_DUR - TOAST_FADE)
        slide = (double)(left - (TOAST_DUR - TOAST_FADE)) / TOAST_FADE * TOAST_SLIDE;
    else if (left < TOAST_FADE)
        slide = (double)(TOAST_FADE - left) / TOAST_FADE * TOAST_SLIDE;
    y = TOAST_Y - (int)floor(slide + 0.5);

    g_dot[4] = g_on ? 0 : 224;
    g_dot[5] = g_on ? 224 : 0;
    g_dot[6] = 0;
    g_dot[7] = 0x2a;
    xl = sqx(x);
    xr = sqx(x + DOT_W);
    put16(g_dot, 0x08, xl); put16(g_dot, 0x0a, y);
    put16(g_dot, 0x0c, xr); put16(g_dot, 0x0e, y);
    put16(g_dot, 0x10, xl); put16(g_dot, 0x12, y + DOT_H);
    put16(g_dot, 0x14, xr); put16(g_dot, 0x16, y + DOT_H);
    for (i = 0; i < 4; i++)
        memcpy(g_dot + 0x18 + 4 * i, w01, 4);
    prepend_hud(g_dot);

    s = g_on ? "ORBIT ON" : "ORBIT OFF";
    ty = y + TXT_DY;
    cx = x + DOT_W + TXT_GAP;
    for (i = 0; s[i] && i < MAX_GLYPHS; i++) {
        uint8_t *g = g_glyph[i];
        int tc = tile_of(s[i]);
        g[4] = g[5] = g[6] = TXT_COL;
        g[7] = 0x6c;
        put16(g, 0x08, sqx(cx));
        put16(g, 0x0a, ty);
        put16(g, 0x0c, tc);
        put16(g, 0x0e, FONT_PAGE);
        prepend_hud(g);
        cx += *(uint8_t *)(G_FONT_WIDTHS + tc) + 1;
    }
    g_frames_left = left - 1;
}
