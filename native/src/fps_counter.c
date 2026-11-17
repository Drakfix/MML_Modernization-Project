/* On-screen frame counter: see fps_counter.h. Phone overlays and driver counters count what they
 * see (Xiaomi's sometimes reads 60 at 120 Hz); this counts the frames the game hands to the screen,
 * in-between frames included, as they are presented. */
#include <stdio.h>

#include <windows.h>   /* (native/compat elsewhere) */

#include "fps_counter.h"
#include "gfx_ui.h"
#include "ui_style.h"

#define FPS_PERIOD 0.5   /* seconds per reading */
#define FPS_STALE  2.0   /* longer without a reading (paused, loading): start a fresh one */

static volatile LONG g_presents;
static int g_on, g_installed, g_shown = -1;
static LONG g_n0;
static double g_t0;
static LARGE_INTEGER g_qpf;

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_qpf.QuadPart;
}

void fps_counter_present(void)
{
    InterlockedIncrement(&g_presents);
}

/* (UI thread: the game thread's snapshot, once per presented frame) */
static void draw(void *user, int sw, int sh)
{
    double t;
    LONG n;
    char s[24];
    float u, sc = 0.42f, tw, lh, pad, x, y, w, h;
    (void)user;
    (void)sh;
    if (!g_on)
        return;
    t = now_s();
    n = g_presents;
    if (g_t0 <= 0.0 || t - g_t0 > FPS_STALE) {
        g_t0 = t;
        g_n0 = n;
    } else if (t - g_t0 >= FPS_PERIOD) {
        g_shown = (int)((double)(n - g_n0) / (t - g_t0) + 0.5);
        g_t0 = t;
        g_n0 = n;
    }
    if (g_shown < 0)
        return;
    snprintf(s, sizeof s, "%d FPS", g_shown);
    ui_alpha = 1;
    u = gfx_ui_line(1.0f);
    tw = gfx_ui_font_width(UI_FONT_DISPLAY, s, sc);
    lh = gfx_ui_font_line(UI_FONT_DISPLAY, sc);
    pad = u * 0.3f;
    w = tw + pad * 2;
    h = lh + pad * 0.6f;
    x = sw - w - u * 0.5f;
    y = u * 0.4f;
    ui_panel(x, y, w, h, h * 0.3f, COL_PANEL_TOP, COL_PANEL_BOT);
    ui_text_outlined(UI_FONT_DISPLAY, x + pad, y + (h - lh) / 2, s, 0xFFFFFFFF, 0xFF9EE2FF, sc, u * 0.04f,
                     0xFF061226);
}

void fps_counter_set(int on)
{
    g_on = on;
    g_t0 = 0.0;
    g_shown = -1;   /* (a new reading, not one from before it was hidden) */
}

void fps_counter_install(int on)
{
    if (!g_installed) {
        QueryPerformanceFrequency(&g_qpf);
        gfx_ui_add_draw(draw, NULL);
        g_installed = 1;
    }
    fps_counter_set(on);
}
