/* A shortcut banner on the title screen: once per launch, when the title (after
 * the intro) is up, a panel in the settings menu's style slides down from the
 * top (the one strip the title art leaves free) for a few seconds, listing the settings menu, the quick save and the modern controls
 * toggle, with the controller buttons and keys actually bound to them. For
 * whoever skipped the launcher's notes (or has no launcher at all).
 *
 * The title is told apart by the game's own state: the game coroutine's state
 * byte (G_GAME_STATE, handlers at 0x511EE5) is 2 on the PRESS START screen
 * (sub-state 3 once the logo has faded in), 4 on the NEW GAME / CONTINUE menu,
 * 5 while a game loads and runs. The card goes away early once the game starts.
 *
 * menu-side state (hints_frame) runs on the game thread, the draw callback
 * while each presented frame is composed. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "gfx_ui.h"
#include "hints.h"
#include "log.h"
#include "ui_style.h"

#define ST_TITLE       2
#define ST_TITLE_MENU  4

#define DUR 10.0
#define T_IN 0.3
#define T_OUT 0.35

static struct {
    int ready, shown;            /* shown: the card already came up this launch */
    char toggle_pad[24], toggle_key[16];
    int modern;
    volatile double start, end;  /* end: when it started going away (0 = not yet) */
    LARGE_INTEGER qpf;
} H;

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)H.qpf.QuadPart;
}

void hints_frame(void)
{
    uint8_t st, sub;
    if (!H.ready)
        return;
    st = *(volatile uint8_t *)G_GAME_STATE;
    sub = *(volatile uint8_t *)(G_GAME_STATE + 1);
    if (!H.shown) {
        if ((st == ST_TITLE && sub >= 3) || st == ST_TITLE_MENU) {
            H.shown = 1;
            H.end = 0;
            H.start = now_s();
            log_printf("hints: shortcut card on the title screen");
        }
    } else if (H.end == 0 && st != ST_TITLE && st != ST_TITLE_MENU) {
        H.end = now_s();   /* a game is starting: off it goes */
    }
}

/* one shortcut: its name over the pad buttons and the key; returns the width */
static float group(float x, float y, float u, int draw_it, const char *label, const char *pad1, const char *join,
                   const char *pad2, const char *key)
{
    float ls = 0.4f, ch = u * 0.66f, cy = y + gfx_ui_font_line(UI_FONT_DISPLAY, ls) + u * 0.1f + ch / 2;
    float lw = gfx_ui_font_width(UI_FONT_DISPLAY, label, ls), cx = x, js = 0.66f;
#define CAPW(s) (gfx_ui_font_width(UI_FONT_BODY, (s), 0.58f) + ch * 0.8f)
    float w = CAPW(pad1) + u * 0.55f + CAPW(key);
    if (pad2)
        w += u * 0.2f + gfx_ui_font_width(UI_FONT_BODY, join, js) + u * 0.2f + CAPW(pad2);
    if (!draw_it)
        return w > lw ? w : lw;
    gfx_ui_font_text(UI_FONT_DISPLAY, x, y, label, ui_col(COL_YELLOW), ui_col(COL_YELLOW_LO), ls);
    cx += ui_key_cap(cx, cy, ch, pad1);
    if (pad2) {
        cx += u * 0.2f;
        cx += ui_text(cx, cy - gfx_ui_font_line(UI_FONT_BODY, js) / 2, join, COL_DIM, js) + u * 0.2f;
        cx += ui_key_cap(cx, cy, ch, pad2);
    }
    ui_kb_cap(cx + u * 0.55f, cy, ch, key);   /* the keyboard key: a light keycap, set apart */
#undef CAPW
    return w > lw ? w : lw;
}

static void draw(void *user, int sw, int sh)
{
    double t = now_s(), age = t - H.start, end = H.end;
    float u, pad, gap, bw, pw, ph, x, y, cut, in, out, gx, w1, w2, w3 = 0, ts = 0.5f;
    char pad1[16], pad2[16];
    const char *join = NULL;
    int toggle;
    (void)user;
    (void)sh;
    if (!H.shown || age < 0 || age > DUR)
        return;
    if (end > 0 && t - end > T_OUT)
        return;
    in = ui_ease_out((float)(age / T_IN));
    out = age > DUR - T_OUT ? (float)((DUR - age) / T_OUT) : 1;
    if (end > 0 && 1 - (float)((t - end) / T_OUT) < out)
        out = 1 - (float)((t - end) / T_OUT);
    ui_alpha = in < out ? in : out;

    toggle = H.modern && H.toggle_pad[0];
    if (toggle) {
        const char *slash = strchr(H.toggle_pad, '/');
        if (slash) {
            size_t n = (size_t)(slash - H.toggle_pad);
            memcpy(pad1, H.toggle_pad, n);
            pad1[n] = 0;
            strcpy_s(pad2, sizeof pad2, slash + 1);
            join = "/";
        } else {
            strcpy_s(pad1, sizeof pad1, H.toggle_pad);
        }
    }

    u = gfx_ui_line(1.0f);
    pad = u * 0.6f;
    gap = u * 0.9f;
    cut = u * 0.55f;
    ph = u * 2.3f;
    bw = gfx_ui_font_width(UI_FONT_DISPLAY, "SHORTCUTS", ts) + u * 1.4f;   /* the blue title block */
    w1 = group(0, 0, u, 0, "SETTINGS", "Back", "+", "Start", "F1");
    w2 = group(0, 0, u, 0, "QUICK SAVE", "Back", "+", "RB", "F6");
    if (toggle)
        w3 = group(0, 0, u, 0, "MODERN CONTROLS", pad1, join, join ? pad2 : NULL, H.toggle_key);
    pw = bw + pad + w1 + gap + w2 + (toggle ? gap + w3 : 0) + pad + cut * 0.5f;
    x = (sw - pw) / 2;
    y = u * 0.7f - (1 - in) * u * 1.6f;

    ui_panel(x + u * 0.22f, y + u * 0.28f, pw, ph, cut, 0x60000000, 0x60000000);
    ui_panel(x, y, pw, ph, cut, COL_PANEL_TOP, COL_PANEL_BOT);
    {   /* title block: Mega Man blue, leaning right edge */
        float xy[8];
        uint32_t c[4];
        xy[0] = x; xy[1] = y;
        xy[2] = x + bw; xy[3] = y;
        xy[4] = x + bw - ph * 0.35f; xy[5] = y + ph;
        xy[6] = x; xy[7] = y + ph - cut;
        c[0] = ui_col(COL_BLUE); c[1] = c[2] = ui_col(COL_BLUE_DARK); c[3] = ui_col(0xFF1854B8);
        gfx_ui_quad(xy, c);
        ui_text_outlined(UI_FONT_DISPLAY, x + u * 0.5f, y + (ph - gfx_ui_font_line(UI_FONT_DISPLAY, ts)) / 2,
                         "SHORTCUTS", 0xFFFFFFFF, 0xFF9EE2FF, ts, u * 0.05f, 0xFF061226);
    }
    ui_panel_edge(x, y, pw, ph, cut, u * 0.07f, COL_EDGE);
    {
        float b = u * 0.55f, th = u * 0.1f;
        ui_line(x, y + b, x, y - th / 2, th, COL_YELLOW);
        ui_line(x - th / 2, y, x + b, y, th, COL_YELLOW);
        ui_line(x + pw, y + ph - b, x + pw, y + ph + th / 2, th, COL_YELLOW);
        ui_line(x + pw + th / 2, y + ph, x + pw - b, y + ph, th, COL_YELLOW);
    }
    {
        float gy = y + u * 0.35f;
        gx = x + bw + pad;
        group(gx, gy, u, 1, "SETTINGS", "Back", "+", "Start", "F1");
        gx += w1 + gap / 2;
        gfx_ui_rect(gx, y + u * 0.35f, 1, ph - u * 0.7f, ui_col(0x24FFFFFF));
        gx += gap / 2;
        group(gx, gy, u, 1, "QUICK SAVE", "Back", "+", "RB", "F6");
        if (toggle) {
            gx += w2 + gap / 2;
            gfx_ui_rect(gx, y + u * 0.35f, 1, ph - u * 0.7f, ui_col(0x24FFFFFF));
            gx += gap / 2;
            group(gx, gy, u, 1, "MODERN CONTROLS", pad1, join, join ? pad2 : NULL, H.toggle_key);
        }
    }
    {   /* time left */
        float left = (float)(1 - age / DUR), bx = x + bw - ph * 0.35f + u * 0.2f;
        gfx_ui_rect(bx, y + ph - u * 0.16f, (x + pw - cut - bx) * left, u * 0.06f, ui_col(0xCC5CC8FF));
    }
    ui_alpha = 1;
}

/* which controller buttons toggle modern controls ([controls] btn_*=orbit), as "L3/R3" */
static void read_bindings(const char *ini)
{
    static const struct { const char *key, *name, *def; } B[] = {
        { "btn_a", "A", "jump" }, { "btn_b", "B", "special" }, { "btn_x", "X", "roll" }, { "btn_y", "Y", "talk" },
        { "btn_lb", "LB", "aim" }, { "btn_rb", "RB", "none" }, { "btn_lt", "LT", "aim" },
        { "btn_rt", "RT", "shoot" }, { "btn_l3", "L3", "orbit" }, { "btn_r3", "R3", "orbit" },
        { "btn_back", "Back", "select" }, { "btn_start", "Start", "menu" },
    };
    char v[32], mode[32];
    int i, n = 0;
    GetPrivateProfileStringA("controls", "mode", "modern", mode, sizeof mode, ini);
    H.modern = _stricmp(mode, "vanilla") != 0;
    H.toggle_pad[0] = 0;
    for (i = 0; i < (int)(sizeof B / sizeof B[0]) && n < 2; i++) {
        GetPrivateProfileStringA("controls", B[i].key, B[i].def, v, sizeof v, ini);
        if (_stricmp(v, "orbit") == 0) {
            if (n)
                strcat_s(H.toggle_pad, sizeof H.toggle_pad, "/");
            strcat_s(H.toggle_pad, sizeof H.toggle_pad, B[i].name);
            n++;
        }
    }
    GetPrivateProfileStringA("controls", "key_orbit", "F5", v, sizeof v, ini);
    {
        char *comma = strchr(v, ',');
        if (comma)
            *comma = 0;
    }
    strcpy_s(H.toggle_key, sizeof H.toggle_key, v[0] ? v : "F5");
}

void hints_install(const char *ini)
{
    QueryPerformanceFrequency(&H.qpf);
    read_bindings(ini);
    gfx_ui_add_draw(draw, NULL);
    H.ready = 1;
}
