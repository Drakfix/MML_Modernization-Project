/* Our own UI over the final picture (the in-game menu): shapes and text in
 * window pixels, drawn after the game's image and any shader preset so it
 * stays sharp. Text comes from a coverage atlas the platform's font engine
 * rasterises (ui_font.h) at a size that follows the window height (rebuilt
 * when that changes): a body font and a bolder display font for titles. The
 * UI's owner registers a draw callback; the backend calls gfx_ui_build while
 * composing every presented frame and draws what it returns. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_ui.h"
#include "log.h"
#include "ui_font.h"

#define ATLAS_W 2048
#define ATLAS_H 2048
#define FIRST_CHAR UI_FIRST_CHAR
#define LAST_CHAR UI_LAST_CHAR
#define MAX_VERTS 60000

typedef gfx_ui_vert ui_vert;

static struct {
    int failed;
    uint8_t *atlas;                    /* R8 coverage, ATLAS_W x ATLAS_H */
    unsigned atlas_gen;                /* changes whenever the atlas is rebuilt */
    int font_px;                       /* body line height the atlas was made for */
    float line[UI_FONTS];              /* line height of each font at scale 1 */
    ui_glyph glyph[UI_FONTS][LAST_CHAR + 1];
    ui_vert verts[MAX_VERTS];
    int nverts;
    struct { gfx_ui_draw_fn fn; void *user; } draw[4];
    int ndraw;
} U;

void gfx_ui_add_draw(gfx_ui_draw_fn draw, void *user)
{
    if (U.ndraw < (int)(sizeof U.draw / sizeof U.draw[0])) {
        U.draw[U.ndraw].fn = draw;
        U.draw[U.ndraw].user = user;
        U.ndraw++;
    }
}

/* both fonts at px line height into the atlas, and a solid 4x4 block at (0,0) for plain shapes */
static int make_font(int px)
{
    int c;
    if (!U.atlas)
        U.atlas = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H);
    if (!U.atlas || !ui_font_rasterize(px, U.atlas, ATLAS_W, ATLAS_H, U.glyph, U.line))
        return 0;
    for (c = 0; c < 4; c++)
        memset(U.atlas + c * ATLAS_W, 0xFF, 4);
    U.font_px = px;
    U.atlas_gen++;
    return 1;
}

#define SOLID_UV (1.5f / ATLAS_W)

static void vert(ui_vert *v, float x, float y, float u, float t, uint32_t c)
{
    v->x = x;
    v->y = y;
    v->u = u;
    v->v = t;
    v->color = c;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 uint32_t top, uint32_t bottom)
{
    ui_vert *v;
    if (U.nverts + 6 > MAX_VERTS)
        return;
    v = U.verts + U.nverts;
    vert(v + 0, x0, y0, u0, v0, top);
    vert(v + 1, x1, y0, u1, v0, top);
    vert(v + 2, x0, y1, u0, v1, bottom);
    v[3] = v[2];
    v[4] = v[1];
    vert(v + 5, x1, y1, u1, v1, bottom);
    U.nverts += 6;
}

void gfx_ui_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb)
{
    ui_vert *v;
    if (U.nverts + 3 > MAX_VERTS)
        return;
    v = U.verts + U.nverts;
    vert(v + 0, x0, y0, SOLID_UV, SOLID_UV, argb);
    vert(v + 1, x1, y1, SOLID_UV, SOLID_UV, argb);
    vert(v + 2, x2, y2, SOLID_UV, SOLID_UV, argb);
    U.nverts += 3;
}

void gfx_ui_quad(const float xy[8], const uint32_t c[4])
{
    static const int order[6] = { 0, 1, 3, 3, 1, 2 };
    ui_vert *v;
    int i;
    if (U.nverts + 6 > MAX_VERTS)
        return;
    v = U.verts + U.nverts;
    for (i = 0; i < 6; i++)
        vert(v + i, xy[order[i] * 2], xy[order[i] * 2 + 1], SOLID_UV, SOLID_UV, c[order[i]]);
    U.nverts += 6;
}

void gfx_ui_poly(const float *xy, int n, uint32_t argb)
{
    int i;
    for (i = 1; i + 1 < n; i++)
        gfx_ui_tri(xy[0], xy[1], xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], argb);
}

void gfx_ui_rect(float x, float y, float w, float h, uint32_t argb)
{
    quad(x, y, x + w, y + h, SOLID_UV, SOLID_UV, SOLID_UV, SOLID_UV, argb, argb);
}

void gfx_ui_vgrad(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
    quad(x, y, x + w, y + h, SOLID_UV, SOLID_UV, SOLID_UV, SOLID_UV, top, bottom);
}

void gfx_ui_hgrad(float x, float y, float w, float h, uint32_t left, uint32_t right)
{
    float xy[8];
    uint32_t c[4];
    xy[0] = x;     xy[1] = y;
    xy[2] = x + w; xy[3] = y;
    xy[4] = x + w; xy[5] = y + h;
    xy[6] = x;     xy[7] = y + h;
    c[0] = c[3] = left;
    c[1] = c[2] = right;
    gfx_ui_quad(xy, c);
}

static int segments(float r)
{
    return r < 6 ? 12 : r < 20 ? 20 : 32;
}

void gfx_ui_circle(float cx, float cy, float r, uint32_t argb)
{
    int i, n = segments(r);
    for (i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        gfx_ui_tri(cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, argb);
    }
}

void gfx_ui_ring(float cx, float cy, float r, float thick, uint32_t argb)
{
    int i, n = segments(r);
    float ri = r - thick, xy[8];
    uint32_t c[4];
    c[0] = c[1] = c[2] = c[3] = argb;
    for (i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        xy[0] = cx + cosf(a0) * ri; xy[1] = cy + sinf(a0) * ri;
        xy[2] = cx + cosf(a0) * r;  xy[3] = cy + sinf(a0) * r;
        xy[4] = cx + cosf(a1) * r;  xy[5] = cy + sinf(a1) * r;
        xy[6] = cx + cosf(a1) * ri; xy[7] = cy + sinf(a1) * ri;
        gfx_ui_quad(xy, c);
    }
}

float gfx_ui_line(float scale)
{
    return U.font_px * scale;
}

float gfx_ui_font_line(int font, float scale)
{
    return U.line[font & 1] * scale;
}

float gfx_ui_font_width(int font, const char *s, float scale)
{
    float w = 0;
    font &= 1;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c >= FIRST_CHAR && c <= LAST_CHAR)
            w += U.glyph[font][c].adv * scale;
    }
    return w;
}

float gfx_ui_font_text(int font, float x, float y, const char *s, uint32_t top, uint32_t bottom, float scale)
{
    float x0 = x;
    font &= 1;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < FIRST_CHAR || c > LAST_CHAR)
            continue;
        if (c != ' ') {
            float gx = x - U.glyph[font][c].lead * scale;
            quad(gx, y, gx + U.glyph[font][c].w * scale, y + U.glyph[font][c].h * scale,
                 U.glyph[font][c].u0, U.glyph[font][c].v0, U.glyph[font][c].u1, U.glyph[font][c].v1, top, bottom);
        }
        x += U.glyph[font][c].adv * scale;
    }
    return x - x0;
}

float gfx_ui_text_width(const char *s, float scale)
{
    return gfx_ui_font_width(0, s, scale);
}

float gfx_ui_text(float x, float y, const char *s, uint32_t argb, float scale)
{
    return gfx_ui_font_text(0, x, y, s, argb, argb, scale);
}

int gfx_ui_build(int w, int h, const gfx_ui_vert **verts, const uint8_t **atlas, int *aw, int *ah,
                 unsigned *atlas_gen)
{
    int px, i;
    if (!U.ndraw || U.failed || w <= 0 || h <= 0)
        return 0;
    px = h / 30 < 14 ? 14 : h / 30 > 96 ? 96 : h / 30;   /* text follows the window size */
    if (px != U.font_px && !make_font(px)) {
        log_printf("ui: font failed");
        U.failed = 1;
        return 0;
    }
    U.nverts = 0;
    for (i = 0; i < U.ndraw; i++)
        U.draw[i].fn(U.draw[i].user, w, h);
    *verts = U.verts;
    *atlas = U.atlas;
    *aw = ATLAS_W;
    *ah = ATLAS_H;
    *atlas_gen = U.atlas_gen;
    return U.nverts;
}
