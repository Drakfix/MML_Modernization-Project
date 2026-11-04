#pragma once
/* Windows parts every rendering backend shares: placing the game's window (borderless covering
 * the monitor, or a centred framed window) and its client size. */
#include <windows.h>

#include "gfx.h"

void gfx_win_place(HWND hwnd, const gfx_options *opt, int app_w, int app_h);
void gfx_win_fill_black(HWND hwnd);   /* before there is a device (intro video) */
void gfx_win_client_size(HWND hwnd, int *w, int *h);   /* at least 1 x 1 */
