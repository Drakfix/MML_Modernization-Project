#pragma once

/* True widescreen: wide display mode + viewport, primitives shifted at draw
 * time. Returns 1 if installed. */
int display_install(int screen_w, int screen_h, int width, int hud_anchor, float hud_scale);
void display_frame(void);          /* once per frame, before the walker */
int display_native_width(void);    /* 640 when native mode is off */
float display_world_shift(void);   /* X offset added to world (bucket 4) vertices */
void display_set_hud_scale(float s);   /* corner HUD size (0.5..1.5), live */
void display_set_minimap_pos(int x, int y);   /* minimap panel corner, 320x240 (the game's: 14,36) */
void display_set_top_gauge_y(int y);   /* top of the boss / Roll gauges, 320x240; < 0 = the game's (16 / 24) */
/* during a DrawPrimitive: the game's vertices before the widescreen shift (else NULL) */
extern const void *g_display_raw_verts;
