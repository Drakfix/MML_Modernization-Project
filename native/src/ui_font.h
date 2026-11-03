#pragma once
/* The UI's text (gfx_ui.c): ASCII glyphs rasterised into a coverage atlas by the platform's
 * font engine (ui_font_gdi.c on Windows). */
#include <stdint.h>

#define UI_FONTS 2              /* 0: body text, 1: display (bold italic, 1.5x) */
#define UI_FIRST_CHAR 32
#define UI_LAST_CHAR 126

typedef struct {
    float u0, v0, u1, v1;   /* in the atlas, 0..1 */
    float w, h;             /* drawn size in pixels */
    float adv, lead;        /* advance; how far the drawn box starts left of the pen */
} ui_glyph;

/* both fonts at px line height (the display font at 1.5x) into cov (R8 coverage, aw x ah, rows of
   aw bytes, cleared first); glyph[font][char] and line[font] filled. 0 if it failed. */
int ui_font_rasterize(int px, uint8_t *cov, int aw, int ah, ui_glyph glyph[UI_FONTS][UI_LAST_CHAR + 1],
                      float line[UI_FONTS]);
