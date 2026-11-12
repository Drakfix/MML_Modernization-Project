#pragma once
#include <windows.h>

/* Keyboard and mouse for the orbit camera (see kbm.c). */
void kbm_attach(HWND hwnd);                       /* the game window is known (game thread) */
void kbm_attach_auto(void);                       /* find it on the calling (game) thread */
int kbm_foreground(void);                         /* the game window is in front */
int kbm_key(int vk);                              /* key/mouse button down, game in front */
void kbm_take_mouse(int *dx, int *dy, int *wheel_steps);   /* motion since the last call */
void kbm_capture(int want);                       /* clip + hide the cursor while the orbit drives */
int kbm_vk_from_name(const char *name);           /* "SPACE", "W", "MOUSE1", "F5"... -> VK (0 = unknown) */
int kbm_vk_name(int vk, char *out, size_t n);     /* the other way; 0 if the key has no name */
#if defined(__linux__) && !defined(__ANDROID__)
void kbm_sdl_motion(float dx, float dy, float wheel);   /* kbm_sdl.c: SDL's mouse events (plat_sdl.c) */
#endif
