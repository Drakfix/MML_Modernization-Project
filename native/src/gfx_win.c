/* gfx_win.h */
#include <windows.h>

#include "gfx_win.h"

void gfx_win_place(HWND hwnd, const gfx_options *opt, int app_w, int app_h)
{
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    if (!opt->windowed) {
        RECT r = mi.rcMonitor;
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongW(hwnd, GWL_EXSTYLE, 0);
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        RECT work = mi.rcWork, r;
        int cw = opt->window_w, ch = opt->window_h;
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        if (cw <= 0 || ch <= 0) {
            ch = (work.bottom - work.top) * 3 / 4;
            cw = ch * app_w / app_h;
        }
        r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
        AdjustWindowRectEx(&r, style, FALSE, 0);
        SetWindowLongW(hwnd, GWL_STYLE, style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, 0);
        SetWindowPos(hwnd, HWND_TOP,
                     work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                     work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                     r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

void gfx_win_fill_black(HWND hwnd)
{
    RECT rc;
    HDC dc;
    GetClientRect(hwnd, &rc);
    dc = GetDC(hwnd);
    if (dc) {
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        ReleaseDC(hwnd, dc);
    }
}

void gfx_win_client_size(HWND hwnd, int *w, int *h)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    *w = rc.right > 0 ? rc.right : 1;
    *h = rc.bottom > 0 ? rc.bottom : 1;
}
