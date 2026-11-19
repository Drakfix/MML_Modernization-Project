/* The look shared by our own UI (the settings menu, the notices): the game's
 * HUD blues, the lock-on yellow and the energy-cell green, panels with cut
 * corners, outlined title lettering. Thin helpers over gfx_ui; every colour
 * goes through ui_col(), which applies the caller's fade (ui_alpha). */
#include <math.h>
#include <stdint.h>

#include "gfx_ui.h"
#include "padprompt.h"
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

/* a cap's whole width, round ends included */
float ui_cap_width(float h, const char *label)
{
    float w = gfx_ui_font_width(UI_FONT_BODY, label, 0.58f) + h * 1.8f;
    return w < h ? h : w;
}

/* a rounded cap with a button's or key's name, from x to x + its width (returned) */
float ui_cap(float x, float cy, float h, const char *label, uint32_t fill, uint32_t ink)
{
    float tw = gfx_ui_font_width(UI_FONT_BODY, label, 0.58f), w = ui_cap_width(h, label), r = h / 2;
    gfx_ui_rect(x + r, cy - r, w - 2 * r, h, ui_col(fill));
    gfx_ui_circle(x + r, cy, r, ui_col(fill));
    gfx_ui_circle(x + w - r, cy, r, ui_col(fill));
    ui_text(x + (w - tw) / 2, cy - gfx_ui_font_line(UI_FONT_BODY, 0.58f) / 2, label, ink, 0.58f);
    return w;
}

/* a PlayStation face button's symbol, centred on cx, cy, s across half-way */
static void ps_symbol(float cx, float cy, float s, int b, uint32_t col)
{
    float th = s * 0.3f;
    if (b == PB_A) {   /* cross */
        float d = s * 0.72f;
        ui_line(cx - d, cy - d, cx + d, cy + d, th, col);
        ui_line(cx - d, cy + d, cx + d, cy - d, th, col);
    } else if (b == PB_B) {   /* circle */
        gfx_ui_ring(cx, cy, s * 0.9f, th, ui_col(col));
    } else if (b == PB_X) {   /* square: four sides, none drawn over another (a fade stays even) */
        float e = s * 0.78f;
        gfx_ui_rect(cx - e, cy - e, e * 2, th, ui_col(col));
        gfx_ui_rect(cx - e, cy + e - th, e * 2, th, ui_col(col));
        gfx_ui_rect(cx - e, cy - e + th, th, e * 2 - th * 2, ui_col(col));
        gfx_ui_rect(cx + e - th, cy - e + th, th, e * 2 - th * 2, ui_col(col));
    } else {   /* triangle: three bands between the outline and a smaller copy inside it */
        float o[6], in[6], gx, gy, k;
        int i;
        o[0] = cx;           o[1] = cy - s * 0.95f;
        o[2] = cx + s * 1.0f; o[3] = cy + s * 0.7f;
        o[4] = cx - s * 1.0f; o[5] = cy + s * 0.7f;
        gx = (o[0] + o[2] + o[4]) / 3;
        gy = (o[1] + o[3] + o[5]) / 3;
        {   /* the inradius: twice the area over the perimeter */
            float a = fabsf((o[2] - o[0]) * (o[5] - o[1]) - (o[4] - o[0]) * (o[3] - o[1]));
            float p = sqrtf((o[2] - o[0]) * (o[2] - o[0]) + (o[3] - o[1]) * (o[3] - o[1])) +
                      sqrtf((o[4] - o[2]) * (o[4] - o[2]) + (o[5] - o[3]) * (o[5] - o[3])) +
                      sqrtf((o[0] - o[4]) * (o[0] - o[4]) + (o[1] - o[5]) * (o[1] - o[5]));
            k = 1 - th / (a / p);
        }
        for (i = 0; i < 3; i++) {
            in[2 * i] = gx + (o[2 * i] - gx) * k;
            in[2 * i + 1] = gy + (o[2 * i + 1] - gy) * k;
        }
        for (i = 0; i < 3; i++) {
            int j = (i + 1) % 3;
            float q[8];
            q[0] = o[2 * i];  q[1] = o[2 * i + 1];
            q[2] = o[2 * j];  q[3] = o[2 * j + 1];
            q[4] = in[2 * j]; q[5] = in[2 * j + 1];
            q[6] = in[2 * i]; q[7] = in[2 * i + 1];
            gfx_ui_poly(q, 4, ui_col(col));
        }
    }
}

/* a face button (PB_A .. PB_Y, by position) as each controller family shows it, so they read apart at
   a glance: Xbox letters on their colours, DualShock symbols in theirs, DualSense symbols in plain
   light grey as the PS5 draws them, and Switch letters knocked out of a white disc as the Switch
   draws them; x to x + 2r (returned) */
float ui_pad_face(float x, float cy, float r, int b)
{
    return ui_pad_face_ex(x, cy, r, b, 0.62f);
}

float ui_pad_face_ex(float x, float cy, float r, int b, float text_scale)
{
    static const uint32_t XBOX[4] = { 0xFF3FA34D, 0xFFD8443C, 0xFF2F7BE0, 0xFFE0A81C };
    static const uint32_t PS[4] = { 0xFF8FB4F0, 0xFFF07070, 0xFFE88AD0, 0xFF4FD8B0 };
    float cx = x + r;
    int f = pad_family();
    if (b < PB_A || b > PB_Y)
        return 0;
    if (f == PF_PS3 || f == PF_PS4 || f == PF_PS5) {
        gfx_ui_circle(cx, cy, r, ui_col(f == PF_PS5 ? 0xFF161C26 : 0xFF2B3A50));
        gfx_ui_ring(cx, cy, r, r * 0.16f, ui_col(f == PF_PS5 ? 0x90FFFFFF : 0x60FFFFFF));
        ps_symbol(cx, cy, r * 0.62f, b, f == PF_PS5 ? 0xFFDCE2EA : PS[b]);
    } else {
        const char *label = pad_cap(b);
        float tw = gfx_ui_font_width(UI_FONT_BODY, label, text_scale);
        int sw = f == PF_SWITCH;
        gfx_ui_circle(cx, cy, r, ui_col(sw ? 0xFFE8ECF2 : XBOX[b]));
        if (!sw)
            gfx_ui_ring(cx, cy, r, r * 0.16f, ui_col(0x60FFFFFF));
        ui_text(cx - tw / 2, cy - gfx_ui_font_line(UI_FONT_BODY, text_scale) / 2, label, sw ? 0xFF141A24 : 0xFFFFFFFF,
                text_scale);
    }
    return r * 2;
}

/* the buttons a controller marks with an icon rather than a word: a DualSense's Create and Options,
   a Switch controller's minus and plus */
enum { IC_NONE, IC_CREATE, IC_OPTIONS, IC_MINUS, IC_PLUS };

static int pad_icon(int b)
{
    int f = pad_family();
    if (b != PB_BACK && b != PB_START)
        return IC_NONE;
    if (f == PF_PS5)
        return b == PB_BACK ? IC_CREATE : IC_OPTIONS;
    if (f == PF_SWITCH)
        return b == PB_BACK ? IC_MINUS : IC_PLUS;
    return IC_NONE;
}

/* the icon, centred on cx, cy, s across half-way */
static void icon_draw(float cx, float cy, float s, int ic, uint32_t col)
{
    float th = s * 0.26f;
    switch (ic) {
    case IC_OPTIONS: {   /* three lines */
        int i;
        for (i = -1; i <= 1; i++)
            gfx_ui_rect(cx - s * 0.62f, cy + i * s * 0.5f - th / 2, s * 1.24f, th, ui_col(col));
        break;
    }
    case IC_CREATE:   /* three rays fanning up from a short stem */
        ui_line(cx, cy + s * 0.75f, cx, cy - s * 0.05f, th, col);
        ui_line(cx, cy - s * 0.3f, cx, cy - s * 0.8f, th, col);
        ui_line(cx - s * 0.25f, cy - s * 0.15f, cx - s * 0.7f, cy - s * 0.55f, th, col);
        ui_line(cx + s * 0.25f, cy - s * 0.15f, cx + s * 0.7f, cy - s * 0.55f, th, col);
        break;
    case IC_MINUS:
        gfx_ui_rect(cx - s * 0.6f, cy - th / 2, s * 1.2f, th, ui_col(col));
        break;
    case IC_PLUS:   /* (the upright drawn in two pieces, nothing drawn twice) */
        gfx_ui_rect(cx - s * 0.6f, cy - th / 2, s * 1.2f, th, ui_col(col));
        gfx_ui_rect(cx - th / 2, cy - s * 0.6f, th, s * 0.6f - th / 2, ui_col(col));
        gfx_ui_rect(cx - th / 2, cy + th / 2, th, s * 0.6f - th / 2, ui_col(col));
        break;
    }
}

/* Back / Start drawn as an icon (cx, cy its centre, r the disc's radius, 0: only the icon); 0 when
   this controller names them with a word instead */
int ui_pad_icon(float cx, float cy, float r, int b)
{
    int ic = pad_icon(b);
    if (ic == IC_NONE)
        return 0;
    if (r > 0) {
        gfx_ui_circle(cx, cy, r, ui_col(0xFF2B3A50));
        gfx_ui_ring(cx, cy, r, r * 0.12f, ui_col(0x40FFFFFF));
    }
    icon_draw(cx, cy, (r > 0 ? r : 1) * 0.62f, ic, 0xFFE6F0FA);
    return 1;
}

/* a controller button (dark cap), named as on our Xbox layout ("A", "LB", "Back"): shown as the
   controller in use names it (padprompt.c); any other label (a key) as it is */
float ui_key_cap(float x, float cy, float h, const char *label)
{
    int b = pad_from_name(label);
    if (b >= PB_A && b <= PB_Y)
        return ui_pad_face(x, cy, h / 2, b);
    if (b >= 0 && ui_pad_icon(x + h / 2, cy, h / 2, b))
        return h;
    return ui_cap(x, cy, h, b >= 0 ? pad_cap(b) : label, 0xFF2B3A50, 0xFFE6F0FA);
}

/* the width ui_key_cap takes */
float ui_key_cap_width(float h, const char *label)
{
    int b = pad_from_name(label);
    if ((b >= PB_A && b <= PB_Y) || (b >= 0 && pad_icon(b) != IC_NONE))
        return h;
    return ui_cap_width(h, b >= 0 ? pad_cap(b) : label);
}

/* a keyboard key (light keycap), when both kinds sit side by side */
float ui_kb_cap(float x, float cy, float h, const char *label)
{
    return ui_cap(x, cy, h, label, 0xFFD3DEEA, 0xFF0A1830);
}
