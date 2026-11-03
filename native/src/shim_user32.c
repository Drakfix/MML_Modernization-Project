/* USER32 and GDI32 shims: the game's window, its message loop and its window procedure.
 *
 * The window is the platform's (plat.h). Its posted events (keys, clicks, MCI's notification) go
 * into our own message queue, and the game's loop takes them from there: PeekMessageA /
 * GetMessageA pump the platform first, DispatchMessageA runs the game's window procedure
 * (0x4A6A5E, or the intro's 0x4F985F while the game's own intro player subclasses the window) as
 * recompiled code. Sent events go to the procedure at once, as Windows does. Default handling was done
 * by the platform already, so DefWindowProcA has nothing left to do. The procedure acts on
 * WM_DESTROY (quit), WM_MOVE, WM_ACTIVATEAPP (pause) and WM_KEYDOWN (F9); plat_win32.c forwards
 * those and the intro's skip and end messages.
 *
 * GDI: the desktop's size and depth at startup, and three text calls for a debug display that
 * only the (disabled) dev keys turned on. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "plat.h"
#include "recomp.h"
#include "recomp_rt.h"
#include "shim.h"

static uint32_t get32(uint32_t guest)
{
    uint32_t v;
    memcpy(&v, HPTR(guest), 4);
    return v;
}

static void put32(uint32_t guest, uint32_t v)
{
    memcpy(HPTR(guest), &v, 4);
}

/* a guest string, or "#n" for a resource number */
static void res_name(uint32_t p, char *out, size_t n)
{
    if (p < 0x10000)
        _snprintf_s(out, n, _TRUNCATE, "#%u", p);
    else
        _snprintf_s(out, n, _TRUNCATE, "%s", (const char *)HPTR(p));
}

/* ---- handles the game only passes back to us ---- */
#define H_ICON   0x7E000000u   /* + index into g_icons */
#define H_CURSOR 0x7E100000u
#define H_STOCK  0x7E200000u   /* + GetStockObject's index */
#define H_DC     0x7E300000u

#define MAX_ICONS 8
static char g_icons[MAX_ICONS][64];
static int g_nicons;

/* LoadIconA(instance, name) */
static uint32_t sh_load_icon_a(const uint32_t *a)
{
    if (g_nicons >= MAX_ICONS)
        return 0;
    res_name(a[1], g_icons[g_nicons], sizeof g_icons[0]);
    return H_ICON + (uint32_t)g_nicons++;
}

/* LoadCursorA(instance, name) */
static uint32_t sh_load_cursor_a(const uint32_t *a)
{
    return H_CURSOR + (a[1] & 0xFFFF);
}

static uint32_t sh_get_stock_object(const uint32_t *a)
{
    return H_STOCK + (a[0] & 0xFF);
}

/* ---- window classes and windows ---- */
#define MAX_CLASSES 4
static struct {
    char name[64];
    char icon[64];
    uint32_t proc;
} g_cls[MAX_CLASSES];
static int g_ncls;

#define MAX_WINDOWS 4
static struct {
    uint32_t win;
    uint32_t proc;   /* the game's window procedure (a guest address) */
} g_win[MAX_WINDOWS];

/* RegisterClassA(const WNDCLASSA *): lpfnWndProc +4, hIcon +20, lpszClassName +36 */
static uint32_t sh_register_class_a(const uint32_t *a)
{
    uint32_t wc = a[0], icon = get32(wc + 20);
    if (g_ncls >= MAX_CLASSES) {
        shim_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    res_name(get32(wc + 36), g_cls[g_ncls].name, sizeof g_cls[0].name);
    g_cls[g_ncls].proc = get32(wc + 4);
    g_cls[g_ncls].icon[0] = 0;
    if (icon >= H_ICON && icon < H_ICON + MAX_ICONS)
        strcpy_s(g_cls[g_ncls].icon, sizeof g_cls[0].icon, g_icons[icon - H_ICON]);
    log_printf("shims: window class %s, procedure %08X", g_cls[g_ncls].name, g_cls[g_ncls].proc);
    g_ncls++;
    return 0xC000u + (uint32_t)g_ncls;   /* the atom */
}

static int win_index(uint32_t win)
{
    int k;
    for (k = 0; k < MAX_WINDOWS; k++)
        if (g_win[k].win == win)
            return k;
    return -1;
}

/* ---- the message queue ---- */
#define QSIZE 256u
static struct {
    uint32_t win, msg, wparam, lparam, time;
} g_q[QSIZE];
static unsigned g_qhead, g_qtail;

static void post(uint32_t win, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    if (g_qtail - g_qhead >= QSIZE) {
        static int warned;
        if (!warned++)
            log_printf("shims: message queue full, %04X dropped", msg);
        return;
    }
    g_q[g_qtail % QSIZE].win = win;
    g_q[g_qtail % QSIZE].msg = msg;
    g_q[g_qtail % QSIZE].wparam = wparam;
    g_q[g_qtail % QSIZE].lparam = lparam;
    g_q[g_qtail % QSIZE].time = plat_ticks_ms();
    g_qtail++;
}

static int g_creating = -1;   /* the window CreateWindowExA is making (events before it returns) */

/* A sent event goes to the procedure now, as Windows does: they arrive while a shim runs
   (ShowWindow, PeekMessageA, SetWindowPos...). Delivered later through the queue, a
   WM_ACTIVATEAPP from ShowWindow reached the intro's procedure instead of the main one, and its
   "resume" to MCI cancelled the end-of-video notification. Only an event that comes from our own
   code's Windows calls outside the game's has to wait in the queue. */
static void on_event(uint32_t win, uint32_t msg, uint32_t wparam, uint32_t lparam, int sent)
{
    int k = win_index(win);
    if (k < 0 && g_creating >= 0) {
        k = g_creating;
        g_win[k].win = win;
    }
    if (sent && k >= 0 && g_win[k].proc && recomp_in_shim()) {
        uint32_t args[4];
        args[0] = win, args[1] = msg, args[2] = wparam, args[3] = lparam;
        recomp_call_stdcall(g_win[k].proc, 4, args);
        return;
    }
    post(win, msg, wparam, lparam);
}

static void pump(void)
{
    static int quit_posted;
    if (!plat_pump() && !quit_posted) {
        quit_posted = 1;
        post(0, WM_QUIT, 0, 0);
    }
}

/* the front message into the game's MSG (hwnd, message, wParam, lParam, time, pt) */
static void take(uint32_t msg_ptr, int remove)
{
    uint32_t m[7];
    m[0] = g_q[g_qhead % QSIZE].win;
    m[1] = g_q[g_qhead % QSIZE].msg;
    m[2] = g_q[g_qhead % QSIZE].wparam;
    m[3] = g_q[g_qhead % QSIZE].lparam;
    m[4] = g_q[g_qhead % QSIZE].time;
    m[5] = m[6] = 0;
    memcpy(HPTR(msg_ptr), m, sizeof m);
    if (remove)
        g_qhead++;
}

static void check_filter(const uint32_t *a, const char *who)
{
    static int warned;
    if ((a[1] || a[2] || a[3]) && !warned++)
        log_printf("shims: %s with a filter (%08X %X-%X), ignored", who, a[1], a[2], a[3]);
}

/* PeekMessageA(msg, hwnd, min, max, flags) */
static uint32_t sh_peek_message_a(const uint32_t *a)
{
    check_filter(a, "PeekMessageA");
    pump();
    if (g_qhead == g_qtail)
        return 0;
    take(a[0], a[4] & PM_REMOVE);
    return 1;
}

/* GetMessageA(msg, hwnd, min, max): waits; 0 for WM_QUIT */
static uint32_t sh_get_message_a(const uint32_t *a)
{
    check_filter(a, "GetMessageA");
    for (;;) {
        pump();
        if (g_qhead != g_qtail) {
            uint32_t msg = g_q[g_qhead % QSIZE].msg;
            take(a[0], 1);
            return msg != WM_QUIT;
        }
        plat_wait();
    }
}

static uint32_t sh_translate_message(const uint32_t *a)
{
    (void)a;
    return 0;   /* the game's procedures take no WM_CHAR */
}

/* DispatchMessageA(msg): the game's window procedure, as recompiled code */
static uint32_t sh_dispatch_message_a(const uint32_t *a)
{
    uint32_t args[4];
    int k;
    memcpy(args, HPTR(a[0]), sizeof args);   /* hwnd, message, wParam, lParam */
    k = win_index(args[0]);
    if (!args[0] || k < 0 || !g_win[k].proc)
        return 0;
    return recomp_call_stdcall(g_win[k].proc, 4, args);
}

static uint32_t sh_def_window_proc_a(const uint32_t *a)
{
    (void)a;
    return 0;   /* the platform did the default handling when the event arrived */
}

static uint32_t sh_post_quit_message(const uint32_t *a)
{
    post(0, WM_QUIT, a[0], 0);
    return 0;
}

/* CreateWindowExA(exstyle, class, title, style, x, y, w, h, parent, menu, instance, param) */
static uint32_t sh_create_window_ex_a(const uint32_t *a)
{
    char cls[64];
    int c, k = win_index(0);
    uint32_t win;
    res_name(a[1], cls, sizeof cls);
    for (c = 0; c < g_ncls; c++)
        if (_stricmp(g_cls[c].name, cls) == 0 || a[1] == 0xC001u + (uint32_t)c)
            break;
    if (c == g_ncls || k < 0) {
        log_printf("shims: CreateWindowExA: no class %s", cls);
        shim_set_error(ERROR_CANNOT_FIND_WND_CLASS);
        return 0;
    }
    g_win[k].proc = g_cls[c].proc;
    g_creating = k;
    win = plat_window_create(g_cls[c].name, a[2] ? (const char *)HPTR(a[2]) : "", a[3], a[0], (int)a[4], (int)a[5],
                             (int)a[6], (int)a[7], g_cls[c].icon[0] ? g_cls[c].icon : NULL, on_event);
    g_creating = -1;
    g_win[k].win = win;
    if (!win)
        g_win[k].proc = 0;
    else
        log_printf("shims: window %08X (%s)", win, g_cls[c].name);
    return win;
}

static uint32_t sh_show_window(const uint32_t *a)
{
    plat_window_show(a[0], (int)a[1]);
    return 0;
}

static uint32_t sh_update_window(const uint32_t *a)
{
    plat_window_update(a[0]);
    return 1;
}

/* SetWindowPos(hwnd, after, x, y, w, h, flags) */
static uint32_t sh_set_window_pos(const uint32_t *a)
{
    plat_window_set_pos(a[0], a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5], a[6]);
    return 1;
}

/* SetWindowLongA(hwnd, index, value): GWL_WNDPROC swaps the game's procedure (the intro's
   subclass); the rest is the platform's */
static uint32_t sh_set_window_long_a(const uint32_t *a)
{
    if ((int32_t)a[1] == GWL_WNDPROC) {
        int k = win_index(a[0]);
        uint32_t old;
        if (k < 0)
            return 0;
        old = g_win[k].proc;
        g_win[k].proc = a[2];
        return old;
    }
    return plat_window_set_long(a[0], (int)a[1], a[2]);
}

/* AdjustWindowRect(rect, style, menu) */
static uint32_t sh_adjust_window_rect(const uint32_t *a)
{
    int32_t r[4];
    memcpy(r, HPTR(a[0]), sizeof r);
    plat_adjust_rect(r, a[1], 0, (int)a[2]);
    memcpy(HPTR(a[0]), r, sizeof r);
    return 1;
}

static uint32_t sh_set_focus(const uint32_t *a)
{
    return plat_window_focus(a[0]);
}

static uint32_t sh_show_cursor(const uint32_t *a)
{
    return (uint32_t)plat_show_cursor(a[0] != 0);
}

/* MessageBoxA(owner, text, title, type) */
static uint32_t sh_message_box_a(const uint32_t *a)
{
    const char *text = a[1] ? (const char *)HPTR(a[1]) : "", *title = a[2] ? (const char *)HPTR(a[2]) : "";
    log_printf("shims: message box \"%s\": %s", title, text);
    plat_message_box(text, title);
    return IDOK;
}

/* IntersectRect(dst, a, b) */
static uint32_t sh_intersect_rect(const uint32_t *a)
{
    int32_t r1[4], r2[4], d[4];
    memcpy(r1, HPTR(a[1]), sizeof r1);
    memcpy(r2, HPTR(a[2]), sizeof r2);
    d[0] = r1[0] > r2[0] ? r1[0] : r2[0];
    d[1] = r1[1] > r2[1] ? r1[1] : r2[1];
    d[2] = r1[2] < r2[2] ? r1[2] : r2[2];
    d[3] = r1[3] < r2[3] ? r1[3] : r2[3];
    if (d[0] >= d[2] || d[1] >= d[3] || r1[0] >= r1[2] || r1[1] >= r1[3] || r2[0] >= r2[2] || r2[1] >= r2[3]) {
        memset(d, 0, sizeof d);
        memcpy(HPTR(a[0]), d, sizeof d);
        return 0;
    }
    memcpy(HPTR(a[0]), d, sizeof d);
    return 1;
}

/* CharUpperBuffA(text, length): code page 1252 */
static uint32_t sh_char_upper_buff_a(const uint32_t *a)
{
    uint8_t *p = HPTR(a[0]);
    uint32_t k;
    for (k = 0; k < a[1]; k++) {
        uint8_t ch = p[k];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7))
            p[k] = (uint8_t)(ch - 0x20);
        else if (ch == 0x9A || ch == 0x9C || ch == 0x9E)   /* s, oe, z with marks */
            p[k] = (uint8_t)(ch - 0x10);
        else if (ch == 0xFF)
            p[k] = 0x9F;
    }
    return a[1];
}

/* ---- GDI ---- */
static uint32_t sh_get_dc(const uint32_t *a)
{
    (void)a;
    return H_DC;
}

static uint32_t sh_release_dc(const uint32_t *a)
{
    (void)a;
    return 1;
}

/* GetDeviceCaps(dc, index): what the game asks of the desktop */
static uint32_t sh_get_device_caps(const uint32_t *a)
{
    int w, h;
    switch (a[1]) {
    case HORZRES:
    case VERTRES:
        plat_desktop_size(&w, &h);
        return (uint32_t)(a[1] == HORZRES ? w : h);
    case BITSPIXEL:
        return 32;
    case PLANES:
        return 1;
    }
    log_printf("shims: GetDeviceCaps(%u): not known", a[1]);
    return 0;
}

/* SetBkMode, SetTextColor, TextOutA: the dev keys' debug text */
static uint32_t sh_gdi_text(const uint32_t *a)
{
    static int warned;
    (void)a;
    if (!warned++)
        log_printf("shims: GDI text output (debug display) isn't drawn");
    return 1;
}

void shim_user32_install(void)
{
    static const struct { const char *dll, *name; int nargs; shim_fn fn; } list[] = {
        { "USER32.dll", "LoadIconA", 2, sh_load_icon_a },
        { "USER32.dll", "LoadCursorA", 2, sh_load_cursor_a },
        { "USER32.dll", "RegisterClassA", 1, sh_register_class_a },
        { "USER32.dll", "CreateWindowExA", 12, sh_create_window_ex_a },
        { "USER32.dll", "ShowWindow", 2, sh_show_window },
        { "USER32.dll", "UpdateWindow", 1, sh_update_window },
        { "USER32.dll", "SetWindowPos", 7, sh_set_window_pos },
        { "USER32.dll", "SetWindowLongA", 3, sh_set_window_long_a },
        { "USER32.dll", "AdjustWindowRect", 3, sh_adjust_window_rect },
        { "USER32.dll", "SetFocus", 1, sh_set_focus },
        { "USER32.dll", "ShowCursor", 1, sh_show_cursor },
        { "USER32.dll", "PeekMessageA", 5, sh_peek_message_a },
        { "USER32.dll", "GetMessageA", 4, sh_get_message_a },
        { "USER32.dll", "TranslateMessage", 1, sh_translate_message },
        { "USER32.dll", "DispatchMessageA", 1, sh_dispatch_message_a },
        { "USER32.dll", "DefWindowProcA", 4, sh_def_window_proc_a },
        { "USER32.dll", "PostQuitMessage", 1, sh_post_quit_message },
        { "USER32.dll", "MessageBoxA", 4, sh_message_box_a },
        { "USER32.dll", "IntersectRect", 3, sh_intersect_rect },
        { "USER32.dll", "CharUpperBuffA", 2, sh_char_upper_buff_a },
        { "USER32.dll", "GetDC", 1, sh_get_dc },
        { "USER32.dll", "ReleaseDC", 2, sh_release_dc },
        { "GDI32.dll", "GetDeviceCaps", 2, sh_get_device_caps },
        { "GDI32.dll", "GetStockObject", 1, sh_get_stock_object },
        { "GDI32.dll", "SetBkMode", 2, sh_gdi_text },
        { "GDI32.dll", "SetTextColor", 2, sh_gdi_text },
        { "GDI32.dll", "TextOutA", 5, sh_gdi_text },
    };
    size_t k;
    for (k = 0; k < sizeof list / sizeof list[0]; k++)
        shim_add(list[k].dll, list[k].name, list[k].nargs, list[k].fn);
}
