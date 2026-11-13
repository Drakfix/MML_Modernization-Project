/* The title screen's shortcuts.
 *
 * A card: once per launch, when the title (after the intro) is up, a panel in
 * the settings menu's style slides down from the top (the one strip the title
 * art leaves free) for a few seconds, listing the settings menu, the quick
 * save, the way back to the start menu and the modern controls toggle, with
 * the controller buttons and keys actually bound to them ([general]
 * title_hints=0: no card).
 *
 * Back to the start menu: Back held alone (or F2) for 1.5 s on the title
 * screen, with a ring filling up while it's held, closes the game and opens
 * the start menu again ([general] menu_once=1 for it: it shows even when it is
 * set to be skipped).
 *
 * An update notice: with the start menu set to Skip (so its own check never runs) and [general]
 * check_updates on, the game asks GitHub for the newest release in the background (updater.h);
 * when one is out, a panel on the title screen says so, and holding Back / F2 opens the start
 * menu on its Update row ([general] menu_once=2).
 *
 * The title is told apart by the game's own state: the game coroutine's state
 * byte (G_GAME_STATE, handlers at 0x511EE5) is 2 on the PRESS START screen
 * (sub-state 3 once the logo has faded in), 4 on the NEW GAME / CONTINUE menu,
 * 5 while a game loads and runs. The card goes away early once the game starts.
 *
 * menu-side state (hints_frame) runs on the game thread, the draw callback
 * while each presented frame is composed. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "gfx_ui.h"
#include "guest.h"
#include "hints.h"
#include "input.h"
#include "kbm.h"
#include "log.h"
#include "menu.h"
#include "plat.h"
#include "toast.h"
#include "ui_style.h"
#include "updater.h"

#define ST_TITLE       2
#define ST_TITLE_MENU  4

#define DUR 10.0
#define T_IN 0.3
#define T_OUT 0.35
#define HOLD_S 1.5      /* Back / F2 held this long: the start menu */
#define HOLD_SHOW 0.15  /* (the ring shows after this: a tap shows nothing) */
#define MENU_KEY VK_F2

static struct {
    int ready, shown;            /* shown: the card already came up this launch */
    int card;                    /* [general] title_hints */
    char toggle_pad[24], toggle_key[16];
    int modern;
    volatile double start, end;  /* end: when it started going away (0 = not yet) */
    volatile double hold_since;  /* Back / F2 held on the title since (0 = not held) */
    volatile int leaving;        /* the start menu is on its way */
    volatile int hold_pad;       /* held on the controller (Back), else the keyboard (F2) */
    int wait_release;            /* (after a failed try: let go first) */
    char ini[MAX_PATH], home[MAX_PATH];
    int upd_polling, upd_frames;  /* the update check runs (polled every so often) */
    volatile int upd_avail;      /* a newer release is out: upd_ver */
    char upd_ver[48];
    volatile int on_title;       /* (for the draw side) */
    double upd_since;            /* when the notice first showed */
    LARGE_INTEGER qpf;
} H;

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)H.qpf.QuadPart;
}

/* the start menu again: this program goes, the menu comes */
static void to_start_menu(void)
{
    H.leaving = 1;
    log_printf("hints: Back / F2 held on the title screen: to the start menu");
    /* (shown even when set to Skip; 2: on its Update row) */
    WritePrivateProfileStringA("general", "menu_once", H.upd_avail ? "2" : "1", H.ini);
    if (plat_relaunch_menu(H.home))
        return;
    WritePrivateProfileStringA("general", "menu_once", NULL, H.ini);
    H.leaving = 0;
    H.hold_since = 0;
    H.wait_release = 1;
    toast_post(TOAST_ICON_WARN, TOAST_BAD, "NO START MENU", NULL, "It couldn't be started: see the log");
}

void hints_frame(void)
{
    uint8_t st, sub;
    int title, held;
    if (!H.ready)
        return;
    st = GVAR(uint8_t, G_GAME_STATE);
    sub = GVAR(uint8_t, G_GAME_STATE + 1);
    title = (st == ST_TITLE && sub >= 3) || st == ST_TITLE_MENU;
    H.on_title = title;
    if (H.upd_polling && ++H.upd_frames >= 30) {   /* (twice a second is plenty) */
        char msg[200], ver[48];
        float progress;
        int us = update_poll(msg, sizeof msg, ver, sizeof ver, &progress);
        H.upd_frames = 0;
        if (us == UPD_AVAILABLE) {
            strcpy_s(H.upd_ver, sizeof H.upd_ver, ver);
            H.upd_avail = 1;
            log_printf("hints: version %s is out: the title screen says so", ver);
        }
        if (us != UPD_CHECKING && us != UPD_IDLE)
            H.upd_polling = 0;
    }
    /* Back alone (no chord: Back + Start is the settings, Back + RB the quick save), or F2 */
    held = title && !H.leaving && !menu_blocks_input() &&
           (menu_pad_buttons() == BTN_BACK || kbm_key(MENU_KEY));
    if (!held) {
        H.hold_since = 0;
        H.wait_release = 0;
    } else if (!H.wait_release) {
        double t = now_s();
        if (H.hold_since == 0) {
            H.hold_pad = menu_pad_buttons() == BTN_BACK;
            H.hold_since = t;
        }
        else if (t - H.hold_since >= HOLD_S)
            to_start_menu();
    }
    if (!H.card)
        return;
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
#define CAPW(s) ui_cap_width(ch, (s))
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

/* the ring that fills while Back / F2 is held, and what it does */
static void arc(float cx, float cy, float r, float th, float f, uint32_t col)
{
    int n = (int)(72 * f) + 1, i;
    uint32_t c[4];
    c[0] = c[1] = c[2] = c[3] = ui_col(col);
    for (i = 0; i < n; i++) {
        float a0 = -1.5707963f + 6.2831853f * f * i / n, a1 = -1.5707963f + 6.2831853f * f * (i + 1) / n;
        float ri = r - th / 2, ro = r + th / 2, xy[8];
        xy[0] = cx + cosf(a0) * ri; xy[1] = cy + sinf(a0) * ri;
        xy[2] = cx + cosf(a0) * ro; xy[3] = cy + sinf(a0) * ro;
        xy[4] = cx + cosf(a1) * ro; xy[5] = cy + sinf(a1) * ro;
        xy[6] = cx + cosf(a1) * ri; xy[7] = cy + sinf(a1) * ri;
        gfx_ui_quad(xy, c);
    }
}

static void draw_hold(int sw, int sh)
{
    double since = H.hold_since, held = since > 0 ? now_s() - since : 0;
    float u, f, r, pw, ph, x, y, cut, ts = 0.5f, ds = 0.66f, tx;
    const char *title = "START MENU", *detail = H.leaving ? "Opening..." : "Keep holding";
    if (!H.leaving && held < HOLD_SHOW)
        return;
    f = H.leaving ? 1 : (float)(held / HOLD_S);
    if (f > 1)
        f = 1;
    u = gfx_ui_line(1.0f);
    ui_alpha = ui_ease_out((float)((held - HOLD_SHOW) / 0.15));
    if (H.leaving)
        ui_alpha = 1;
    r = u * 1.0f;
    cut = u * 0.5f;
    ph = u * 2.9f;
    pw = r * 2 + u * 1.3f + gfx_ui_font_width(UI_FONT_DISPLAY, title, ts) + u * 0.9f;
    x = sw - pw - u * 1.2f;   /* (the bottom right corner: clear of the title's copyright line) */
    y = sh - ph - u * 0.9f;
    ui_panel(x + u * 0.2f, y + u * 0.25f, pw, ph, cut, 0x60000000, 0x60000000);
    ui_panel(x, y, pw, ph, cut, COL_PANEL_TOP, COL_PANEL_BOT);
    ui_panel_edge(x, y, pw, ph, cut, u * 0.07f, COL_EDGE);
    gfx_ui_ring(x + u * 0.6f + r, y + ph / 2, r, u * 0.2f, ui_col(0xFF1C2C44));
    arc(x + u * 0.6f + r, y + ph / 2, r, u * 0.2f, f, COL_YELLOW);
    {   /* what is held, inside the ring: the controller's Back, or the F2 key */
        float cx = x + u * 0.6f + r, cy = y + ph / 2, room = (r - u * 0.1f) * 2 * 0.8f;
        if (H.hold_pad) {
            float bs = 0.6f, bw = gfx_ui_font_width(UI_FONT_BODY, "Back", bs);
            if (bw > room) {
                bs *= room / bw;
                bw = room;
            }
            gfx_ui_circle(cx, cy, r - u * 0.16f, ui_col(0xFF2B3A50));
            ui_text(cx - bw / 2, cy - gfx_ui_font_line(UI_FONT_BODY, bs) / 2, "Back", 0xFFE6F0FA, bs);
        } else {
            float kh = u * 0.62f;
            ui_kb_cap(cx - ui_cap_width(kh, "F2") / 2, cy, kh, "F2");
        }
    }
    tx = x + u * 0.6f + r * 2 + u * 0.6f;
    gfx_ui_font_text(UI_FONT_DISPLAY, tx, y + ph / 2 - gfx_ui_font_line(UI_FONT_DISPLAY, ts) + u * 0.05f, title,
                     ui_col(COL_YELLOW), ui_col(COL_YELLOW_LO), ts);
    ui_text(tx, y + ph / 2 + u * 0.1f, detail, COL_DIM, ds);
    ui_alpha = 1;
}

/* a newer release is out: one row along the bottom left of the title screen (under the copyright
   line), how to get to it */
static void draw_update(int sw, int sh)
{
    float u, x, y, pw, ph, cut, ts = 0.42f, bs = 0.62f, kh, tx, cy, fade, bl;
    char head[64];
    double t = now_s();
    (void)sw;
    if (!H.upd_avail || !H.on_title || H.leaving)
        return;
    if (H.upd_since == 0)
        H.upd_since = t;
    fade = ui_ease_out((float)((t - H.upd_since) / 0.3));
    u = gfx_ui_line(1.0f);
    kh = u * 0.56f;
    cut = u * 0.4f;
    ph = u * 1.45f;
    bl = gfx_ui_font_line(UI_FONT_BODY, bs);
    _snprintf_s(head, sizeof head, _TRUNCATE, "UPDATE %s", H.upd_ver);
    pw = u * 1.3f + gfx_ui_font_width(UI_FONT_DISPLAY, head, ts) + u * 0.4f +
         gfx_ui_font_width(UI_FONT_BODY, "is out. Hold", bs) + u * 0.25f + ui_cap_width(kh, "Back") +
#ifndef __ANDROID__
         u * 0.25f + gfx_ui_font_width(UI_FONT_BODY, "or", bs) + u * 0.25f + ui_cap_width(kh, "F2") +
#endif
         u * 0.25f + gfx_ui_font_width(UI_FONT_BODY, "for the start menu", bs);
    x = u * 1.2f - (1 - fade) * u * 1.5f;
    y = sh - ph - u * 0.3f;
    cy = y + ph / 2;
    ui_alpha = fade;
    ui_panel(x + u * 0.15f, y + u * 0.2f, pw, ph, cut, 0x60000000, 0x60000000);
    ui_panel(x, y, pw, ph, cut, COL_PANEL_TOP, COL_PANEL_BOT);
    ui_panel_edge(x, y, pw, ph, cut, u * 0.06f, COL_EDGE);
    gfx_ui_rect(x, y + u * 0.25f, u * 0.1f, ph - u * 0.5f, ui_col(COL_YELLOW));
    tx = x + u * 0.6f;
    tx += gfx_ui_font_text(UI_FONT_DISPLAY, tx, cy - gfx_ui_font_line(UI_FONT_DISPLAY, ts) / 2, head,
                           ui_col(COL_YELLOW), ui_col(COL_YELLOW_LO), ts) + u * 0.4f;
    tx += ui_text(tx, cy - bl / 2, "is out. Hold", COL_TEXT, bs) + u * 0.25f;
    tx += ui_key_cap(tx, cy, kh, "Back") + u * 0.25f;
#ifndef __ANDROID__
    tx += ui_text(tx, cy - bl / 2, "or", COL_DIM, bs) + u * 0.25f;
    tx += ui_kb_cap(tx, cy, kh, "F2") + u * 0.25f;
#endif
    ui_text(tx, cy - bl / 2, "for the start menu", COL_TEXT, bs);
    ui_alpha = 1;
}

static void draw_card(int sw, int sh);

static void draw(void *user, int sw, int sh)
{
    (void)user;
    if (H.card)
        draw_card(sw, sh);
    draw_update(sw, sh);
    draw_hold(sw, sh);
}

static void draw_card(int sw, int sh)
{
    double t = now_s(), age = t - H.start, end = H.end;
    float u, pad, gap, bw, pw, ph, x, y, cut, in, out, gx, w1, w2, w3 = 0, w4, ts = 0.5f;
    char pad1[16], pad2[16];
    const char *join = NULL;
    int toggle;
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
    w4 = group(0, 0, u, 0, "START MENU (HOLD)", "Back", NULL, NULL, "F2");
    if (toggle)
        w3 = group(0, 0, u, 0, "MODERN CONTROLS", pad1, join, join ? pad2 : NULL, H.toggle_key);
    pw = bw + pad + w1 + gap + w2 + gap + w4 + (toggle ? gap + w3 : 0) + pad + cut * 0.5f;
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
        gx += w2 + gap / 2;
        gfx_ui_rect(gx, y + u * 0.35f, 1, ph - u * 0.7f, ui_col(0x24FFFFFF));
        gx += gap / 2;
        group(gx, gy, u, 1, "START MENU (HOLD)", "Back", NULL, NULL, "F2");
        if (toggle) {
            gx += w4 + gap / 2;
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
        { "btn_lb", "LB", "aim" }, { "btn_rb", "RB", "skates" }, { "btn_lt", "LT", "aim" },
        { "btn_rt", "RT", "shoot" }, { "btn_l3", "L3", "orbit" }, { "btn_r3", "R3", "shoulder" },
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
    char *slash;
    QueryPerformanceFrequency(&H.qpf);
    read_bindings(ini);
    H.card = GetPrivateProfileIntA("general", "title_hints", 1, ini) != 0;
    strcpy_s(H.ini, sizeof H.ini, ini);
    strcpy_s(H.home, sizeof H.home, ini);   /* (the folder with mml.ini: the app's, with the start menu) */
    slash = strrchr(H.home, '\\');
    if (slash)
        *slash = 0;
    /* the start menu set to Skip doesn't look for updates: the game does, for the title screen */
    if (update_can() && GetPrivateProfileIntA("general", "check_updates", 1, ini) &&
        !GetPrivateProfileIntA("general", "start_menu", 1, ini)) {
        update_check_begin();
        H.upd_polling = 1;
        log_printf("hints: the start menu is skipped: the game looks for updates");
    }
    gfx_ui_add_draw(draw, NULL);
    H.ready = 1;
}
