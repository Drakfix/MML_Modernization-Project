#pragma once

/* True widescreen: wide display mode + viewport, primitives shifted at draw
 * time. Returns 1 if installed. */
int display_install(int screen_w, int screen_h, int hud_anchor);
void display_frame(void);          /* once per frame, before the walker */
int display_native_width(void);    /* 640 when native mode is off */
