/* plat.h on Windows. The game's window is ours: our window procedure does Windows' default
 * handling (with the few keys and system commands the game's own procedure kept from it) and
 * hands the messages the game's procedure acts on to the shims' queue (shim_user32.c), which
 * gives them to the game's procedure as recompiled code. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "plat.h"

#define MAX_WINDOWS 8
static struct {
    HWND hwnd;
    plat_event_fn fn;
    int closing;
} g_win[MAX_WINDOWS];
static plat_event_fn g_creating;   /* messages that arrive while CreateWindowEx runs */
static int g_quit;

uint32_t plat_ticks_ms(void)
{
    return GetTickCount();
}

void plat_sleep_ms(unsigned ms)
{
    Sleep(ms);
}

long plat_atomic_xchg(volatile long *p, long v)
{
    return InterlockedExchange(p, v);
}

void plat_desktop_size(int *w, int *h)
{
    HDC dc = GetDC(NULL);
    *w = GetDeviceCaps(dc, HORZRES);
    *h = GetDeviceCaps(dc, VERTRES);
    ReleaseDC(NULL, dc);
}

void plat_message_box(const char *text, const char *title)
{
    MessageBoxA(NULL, text, title, MB_OK | MB_ICONERROR);
}

static int win_index(HWND w)
{
    int k;
    for (k = 0; k < MAX_WINDOWS; k++)
        if (g_win[k].hwnd == w)
            return k;
    return -1;
}

static LRESULT CALLBACK host_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    int k = win_index(w);
    plat_event_fn fn;
    if (k < 0 && g_creating && (k = win_index(NULL)) >= 0) {
        g_win[k].hwnd = w;
        g_win[k].fn = g_creating;
        g_win[k].closing = 0;
    }
    fn = k >= 0 ? g_win[k].fn : NULL;
    switch (msg) {
    case WM_CLOSE:
        /* the game quits on WM_DESTROY; the window stays until it has (the renderer draws to it) */
        if (fn && !g_win[k].closing) {
            g_win[k].closing = 1;
            fn((uint32_t)(uintptr_t)w, WM_DESTROY, 0, 0, 1);
        }
        return 0;
    case WM_DESTROY:
        if (fn && !g_win[k].closing)
            fn((uint32_t)(uintptr_t)w, WM_DESTROY, 0, 0, 1);
        if (k >= 0)
            g_win[k].hwnd = NULL;
        break;
    case WM_MOVE:
    case WM_ACTIVATEAPP:
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case MM_MCINOTIFY:   /* the game's own intro player (when ours doesn't play) */
        if (fn)
            fn((uint32_t)(uintptr_t)w, msg, (uint32_t)wp, (uint32_t)lp, msg == WM_MOVE || msg == WM_ACTIVATEAPP);
        break;
    case WM_SYSKEYDOWN:   /* the game's procedure keeps F10 and Alt from opening the system menu */
        if (wp == VK_F10 || wp == VK_MENU)
            return 0;
        break;
    case WM_SYSCOMMAND:   /* ... and the screen saver and monitor power-off away */
        if ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER)
            return 0;
        break;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

uint32_t plat_window_create(const char *cls, const char *title, uint32_t style, uint32_t exstyle, int x, int y,
                            int w, int h, const char *icon, plat_event_fn on_event)
{
    WNDCLASSA wc;
    HINSTANCE inst = GetModuleHandleA(NULL);
    HWND hwnd;
    if (!GetClassInfoA(inst, cls, &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = host_proc;
        wc.hInstance = inst;
        wc.hIcon = icon ? LoadIconA(inst, icon) : NULL;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = cls;
        if (!RegisterClassA(&wc)) {
            log_printf("plat: RegisterClass %s failed (%lu)", cls, GetLastError());
            return 0;
        }
    }
    g_creating = on_event;
    hwnd = CreateWindowExA(exstyle, cls, title, style, x, y, w, h, NULL, NULL, inst, NULL);
    g_creating = NULL;
    if (!hwnd)
        log_printf("plat: CreateWindowEx %s failed (%lu)", cls, GetLastError());
    return (uint32_t)(uintptr_t)hwnd;
}

void plat_window_show(uint32_t win, int cmd)
{
    ShowWindow((HWND)(uintptr_t)win, cmd);
}

void plat_window_update(uint32_t win)
{
    UpdateWindow((HWND)(uintptr_t)win);
}

void plat_window_set_pos(uint32_t win, uint32_t after, int x, int y, int w, int h, uint32_t flags)
{
    SetWindowPos((HWND)(uintptr_t)win, (HWND)(uintptr_t)after, x, y, w, h, flags);
}

uint32_t plat_window_set_long(uint32_t win, int index, uint32_t value)
{
    return (uint32_t)SetWindowLongA((HWND)(uintptr_t)win, index, (LONG)value);
}

uint32_t plat_window_focus(uint32_t win)
{
    return (uint32_t)(uintptr_t)SetFocus((HWND)(uintptr_t)win);
}

void plat_adjust_rect(int32_t rect[4], uint32_t style, uint32_t exstyle, int menu)
{
    RECT r;
    r.left = rect[0], r.top = rect[1], r.right = rect[2], r.bottom = rect[3];
    AdjustWindowRectEx(&r, style, menu, exstyle);
    rect[0] = r.left, rect[1] = r.top, rect[2] = r.right, rect[3] = r.bottom;
}

int plat_show_cursor(int show)
{
    return ShowCursor(show);
}

/* DIK codes are set-1 scan codes, 0x80 marking the E0-prefixed (extended) ones */
void plat_key_state(uint8_t keys[256])
{
    static uint8_t vk[256];
    static int ready;
    int k;
    if (!ready) {
        for (k = 1; k < 256; k++)
            vk[k] = (uint8_t)MapVirtualKeyA(k < 0x80 ? (UINT)k : 0xE000u | (UINT)(k & 0x7F), MAPVK_VSC_TO_VK_EX);
        vk[0x45] = VK_NUMLOCK, vk[0xC5] = VK_PAUSE;   /* the two the table swaps */
        ready = 1;
    }
    keys[0] = 0;
    for (k = 1; k < 256; k++)
        keys[k] = vk[k] && (GetAsyncKeyState(vk[k]) & 0x8000) ? 0x80 : 0;
}

/* everything queued for this thread's windows (ours, and the renderer's and video player's);
   the game took one message a frame, which let raw input and the like pile up */
int plat_pump(void)
{
    MSG m;
    if (g_quit)
        return 0;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            g_quit = 1;
            return 0;
        }
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 1;
}

void plat_wait(void)
{
    if (!g_quit)
        WaitMessage();
}
