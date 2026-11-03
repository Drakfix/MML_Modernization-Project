#pragma once
#include "gfx.h"

/* Point the exe's DirectDrawCreate / DirectDrawEnumerateA imports at our own
 * DirectDraw 4 / Direct3D 3 implementation (renderer=mml). Call from DllMain. */
int ddshim_install(const gfx_options *opt);

/* dev: [debug] fog_debug=1 logs the depth range of fogged geometry once a second */
extern int g_fog_debug;
void ddshim_frame(void);
extern int g_probe_x, g_probe_y;
extern int g_drop_clamped;

/* Bring the renderer up early for our intro player (see intro_video.c). */
int ddshim_video_begin(HWND w);
void ddshim_reupload_textures(void);   /* in-game menu: re-run every texture through the upload */

/* our Direct3D device's own vtable, for the mods that hook its methods (display.c, terrain.c,
   fog.c); NULL until the game made the device */
void **ddshim_device_vtable(void);
