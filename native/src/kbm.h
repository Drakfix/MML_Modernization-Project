#pragma once
#include <windows.h>

/* Keyboard and mouse for the orbit camera (see kbm.c). */
void kbm_attach(HWND hwnd);                       /* the game window is known (game thread) */
void kbm_attach_auto(void);                       /* find it on the calling (game) thread */
int kbm_foreground(void);                         /* the game window is in front */
int kbm_key(int vk);                              /* key/mouse button down, game in front */
void kbm_take_mouse(int *dx, int *dy, int *wheel_steps);   /* motion since the last call */
void kbm_capture(int want);                       /* clip + hide the cursor while the orbit drives */
