/* plat.h over SDL3: the platform under the shims where there is no Windows (Android; later Linux
 * and Apple). One window, fullscreen, made for Vulkan; its SDL events become the Windows
 * messages the game's window procedure takes; the keyboard by DirectInput scan code. */
#ifndef _WIN32
#include <windows.h>
#include <SDL3/SDL.h>
#ifndef __ANDROID__
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#endif

#include "kbm.h"
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
#if defined(__linux__) && !defined(__ANDROID__)
    /* a desktop: [video] windowed=1 a 1280x720 window, else the whole screen (borderless, as on
       Windows); its size is fixed while the game runs */
    if (GetPrivateProfileIntA("video", "windowed", 0, "mml.ini"))   /* (the game folder is current) */
        g_win = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_VULKAN);
    else
        g_win = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
#else
    g_win = SDL_CreateWindow(title, 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
#endif
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
    /* the rest of what MEGAMAN.CFG can name (the default keys use Insert, Delete, Page Up/Down) */
    case SDL_SCANCODE_INSERT: return 0xD2;
    case SDL_SCANCODE_DELETE: return 0xD3;
    case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_END: return 0xCF;
    case SDL_SCANCODE_PAGEUP: return 0xC9;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_KP_ENTER: return 0x9C;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    default: return 0;
    }
}

/* an SDL key as the Windows virtual key the game's window procedure expects (WM_KEYDOWN); 0 if it
   has no use for it. (SDL's codes are not Windows': its lower-case letters are VK_F1-F11's and the
   numpad's numbers, so 'x' would arrive as F9, the game's exit key.) */
static uint32_t vk_of(SDL_Keycode k)
{
    if (k >= 'a' && k <= 'z')
        return (uint32_t)(k - 'a' + 'A');
    if ((k >= '0' && k <= '9') || k == SDLK_RETURN || k == SDLK_ESCAPE || k == SDLK_SPACE || k == SDLK_BACKSPACE ||
        k == SDLK_TAB)
        return (uint32_t)k;   /* (the same codes) */
    if (k >= SDLK_F1 && k <= SDLK_F12)
        return 0x70 + (uint32_t)(k - SDLK_F1);
    switch (k) {
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
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

/* GetAsyncKeyState for the start menu and our own keys (menu.c, controls.c): a Windows virtual key
   as SDL's scancode, down or not (the bit Windows sets for "down now") */
SHORT GetAsyncKeyState(int vk)
{
    SDL_Scancode s = SDL_SCANCODE_UNKNOWN, s2 = SDL_SCANCODE_UNKNOWN;
    int n = 0;
    const bool *k;
    if (vk >= 0x01 && vk <= 0x06 && vk != 0x03) {   /* the mouse buttons (VK_LBUTTON ... VK_XBUTTON2) */
        static const int button[7] = { 0, SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, 0, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1,
                                       SDL_BUTTON_X2 };
        return (SHORT)(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_MASK(button[vk]) ? 0x8000 : 0);
    }
    if (vk >= 'A' && vk <= 'Z')
        s = (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    else if (vk >= '1' && vk <= '9')
        s = (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    else if (vk >= 0x70 && vk <= 0x7B)   /* F1-F12 */
        s = (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - 0x70));
    else
        switch (vk) {
        case '0': s = SDL_SCANCODE_0; break;
        case 0x08: s = SDL_SCANCODE_BACKSPACE; break;
        case 0x09: s = SDL_SCANCODE_TAB; break;
        case 0x0D: s = SDL_SCANCODE_RETURN; break;
        case 0x1B: s = SDL_SCANCODE_ESCAPE; break;
        case 0x20: s = SDL_SCANCODE_SPACE; break;
        case 0x21: s = SDL_SCANCODE_PAGEUP; break;
        case 0x22: s = SDL_SCANCODE_PAGEDOWN; break;
        case 0x25: s = SDL_SCANCODE_LEFT; break;
        case 0x26: s = SDL_SCANCODE_UP; break;
        case 0x27: s = SDL_SCANCODE_RIGHT; break;
        case 0x28: s = SDL_SCANCODE_DOWN; break;
        case 0x10: s = SDL_SCANCODE_LSHIFT; s2 = SDL_SCANCODE_RSHIFT; break;   /* (either one) */
        case 0x11: s = SDL_SCANCODE_LCTRL; s2 = SDL_SCANCODE_RCTRL; break;
        case 0x12: s = SDL_SCANCODE_LALT; s2 = SDL_SCANCODE_RALT; break;
        case 0x14: s = SDL_SCANCODE_CAPSLOCK; break;
        case 0x23: s = SDL_SCANCODE_END; break;
        case 0x24: s = SDL_SCANCODE_HOME; break;
        case 0x2D: s = SDL_SCANCODE_INSERT; break;
        case 0x2E: s = SDL_SCANCODE_DELETE; break;
        case 0x60: s = SDL_SCANCODE_KP_0; break;
        case 0xA0: s = SDL_SCANCODE_LSHIFT; break;
        case 0xA1: s = SDL_SCANCODE_RSHIFT; break;
        case 0xA2: s = SDL_SCANCODE_LCTRL; break;
        case 0xA3: s = SDL_SCANCODE_RCTRL; break;
        case 0xA4: s = SDL_SCANCODE_LALT; break;
        case 0xA5: s = SDL_SCANCODE_RALT; break;
        case 0xBA: s = SDL_SCANCODE_SEMICOLON; break;
        case 0xBB: s = SDL_SCANCODE_EQUALS; break;
        case 0xBC: s = SDL_SCANCODE_COMMA; break;
        case 0xBD: s = SDL_SCANCODE_MINUS; break;
        case 0xBE: s = SDL_SCANCODE_PERIOD; break;
        case 0xBF: s = SDL_SCANCODE_SLASH; break;
        case 0xC0: s = SDL_SCANCODE_GRAVE; break;
        case 0xDB: s = SDL_SCANCODE_LEFTBRACKET; break;
        case 0xDC: s = SDL_SCANCODE_BACKSLASH; break;
        case 0xDD: s = SDL_SCANCODE_RIGHTBRACKET; break;
        case 0xDE: s = SDL_SCANCODE_APOSTROPHE; break;
        default:
            if (vk >= 0x61 && vk <= 0x69)   /* VK_NUMPAD1-9 (SDL's keypad runs 1-9, then 0) */
                s = (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - 0x61));
            else
                return 0;
        }
    k = SDL_GetKeyboardState(&n);
    if (!k)
        return 0;
    return (SHORT)(((int)s < n && k[s]) || (s2 && (int)s2 < n && k[s2]) ? 0x8000 : 0);
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

#ifdef __ANDROID__
void android_restart(void);   /* android_files.c */

int plat_relaunch_menu(const char *home)
{
    (void)home;
    log_printf("plat: back to the start menu: the app restarts");
    android_restart();
    return 1;
}
#else
/* this program again, with the same arguments (the game folder: the same one) */
int plat_relaunch_menu(const char *home)
{
    static char buf[8192];
    char *argv[64];
    int fd = open("/proc/self/cmdline", O_RDONLY), n = 0, argc = 0, i;
    (void)home;
    if (fd >= 0) {
        n = (int)read(fd, buf, sizeof buf - 1);
        close(fd);
    }
    if (n <= 0)
        return 0;
    buf[n] = 0;
    for (i = 0; i < n && argc < 63; i += (int)strlen(buf + i) + 1)
        argv[argc++] = buf + i;
    argv[argc] = NULL;
    {   /* the program's own path (exec'd as /proc/self/exe it would be named "exe") */
        static char self[4096];
        ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
        if (len <= 0)
            return 0;
        self[len] = 0;
        if (len > 10 && strcmp(self + len - 10, " (deleted)") == 0)   /* (an update replaced it) */
            self[len - 10] = 0;
        log_printf("plat: back to the start menu: %s starts again", self);
        /* No SDL_Quit: it unloads the video and Vulkan libraries while the render and input threads
           still run in them (a crash). execv ends every thread at once and the system closes the
           window. Nothing open carries over (the debug pad's socket, files), but nothing is closed
           under the running threads either: everything is marked to close on exec. */
        for (fd = 3; fd < 1024; fd++)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        execv(self, argv);
    }
    log_printf("plat: it didn't start again (errno %d)", errno);
    return 0;
}
#endif

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
            if (g_on_event && !e.key.repeat && vk_of(e.key.key))
                g_on_event(GAME_WINDOW, WM_KEYDOWN, vk_of(e.key.key), 0, 0);
            break;
#if defined(__linux__) && !defined(__ANDROID__)
        case SDL_EVENT_MOUSE_MOTION:   /* mouse look (kbm_sdl.c) */
            kbm_sdl_motion(e.motion.xrel, e.motion.yrel, 0);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            kbm_sdl_motion(0, 0, e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
            break;
#endif
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
