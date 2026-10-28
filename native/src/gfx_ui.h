#pragma once
#include <stdint.h>
#include <d3d11.h>

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

/* gfx_d3d11.c: draw the UI into the window's back buffer */
void gfx_ui_render(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, int w, int h);
