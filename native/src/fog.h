#pragma once

/* Distance fog options. */
enum { FOG_ORIGINAL, FOG_LIGHT, FOG_OFF };
void fog_install(int mode, float distance);   /* distance: how much farther "light" pushes it */
void fog_frame(void);                          /* call once per frame */
