/* Keyboard and mouse for the orbit camera: raw mouse motion, wheel, and the
 * keys/buttons controls.c turns into actions.
 *
 * Mouse motion comes from Raw Input (no Windows pointer acceleration) on a
 * thread of our own: a message-only window registered with RIDEV_INPUTSINK.
 * Registering the game's window instead posts a WM_INPUT per mouse report into
 * the game's queue, which the 1997 loop drains slowly: motion arrived late in
 * bursts and even Alt+F4 got stuck behind it. Keys and buttons count only
 * while the game window is in front. The cursor is captured (pinned to the
 * window centre, hidden) only while controls.c says the orbit camera drives
 * gameplay and the mouse has been used since; it is let go in menus, scenes
 * and on Alt+Tab. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kbm.h"
#include "log.h"

#ifndef HID_USAGE_PAGE_GENERIC
#define HID_USAGE_PAGE_GENERIC 0x01
#define HID_USAGE_GENERIC_MOUSE 0x02
#endif

static HWND g_hwnd;
static HANDLE g_thread;
static volatile LONG g_dx, g_dy, g_wheel, g_raw_ok;
static int g_captured, g_engaged, g_cursor_hidden;

static LRESULT CALLBACK raw_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            InterlockedExchangeAdd(&g_dx, ri.data.mouse.lLastX);
            InterlockedExchangeAdd(&g_dy, ri.data.mouse.lLastY);
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                InterlockedExchangeAdd(&g_wheel, (SHORT)ri.data.mouse.usButtonData);
        }
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static DWORD WINAPI raw_thread(LPVOID unused)
{
    WNDCLASSA wc;
    RAWINPUTDEVICE rid;
    HWND w;
    MSG m;
    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = raw_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "MMLRawMouse";
    RegisterClassA(&wc);
    w = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags = RIDEV_INPUTSINK;   /* to us even though our window is never in front */
    rid.hwndTarget = w;
    if (!w || !RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        log_printf("kbm: raw mouse input unavailable (error %lu), no mouse look", GetLastError());
        return 0;
    }
    g_raw_ok = 1;
    while (GetMessageA(&m, NULL, 0, 0) > 0)
        DispatchMessageA(&m);
    return 0;
}

void kbm_attach(HWND hwnd)
{
    if (!hwnd || hwnd == g_hwnd)
        return;
    g_hwnd = hwnd;
    if (!g_thread)
        g_thread = CreateThread(NULL, 0, raw_thread, NULL, 0, NULL);
    log_printf("kbm: keyboard and mouse ready");
}

static BOOL CALLBACK find_game_window(HWND w, LPARAM out)
{
    if (IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        *(HWND *)out = w;
        return FALSE;
    }
    return TRUE;
}

/* call on the game thread until it sticks: the game's top-level window */
void kbm_attach_auto(void)
{
    HWND w = NULL;
    if (g_hwnd)
        return;
    EnumThreadWindows(GetCurrentThreadId(), find_game_window, (LPARAM)&w);
    if (w)
        kbm_attach(w);
}

int kbm_foreground(void)
{
    return g_hwnd && GetForegroundWindow() == g_hwnd;
}

int kbm_key(int vk)
{
    return kbm_foreground() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void kbm_take_mouse(int *dx, int *dy, int *wheel_steps)
{
    static int wheel_rest;
    int w;
    *dx = (int)InterlockedExchange(&g_dx, 0);
    *dy = (int)InterlockedExchange(&g_dy, 0);
    w = (int)InterlockedExchange(&g_wheel, 0) + wheel_rest;
    *wheel_steps = w / WHEEL_DELTA;
    wheel_rest = w % WHEEL_DELTA;
    if (!kbm_foreground()) {
        *dx = *dy = *wheel_steps = 0;
        wheel_rest = 0;
    } else if (*dx || *dy || *wheel_steps) {
        g_engaged = 1;   /* the mouse is in use: capturing it is welcome now */
    }
}

/* one step of the cursor display count each way, so a cursor the game hid
 * itself stays hidden when we let go */
static void set_cursor_hidden(int hide)
{
    if (hide && !g_cursor_hidden) {
        ShowCursor(FALSE);
        g_cursor_hidden = 1;
    } else if (!hide && g_cursor_hidden) {
        ShowCursor(TRUE);
        g_cursor_hidden = 0;
    }
}

void kbm_capture(int want)
{
    int fg = kbm_foreground();
    if (!fg)
        g_engaged = 0;   /* after Alt+Tab, wait for the mouse to be used again */
    want = want && fg && g_engaged;
    if (want) {
        RECT rc;
        POINT a = { 0, 0 }, b;
        GetClientRect(g_hwnd, &rc);
        b.x = rc.right;
        b.y = rc.bottom;
        ClientToScreen(g_hwnd, &a);
        ClientToScreen(g_hwnd, &b);
        rc.left = rc.right = (a.x + b.x) / 2;   /* pinned in the middle: no stray clicks on the edges */
        rc.top = rc.bottom = (a.y + b.y) / 2;
        rc.right++;
        rc.bottom++;
        ClipCursor(&rc);
        set_cursor_hidden(1);
    } else if (g_captured) {
        ClipCursor(NULL);
        set_cursor_hidden(0);
    }
    g_captured = want;
}

/* ---- key names (mml.ini key_* values; the launcher uses the same names) ---- */
static const struct { const char *name; int vk; } g_key_names[] = {
    { "SPACE", VK_SPACE }, { "TAB", VK_TAB }, { "ENTER", VK_RETURN }, { "BACKSPACE", VK_BACK },
    { "CAPSLOCK", VK_CAPITAL }, { "LSHIFT", VK_LSHIFT }, { "RSHIFT", VK_RSHIFT },
    { "LCTRL", VK_LCONTROL }, { "RCTRL", VK_RCONTROL }, { "LALT", VK_LMENU }, { "RALT", VK_RMENU },
    { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME }, { "END", VK_END },
    { "PGUP", VK_PRIOR }, { "PGDN", VK_NEXT },
    { "MOUSE1", VK_LBUTTON }, { "MOUSE2", VK_RBUTTON }, { "MOUSE3", VK_MBUTTON },
    { "MOUSE4", VK_XBUTTON1 }, { "MOUSE5", VK_XBUTTON2 },
    { "NUM0", VK_NUMPAD0 }, { "NUM1", VK_NUMPAD1 }, { "NUM2", VK_NUMPAD2 }, { "NUM3", VK_NUMPAD3 },
    { "NUM4", VK_NUMPAD4 }, { "NUM5", VK_NUMPAD5 }, { "NUM6", VK_NUMPAD6 }, { "NUM7", VK_NUMPAD7 },
    { "NUM8", VK_NUMPAD8 }, { "NUM9", VK_NUMPAD9 },
    { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD }, { "SLASH", VK_OEM_2 },
    { "SEMICOLON", VK_OEM_1 }, { "QUOTE", VK_OEM_7 }, { "LBRACKET", VK_OEM_4 }, { "RBRACKET", VK_OEM_6 },
    { "MINUS", VK_OEM_MINUS }, { "EQUALS", VK_OEM_PLUS }, { "BACKQUOTE", VK_OEM_3 }, { "BACKSLASH", VK_OEM_5 },
};

int kbm_vk_from_name(const char *name)
{
    size_t i;
    if (!name || !name[0])
        return 0;
    if (!name[1] && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return name[0];
    if (!name[1] && name[0] >= 'a' && name[0] <= 'z')
        return name[0] - 'a' + 'A';
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12)
            return VK_F1 + n - 1;
    }
    for (i = 0; i < sizeof g_key_names / sizeof g_key_names[0]; i++)
        if (_stricmp(name, g_key_names[i].name) == 0)
            return g_key_names[i].vk;
    return 0;
}
