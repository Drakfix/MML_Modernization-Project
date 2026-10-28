/* Short notices ("ORBIT ON", "QUICK SAVED"...).
 *
 * With our renderer they're drawn with gfx_ui in the settings menu's style:
 * a small panel at the top of the screen with an icon, a title, an optional
 * ON/OFF badge, a line of detail and a bar that runs down until it goes.
 *
 * With DDrawCompat: a coloured dot plus a word or two in the game's font,
 * top-left, sliding in and out. Drawn inside the game's display list so it
 * shows up through DDrawCompat's flip presentation, which an overlay window
 * can't draw over.
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
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "game.h"
#include "gfx_ui.h"
#include "mml.h"
#include "toast.h"
#include "ui_style.h"

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
#define MAX_GLYPHS 24

static __declspec(align(8)) uint8_t g_dot[0x40];
static __declspec(align(8)) uint8_t g_glyph[MAX_GLYPHS][0x10];
static volatile LONG g_frames_left;
static char g_text[MAX_GLYPHS + 1];
static uint8_t g_rgb[3];

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

static void legacy_message(const char *text, int r, int g, int b)
{
    strncpy_s(g_text, sizeof g_text, text, _TRUNCATE);
    g_rgb[0] = (uint8_t)r;
    g_rgb[1] = (uint8_t)g;
    g_rgb[2] = (uint8_t)b;
    g_frames_left = TOAST_DUR;
}

/* ---- our renderer ---- */

#define UI_DUR 2.8      /* seconds on screen */
#define UI_IN 0.22
#define UI_OUT 0.3

static SRWLOCK g_lock = SRWLOCK_INIT;
static struct {
    int on, icon, tone;
    char title[32], badge[8], detail[96];
    double start, bump;
} T;
static int g_ui;
static LARGE_INTEGER g_qpf;

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_qpf.QuadPart;
}

static void icon(int which, float cx, float cy, float s, uint32_t dark)
{
    const uint32_t W = 0xFFFFFFFF;
    if (which == TOAST_ICON_PAD) {   /* a controller */
        float bw = s * 0.78f, r = s * 0.3f;
        gfx_ui_rect(cx - bw / 2, cy - r, bw, r * 1.5f, ui_col(W));
        gfx_ui_circle(cx - bw / 2, cy - r * 0.15f, r, ui_col(W));     /* the two halves... */
        gfx_ui_circle(cx + bw / 2, cy - r * 0.15f, r, ui_col(W));
        gfx_ui_circle(cx - bw / 2 - r * 0.1f, cy + r * 0.55f, r * 0.8f, ui_col(W));   /* ...and the grips */
        gfx_ui_circle(cx + bw / 2 + r * 0.1f, cy + r * 0.55f, r * 0.8f, ui_col(W));
        gfx_ui_rect(cx - bw / 2 - r * 0.5f, cy - r * 0.27f, r, r * 0.3f, ui_col(dark));   /* d-pad */
        gfx_ui_rect(cx - bw / 2 - r * 0.15f, cy - r * 0.62f, r * 0.3f, r, ui_col(dark));
        gfx_ui_circle(cx + bw / 2 - r * 0.25f, cy - r * 0.05f, r * 0.2f, ui_col(dark));   /* buttons */
        gfx_ui_circle(cx + bw / 2 + r * 0.25f, cy - r * 0.45f, r * 0.2f, ui_col(dark));
    } else if (which == TOAST_ICON_SAVE) {   /* a floppy disk */
        float h = s * 0.5f;
        gfx_ui_rect(cx - h, cy - h, s, s, ui_col(W));
        gfx_ui_tri(cx + h - s * 0.2f, cy - h, cx + h, cy - h, cx + h, cy - h + s * 0.2f, ui_col(dark));
        gfx_ui_rect(cx - h + s * 0.2f, cy - h, s * 0.5f, s * 0.32f, ui_col(dark));
        gfx_ui_rect(cx - h + s * 0.48f, cy - h + s * 0.05f, s * 0.12f, s * 0.2f, ui_col(W));
        gfx_ui_rect(cx - h + s * 0.14f, cy + s * 0.04f, s * 0.72f, s * 0.38f, ui_col(dark));
    } else {   /* warning sign */
        float h = s * 0.55f, sc = s / gfx_ui_font_line(UI_FONT_DISPLAY, 1) * 0.75f;
        float lh = gfx_ui_font_line(UI_FONT_DISPLAY, sc);
        gfx_ui_tri(cx, cy - h, cx + h * 1.1f, cy + h * 0.8f, cx - h * 1.1f, cy + h * 0.8f, ui_col(W));
        gfx_ui_font_text(UI_FONT_DISPLAY, cx - gfx_ui_font_width(UI_FONT_DISPLAY, "!", sc) / 2,
                         cy + h * 0.25f - lh / 2, "!", ui_col(dark), ui_col(dark), sc);
    }
}

static void draw(void *user, int sw, int sh)
{
    static const uint32_t TONE_HI[3] = { 0xFF3CCB62, 0xFF6A7C92, 0xFFFF8A3A };
    static const uint32_t TONE_LO[3] = { 0xFF158A3E, 0xFF36455A, 0xFFD23C34 };
    char title[32], badge[8], detail[96];
    int ic, tone;
    double t = now_s(), age, bump;
    float u, h, ib, x, y, w, cw, tw, bw = 0, in, out, cut, ts = 0.5f, ds = 0.62f, bs = 0.34f;
    (void)user;
    (void)sh;
    AcquireSRWLockShared(&g_lock);
    if (!T.on) {
        ReleaseSRWLockShared(&g_lock);
        return;
    }
    age = t - T.start;
    bump = t - T.bump;
    ic = T.icon;
    tone = T.tone;
    memcpy(title, T.title, sizeof title);
    memcpy(badge, T.badge, sizeof badge);
    memcpy(detail, T.detail, sizeof detail);
    ReleaseSRWLockShared(&g_lock);
    if (age < 0 || age > UI_DUR)
        return;
    in = ui_ease_out((float)(age / UI_IN));
    out = age > UI_DUR - UI_OUT ? (float)((UI_DUR - age) / UI_OUT) : 1;
    ui_alpha = in < out ? in : out;

    u = gfx_ui_line(1.0f) * 0.85f;
    h = detail[0] ? u * 2.75f : u * 1.7f;
    ib = h * 1.05f;
    cut = u * 0.55f;
    tw = gfx_ui_font_width(UI_FONT_DISPLAY, title, ts);
    if (badge[0])
        bw = gfx_ui_font_width(UI_FONT_DISPLAY, badge, bs) + u * 0.8f;
    cw = tw + (bw > 0 ? bw + u * 0.4f : 0);
    if (detail[0] && gfx_ui_font_width(UI_FONT_BODY, detail, ds) > cw)
        cw = gfx_ui_font_width(UI_FONT_BODY, detail, ds);
    w = ib + u * 0.6f + cw + u * 1.0f;
    x = (sw - w) / 2;
    y = u * 0.8f - (1 - in) * u * 1.4f - (1 - out) * u * 0.8f;

    ui_panel(x + u * 0.2f, y + u * 0.25f, w, h, cut, 0x60000000, 0x60000000);
    ui_panel(x, y, w, h, cut, COL_PANEL_TOP, COL_PANEL_BOT);
    {   /* the icon block: tone colour, leaning right edge, flashes when a notice replaces another */
        float xy[8], flash = bump < 0.25 ? (float)(1 - bump / 0.25) : 0;
        uint32_t c[4];
        xy[0] = x; xy[1] = y;
        xy[2] = x + ib; xy[3] = y;
        xy[4] = x + ib - h * 0.3f; xy[5] = y + h;
        xy[6] = x; xy[7] = y + h - cut;
        c[0] = c[1] = ui_col(ui_lerp_col(TONE_HI[tone], 0xFFFFFFFF, flash * 0.6f));
        c[2] = c[3] = ui_col(ui_lerp_col(TONE_LO[tone], 0xFFFFFFFF, flash * 0.6f));
        gfx_ui_quad(xy, c);
        icon(ic, x + (ib - h * 0.3f) * 0.52f, y + h / 2, h * 0.48f, TONE_LO[tone]);
    }
    ui_panel_edge(x, y, w, h, cut, u * 0.07f, COL_EDGE);
    {
        float b = u * 0.55f, th = u * 0.1f;
        ui_line(x, y + b, x, y - th / 2, th, COL_YELLOW);
        ui_line(x - th / 2, y, x + b, y, th, COL_YELLOW);
    }
    {
        float tx = x + ib + u * 0.4f, lh = gfx_ui_font_line(UI_FONT_DISPLAY, ts);
        float ty = detail[0] ? y + h * 0.1f : y + (h - lh) / 2;
        ui_text_outlined(UI_FONT_DISPLAY, tx, ty, title, 0xFFFFFFFF, 0xFF9EE2FF, ts, u * 0.05f, 0xFF061226);
        if (badge[0]) {
            float bx = tx + tw + u * 0.4f, bh = lh * 0.62f, by = ty + (lh - bh) / 2;
            float blh = gfx_ui_font_line(UI_FONT_DISPLAY, bs);
            float btw = gfx_ui_font_width(UI_FONT_DISPLAY, badge, bs);
            ui_slant(bx, by, bw, bh, bh * 0.3f, TONE_HI[tone], TONE_LO[tone]);
            gfx_ui_font_text(UI_FONT_DISPLAY, bx + (bw + bh * 0.3f - btw) / 2, by + (bh - blh) / 2, badge,
                             ui_col(0xFFFFFFFF), ui_col(0xFFFFFFFF), bs);
        }
        if (detail[0])
            ui_text(tx, y + h * 0.52f, detail, 0xFFB4C6D8, ds);
    }
    {   /* time left */
        float left = (float)(1 - age / UI_DUR), bx = x + ib - h * 0.3f + u * 0.2f, bwid = w - (bx - x) - cut;
        gfx_ui_rect(bx, y + h - u * 0.16f, bwid * left, u * 0.06f, ui_col(ui_with_alpha(TONE_HI[tone], 0.8f)));
    }
    ui_alpha = 1;
}

void toast_use_ui(void)
{
    QueryPerformanceFrequency(&g_qpf);
    g_ui = 1;
    gfx_ui_add_draw(draw, NULL);
}

/* the game's font has letters and spaces: keep those, in capitals */
static void legacy_text(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 1 < n; in++) {
        char c = *in;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || c == ' ')
            out[k++] = c;
    }
    out[k] = 0;
}

void toast_post(int icon_kind, int tone, const char *title, const char *badge, const char *detail)
{
    if (g_ui) {
        double t = now_s();
        AcquireSRWLockExclusive(&g_lock);
        /* already up: stay put and flash, instead of sliding in again */
        if (T.on && t - T.start < UI_DUR - UI_OUT)
            T.start = t - UI_IN;
        else
            T.start = t;
        T.bump = t;
        T.on = 1;
        T.icon = icon_kind;
        T.tone = tone;
        strncpy_s(T.title, sizeof T.title, title, _TRUNCATE);
        strncpy_s(T.badge, sizeof T.badge, badge ? badge : "", _TRUNCATE);
        strncpy_s(T.detail, sizeof T.detail, detail ? detail : "", _TRUNCATE);
        ReleaseSRWLockExclusive(&g_lock);
    } else {
        char text[MAX_GLYPHS + 1], full[64];
        if (icon_kind == TOAST_ICON_PAD && badge)
            _snprintf_s(full, sizeof full, _TRUNCATE, "MODERN CONTROLS %s", badge);
        else
            strncpy_s(full, sizeof full, title, _TRUNCATE);
        legacy_text(full, text, sizeof text);
        legacy_message(text, tone == TOAST_GOOD ? 0 : 224, tone == TOAST_GOOD ? 224 : 0, 0);
    }
}

/* the modern controls toggle (F5 / L3 / R3); internally still "the orbit" */
void toast_show(int on)
{
    toast_post(TOAST_ICON_PAD, on ? TOAST_GOOD : TOAST_OFF, "MODERN CONTROLS", on ? "ON" : "OFF",
               on ? "Free camera, lock-on targeting and dodge rolls"
                  : "The original 1997 controls and camera");
}

void toast_frame(void)
{
    int left = g_frames_left, i, y, ty, cx, xl, xr;
    int x = display_native_width() > 640 ? TOAST_X_NATIVE : TOAST_X;
    double slide = 0;
    const char *s;
    static const uint8_t w01[4] = { 0xCD, 0xCC, 0xCC, 0x3D };   /* 0.1f */

    if (g_ui || left <= 0 || !*(void **)G_GFX_CUR_CTX)
        return;
    if (left > TOAST_DUR - TOAST_FADE)
        slide = (double)(left - (TOAST_DUR - TOAST_FADE)) / TOAST_FADE * TOAST_SLIDE;
    else if (left < TOAST_FADE)
        slide = (double)(TOAST_FADE - left) / TOAST_FADE * TOAST_SLIDE;
    y = TOAST_Y - (int)floor(slide + 0.5);

    g_dot[4] = g_rgb[0];
    g_dot[5] = g_rgb[1];
    g_dot[6] = g_rgb[2];
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

    s = g_text;
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
