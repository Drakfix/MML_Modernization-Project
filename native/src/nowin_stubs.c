/* Where there is no Windows: the parts of ours that are Windows by nature, as calls that do
 * nothing. The debugging tools (watch.c, apicensus.c) read the game through Windows
 * machinery; iat.c patches Windows imports; kbm.c is Raw Input. Keyboard and mouse on other
 * platforms come later, over SDL; until then they report nothing pressed and no motion, so
 * controls.c falls back to the gamepad alone. */
#ifndef _WIN32
#include <windows.h>

#include "apicensus.h"
#include "iat.h"
#include "kbm.h"
#include "log.h"
#include "watch.h"

void watch_configure(const char *spec)
{
    log_printf("watch: not on this platform (%s)", spec ? spec : "");
}

void watch_frame(void)
{
}

void apicensus_install(void)
{
    log_printf("api census: not on this platform");
}

void apicensus_frame(void)
{
}

int iat_patch(HMODULE mod, const char *dll, const char *func, void *repl, void **orig)
{
    (void)mod; (void)dll; (void)func; (void)repl; (void)orig;
    return 0;
}

void kbm_attach(HWND hwnd)
{
    (void)hwnd;
}

void kbm_attach_auto(void)
{
}

int kbm_foreground(void)
{
    return 0;
}

int kbm_key(int vk)
{
    (void)vk;
    return 0;
}

void kbm_take_mouse(int *dx, int *dy, int *wheel_steps)
{
    *dx = *dy = *wheel_steps = 0;
}

void kbm_capture(int want)
{
    (void)want;
}

int kbm_vk_from_name(const char *name)
{
    (void)name;
    return 0;
}
#else
typedef int nowin_stubs_unused;   /* Windows: the real ones */
#endif
