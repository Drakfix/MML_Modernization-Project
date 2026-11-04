/* plat.h over SDL3: the platform under the shims where there is no Windows (Android; later Linux
 * and Apple). One window, fullscreen, made for Vulkan; its SDL events become the Windows
 * messages the game's window procedure takes; the keyboard by DirectInput scan code. */
#ifndef _WIN32
#include <windows.h>
#include <SDL3/SDL.h>

#include "log.h"
#include "plat.h"
#include "plat_sdl.h"

#define GAME_WINDOW 0x10010u   /* the window handle the game sees */

static SDL_Window *g_win;
static plat_event_fn g_on_event;
static int g_cursor_count;
static volatile int g_back, g_quit;   /* Back presses not taken yet; plat_quit asked */

SDL_Window *plat_sdl_window(void)
{
    return g_win;
}

uint32_t plat_ticks_ms(void)
{
    return (uint32_t)SDL_GetTicks();
}

void plat_sleep_ms(unsigned ms)
{
    SDL_Delay(ms);
}

long plat_atomic_xchg(volatile long *p, long v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

void plat_desktop_size(int *w, int *h)
{
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    *w = m ? m->w : 1920;
    *h = m ? m->h : 1080;
}

void plat_message_box(const char *text, const char *title)
{
    log_printf("message box \"%s\": %s", title ? title : "", text ? text : "");
}

uint32_t plat_window_create(const char *cls, const char *title, uint32_t style, uint32_t exstyle, int x, int y,
                            int w, int h, const char *icon, plat_event_fn on_event)
{
    int pw = 0, ph = 0;
    (void)cls; (void)style; (void)exstyle; (void)x; (void)y; (void)w; (void)h; (void)icon;
    if (g_win) {
        if (on_event && !g_on_event) {   /* the start menu's window: the game's from now on */
            g_on_event = on_event;
            log_printf("plat: the game takes the start menu's window");
            return GAME_WINDOW;
        }
        log_printf("plat: only one window here");
        return 0;
    }
    g_win = SDL_CreateWindow(title, 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
    if (!g_win) {
        log_printf("plat: SDL_CreateWindow failed: %s", SDL_GetError());
        return 0;
    }
    g_on_event = on_event;
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    compat_set_window((HWND)(uintptr_t)GAME_WINDOW, pw, ph);
    log_printf("plat: window %dx%d", pw, ph);
    return GAME_WINDOW;
}

/* Windows activates a window as ShowWindow first shows it (WM_ACTIVATEAPP, sent), and the game
   stays paused until then. The app has had the focus since before the window existed, so SDL
   reports no change: the activation is ours to send. */
void plat_window_show(uint32_t win, int cmd)
{
    static int shown;
    if (cmd == 0 /* SW_HIDE */ || shown || !g_on_event)
        return;
    shown = 1;
    g_on_event(win, WM_ACTIVATEAPP, (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0, 0, 1);
}

void plat_window_update(uint32_t win)
{
    (void)win;
}

void plat_window_set_pos(uint32_t win, uint32_t after, int x, int y, int w, int h, uint32_t flags)
{
    (void)win; (void)after; (void)x; (void)y; (void)w; (void)h; (void)flags;   /* (always fullscreen) */
}

uint32_t plat_window_set_long(uint32_t win, int index, uint32_t value)
{
    (void)win; (void)index; (void)value;
    return 0;
}

uint32_t plat_window_focus(uint32_t win)
{
    (void)win;
    return GAME_WINDOW;
}

void plat_adjust_rect(int32_t rect[4], uint32_t style, uint32_t exstyle, int menu)
{
    (void)rect; (void)style; (void)exstyle; (void)menu;   /* no frame */
}

int plat_show_cursor(int show)
{
    g_cursor_count += show ? 1 : -1;
    return g_cursor_count;
}

/* SDL scancode -> DirectInput scan code (DIK_*) */
static uint8_t dik_of(SDL_Scancode s)
{
    static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                         0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z)
        return letters[s - SDL_SCANCODE_A];
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9)
        return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10)
        return (uint8_t)(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_RALT: return 0xB8;
    case SDL_SCANCODE_UP: return 0xC8;
    case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    default: return 0;
    }
}

void plat_key_state(uint8_t keys[256])
{
    int n = 0, s;
    const bool *k = SDL_GetKeyboardState(&n);
    memset(keys, 0, 256);
    for (s = 0; s < n; s++)
        if (k[s]) {
            uint8_t d = dik_of((SDL_Scancode)s);
            if (d)
                keys[d] = 0x80;
        }
}

int plat_take_back(void)
{
    int n = g_back;
    g_back = 0;
    return n;
}

void plat_quit(void)
{
    g_quit = 1;
}

int plat_pump(void)
{
    SDL_Event e;
    int alive = 1;
    if (g_quit) {
        if (g_on_event)
            g_on_event(GAME_WINDOW, WM_DESTROY, 0, 0, 1);
        return 0;
    }
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            if (g_on_event)
                g_on_event(GAME_WINDOW, WM_DESTROY, 0, 0, 1);
            alive = 0;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (g_on_event)
                g_on_event(GAME_WINDOW, WM_ACTIVATEAPP, e.type == SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 1);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_AC_BACK) {   /* Android's Back: ours (menu.c), not the game's */
                if (!e.key.repeat)
                    g_back++;
                break;
            }
            if (g_on_event && !e.key.repeat && e.key.key < 0x80)
                g_on_event(GAME_WINDOW, WM_KEYDOWN, (uint32_t)e.key.key, 0, 0);
            break;
        default:
            break;
        }
    }
    return alive;
}

void plat_wait(void)
{
    SDL_WaitEventTimeout(NULL, 50);
}
#else
typedef int plat_sdl_unused;   /* Windows: plat_win32.c */
#endif
