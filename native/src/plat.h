#pragma once
/* The host platform under the shims (shim_user32.c, shim_process.c): the game's window, its
 * events and the clock. plat_win32.c is the Windows backend; the portable runtime gets an SDL
 * one. Window handles are what the game sees: 32-bit values (a real HWND on Windows). */
#include <stdint.h>

/* the events the game's window procedure takes, in Windows' own message numbering; sent: one
   Windows hands the procedure at once (WM_ACTIVATEAPP, WM_MOVE, WM_DESTROY), not through the
   queue, so it reaches the procedure that is current then */
typedef void (*plat_event_fn)(uint32_t win, uint32_t msg, uint32_t wparam, uint32_t lparam, int sent);

uint32_t plat_ticks_ms(void);                 /* milliseconds, wrapping like GetTickCount */
void plat_desktop_size(int *w, int *h);       /* the primary display's resolution */
void plat_message_box(const char *text, const char *title);

/* a top-level window: class name (what other programs find it by), Windows-style style bits and
   outer size; icon is the exe's resource name ("#n" for a number, or NULL); every event for it
   goes to on_event, on the thread that created it */
uint32_t plat_window_create(const char *cls, const char *title, uint32_t style, uint32_t exstyle, int x, int y,
                            int w, int h, const char *icon, plat_event_fn on_event);
void plat_window_show(uint32_t win, int cmd);  /* ShowWindow's commands */
void plat_window_update(uint32_t win);         /* paint now */
void plat_window_set_pos(uint32_t win, uint32_t after, int x, int y, int w, int h, uint32_t flags);
uint32_t plat_window_set_long(uint32_t win, int index, uint32_t value);   /* style, ex style */
uint32_t plat_window_focus(uint32_t win);     /* previous focus window */
void plat_adjust_rect(int32_t rect[4], uint32_t style, uint32_t exstyle, int menu);
int plat_show_cursor(int show);               /* the new display count, as ShowCursor */

/* deliver pending events (to on_event); returns 0 once the platform asked the program to quit */
int plat_pump(void);
void plat_wait(void);                         /* sleep until there's an event to pump */
