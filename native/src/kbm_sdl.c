/* kbm.h on Linux, over SDL: what kbm.c does on Windows with Raw Input. Mouse motion and the wheel
 * come from SDL's events (plat_sdl.c passes them on, kbm_sdl_motion), relative and unaccelerated
 * while the cursor is captured; keys and buttons from SDL's keyboard and mouse state
 * (GetAsyncKeyState, plat_sdl.c), and count only while the game window has the focus. The cursor
 * is captured (SDL's relative mouse mode: hidden, held in the window) only while controls.c says
 * the orbit camera drives gameplay and the mouse has been used since; it is let go in menus,
 * scenes and when the window loses the focus. */
#if defined(__linux__) && !defined(__ANDROID__)
#include <windows.h>   /* (native/compat) */
#include <SDL3/SDL.h>

#include "kbm.h"
#include "log.h"
#include "menu.h"
#include "plat_sdl.h"

#define WHEEL_NOTCH 120   /* (Windows' WHEEL_DELTA: controls.c counts steps the same way) */

static int g_dx, g_dy, g_wheel;   /* (__atomic: added on the events' thread, taken on the game's) */
static int g_captured, g_engaged;

void kbm_sdl_motion(float dx, float dy, float wheel)
{
    static float rx, ry, rw;   /* the fractions, kept for the next event */
    int ix, iy, iw;
    rx += dx;
    ry += dy;
    rw += wheel * WHEEL_NOTCH;
    ix = (int)rx;
    iy = (int)ry;
    iw = (int)rw;
    rx -= (float)ix;
    ry -= (float)iy;
    rw -= (float)iw;
    __atomic_fetch_add(&g_dx, ix, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_dy, iy, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_wheel, iw, __ATOMIC_RELAXED);
}

void kbm_attach(HWND hwnd)
{
    static int said;
    (void)hwnd;
    if (!said)
        log_printf("kbm: keyboard and mouse ready (SDL)");
    said = 1;
}

void kbm_attach_auto(void)
{
    kbm_attach(NULL);
}

int kbm_foreground(void)
{
    SDL_Window *w = plat_sdl_window();
    return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

int kbm_key(int vk)
{
    return kbm_foreground() && !menu_blocks_input() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void kbm_take_mouse(int *dx, int *dy, int *wheel_steps)
{
    static int wheel_rest;
    int w;
    *dx = __atomic_exchange_n(&g_dx, 0, __ATOMIC_RELAXED);
    *dy = __atomic_exchange_n(&g_dy, 0, __ATOMIC_RELAXED);
    w = __atomic_exchange_n(&g_wheel, 0, __ATOMIC_RELAXED) + wheel_rest;
    *wheel_steps = w / WHEEL_NOTCH;
    wheel_rest = w % WHEEL_NOTCH;
    if (!kbm_foreground() || menu_blocks_input()) {
        *dx = *dy = *wheel_steps = 0;
        wheel_rest = 0;
    } else if (*dx || *dy || *wheel_steps) {
        g_engaged = 1;   /* the mouse is in use: capturing it is welcome now */
    }
}

void kbm_capture(int want)
{
    SDL_Window *w = plat_sdl_window();
    int fg = kbm_foreground();
    if (!fg)
        g_engaged = 0;   /* after the focus left, wait for the mouse to be used again */
    want = want && fg && g_engaged && w;
    if (want != g_captured && w)
        SDL_SetWindowRelativeMouseMode(w, want ? true : false);
    g_captured = want;
}
#else
typedef int kbm_sdl_unused;   /* Windows: kbm.c; Android: nowin_stubs.c */
#endif
