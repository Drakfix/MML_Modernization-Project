/* The look shared by our own UI (the settings menu, the notices): the game's
 * HUD blues, the lock-on yellow and the energy-cell green, panels with cut
 * corners, outlined title lettering. Thin helpers over gfx_ui; every colour
 * goes through ui_col(), which applies the caller's fade (ui_alpha). */
#include <math.h>
#include <stdint.h>

#include "gfx_ui.h"
#include "ui_style.h"

float ui_alpha = 1;

uint32_t ui_col(uint32_t c)
{
    uint32_t a = (uint32_t)((c >> 24) * ui_alpha + 0.5f);
    return (a << 24) | (c & 0xFFFFFF);
}

uint32_t ui_with_alpha(uint32_t c, float a)
{
    return ((uint32_t)((c >> 24) * a + 0.5f) << 24) | (c & 0xFFFFFF);
}

float ui_ease_out(float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

void ui_line(float x0, float y0, float x1, float y1, float th, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy), nx, ny, xy[8];
    uint32_t cc[4];
    if (l <= 0)
        return;
    nx = -dy / l * th * 0.5f;
    ny = dx / l * th * 0.5f;
    xy[0] = x0 + nx; xy[1] = y0 + ny;
    xy[2] = x1 + nx; xy[3] = y1 + ny;
    xy[4] = x1 - nx; xy[5] = y1 - ny;
    xy[6] = x0 - nx; xy[7] = y0 - ny;
    cc[0] = cc[1] = cc[2] = cc[3] = ui_col(c);
    gfx_ui_quad(xy, cc);
}

/* a slanted bar: the left edge leans by `slant` (positive = top to the right) */
void ui_slant(float x, float y, float w, float h, float slant, uint32_t left, uint32_t right)
{
    float xy[8];
    uint32_t c[4];
    xy[0] = x + slant; xy[1] = y;
    xy[2] = x + w + slant; xy[3] = y;
    xy[4] = x + w; xy[5] = y + h;
    xy[6] = x; xy[7] = y + h;
    c[0] = c[3] = ui_col(left);
    c[1] = c[2] = ui_col(right);
    gfx_ui_quad(xy, c);
}

uint32_t ui_lerp_col(uint32_t a, uint32_t b, float t)
{
    uint32_t r = 0;
    int s;
    for (s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        r |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return r;
}

/* panel with the top-right and bottom-left corners cut off, vertical gradient */
void ui_panel(float x, float y, float w, float h, float cut, uint32_t top, uint32_t bottom)
{
    float xy[8];
    uint32_t c[4], ct = ui_col(top), cb = ui_col(bottom);
    uint32_t c1 = ui_col(ui_lerp_col(top, bottom, cut / h)), c2 = ui_col(ui_lerp_col(top, bottom, 1 - cut / h));
    xy[0] = x; xy[1] = y; xy[2] = x + w - cut; xy[3] = y; xy[4] = x + w; xy[5] = y + cut; xy[6] = x; xy[7] = y + cut;
    c[0] = c[1] = ct; c[2] = c[3] = c1;
    gfx_ui_quad(xy, c);
    xy[0] = x; xy[1] = y + cut; xy[2] = x + w; xy[3] = y + cut; xy[4] = x + w; xy[5] = y + h - cut;
    xy[6] = x; xy[7] = y + h - cut;
    c[0] = c[1] = c1; c[2] = c[3] = c2;
    gfx_ui_quad(xy, c);
    xy[0] = x; xy[1] = y + h - cut; xy[2] = x + w; xy[3] = y + h - cut; xy[4] = x + w; xy[5] = y + h;
    xy[6] = x + cut; xy[7] = y + h;
    c[0] = c[1] = c2; c[2] = c[3] = cb;
    gfx_ui_quad(xy, c);
}

void ui_panel_edge(float x, float y, float w, float h, float cut, float th, uint32_t col)
{
    float p[12];
    int i;
    p[0] = x; p[1] = y;
    p[2] = x + w - cut; p[3] = y;
    p[4] = x + w; p[5] = y + cut;
    p[6] = x + w; p[7] = y + h;
    p[8] = x + cut; p[9] = y + h;
    p[10] = x; p[11] = y + h - cut;
    for (i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        ui_line(p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1], th, col);
    }
}

/* text with a dark outline, like the game's own title lettering */
float ui_text_outlined(int font, float x, float y, const char *s, uint32_t top, uint32_t bottom, float scale,
                       float o, uint32_t outline)
{
    static const float D[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -0.7f, -0.7f }, { 0.7f, -0.7f },
                                   { -0.7f, 0.7f }, { 0.7f, 0.7f } };
    int i;
    gfx_ui_font_text(font, x + o * 1.6f, y + o * 1.6f, s, ui_col(ui_with_alpha(outline, 0.6f)),
                     ui_col(ui_with_alpha(outline, 0.6f)), scale);
    for (i = 0; i < 8; i++)
        gfx_ui_font_text(font, x + D[i][0] * o, y + D[i][1] * o, s, ui_col(outline), ui_col(outline), scale);
    return gfx_ui_font_text(font, x, y, s, ui_col(top), ui_col(bottom), scale);
}

float ui_text(float x, float y, const char *s, uint32_t col, float scale)
{
    return gfx_ui_font_text(UI_FONT_BODY, x, y, s, ui_col(col), ui_col(col), scale);
}

void ui_arrow(float cx, float cy, float size, int dir, uint32_t col)
{
    gfx_ui_tri(cx + dir * size * 0.5f, cy, cx - dir * size * 0.5f, cy - size * 0.6f,
               cx - dir * size * 0.5f, cy + size * 0.6f, ui_col(col));
}

/* a rounded cap with a button's or key's name */
float ui_cap(float x, float cy, float h, const char *label, uint32_t fill, uint32_t ink)
{
    float tw = gfx_ui_font_width(UI_FONT_BODY, label, 0.58f), w = tw + h * 0.8f, r = h / 2;
    if (w < h)
        w = h;
    gfx_ui_rect(x + r * 0.5f, cy - r, w - r, h, ui_col(fill));
    gfx_ui_circle(x + r * 0.5f, cy, r, ui_col(fill));
    gfx_ui_circle(x + w - r * 0.5f, cy, r, ui_col(fill));
    ui_text(x + (w - tw) / 2, cy - gfx_ui_font_line(UI_FONT_BODY, 0.58f) / 2, label, ink, 0.58f);
    return w;
}

/* a controller button (dark cap) */
float ui_key_cap(float x, float cy, float h, const char *label)
{
    return ui_cap(x, cy, h, label, 0xFF2B3A50, 0xFFE6F0FA);
}

/* a keyboard key (light keycap), when both kinds sit side by side */
float ui_kb_cap(float x, float cy, float h, const char *label)
{
    return ui_cap(x, cy, h, label, 0xFFD3DEEA, 0xFF0A1830);
}
