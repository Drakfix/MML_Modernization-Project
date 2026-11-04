#pragma once
#include <stdint.h>

/* Our UI over the final picture, in window pixels (see gfx_ui.c). Each owner
 * registers a draw callback, which calls the drawing functions below; they run
 * in the order added (later ones on top) while each presented frame is
 * composed. Colours are 0xAARRGGBB. */
typedef void (*gfx_ui_draw_fn)(void *user, int w, int h);
void gfx_ui_add_draw(gfx_ui_draw_fn draw, void *user);

/* shapes */
void gfx_ui_rect(float x, float y, float w, float h, uint32_t argb);
void gfx_ui_vgrad(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void gfx_ui_hgrad(float x, float y, float w, float h, uint32_t left, uint32_t right);
void gfx_ui_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb);
void gfx_ui_quad(const float xy[8], const uint32_t c[4]);   /* corners in order, one colour each */
void gfx_ui_poly(const float *xy, int n, uint32_t argb);    /* convex, n points */
void gfx_ui_circle(float cx, float cy, float r, uint32_t argb);
void gfx_ui_ring(float cx, float cy, float r, float thick, uint32_t argb);

/* text: font 0 = body, 1 = display (bold italic, 1.5x the body size) */
enum { UI_FONT_BODY, UI_FONT_DISPLAY };
float gfx_ui_font_text(int font, float x, float y, const char *s, uint32_t top, uint32_t bottom, float scale);
float gfx_ui_font_width(int font, const char *s, float scale);
float gfx_ui_font_line(int font, float scale);
float gfx_ui_text(float x, float y, const char *s, uint32_t argb, float scale);   /* body font; returns the width */
float gfx_ui_text_width(const char *s, float scale);
float gfx_ui_line(float scale);   /* body line height */

/* For the backends, while composing a presented frame: the UI's triangles for a w x h window
   (their count; 0 = nothing to draw) and the coverage atlas they sample (R8, aw x ah, alpha =
   colour alpha x coverage), a new one whenever atlas_gen changes. Positions are window pixels
   (y down). Drawn with alpha = vertex alpha x coverage, blended SRC_ALPHA / INV_SRC_ALPHA (alpha:
   ONE / INV_SRC_ALPHA), no depth, no culling, the atlas sampled bilinear and clamped. */
typedef struct {
    float x, y, u, v;
    uint32_t color;   /* 0xAARRGGBB (B8G8R8A8 in memory) */
} gfx_ui_vert;
int gfx_ui_build(int w, int h, const gfx_ui_vert **verts, const uint8_t **atlas, int *aw, int *ah,
                 unsigned *atlas_gen);
/* gfx.c with a render thread: the callbacks run in gfx_ui_snapshot (on the game thread, at each
   queued frame end) and gfx_ui_build returns the latest snapshot */
void gfx_ui_threaded(int on);
void gfx_ui_snapshot(void);
