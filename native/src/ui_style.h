#pragma once
#include <stdint.h>

#include "gfx_ui.h"

/* Shared look of our UI (see ui_style.c). */
/* palette: the game's HUD blues, the lock-on yellow, the energy-cell green */
#define COL_PANEL_TOP  0xF0122440
#define COL_PANEL_BOT  0xF0081322
#define COL_EDGE       0xFF5CC8FF
#define COL_BLUE       0xFF1F66DA
#define COL_BLUE_DARK  0xFF0C2A6A
#define COL_YELLOW     0xFFFFD23A
#define COL_YELLOW_LO  0xFFFFA81C
#define COL_TEXT       0xFFE6F0FA
#define COL_DIM        0xFF8EA4BC
#define COL_NAVY       0xFF0A1830
#define COL_GREEN_HI   0xFF9BFF6E
#define COL_GREEN_LO   0xFF1FAA48
#define COL_WARN       0xFFFFB347

extern float ui_alpha;   /* fade for everything drawn next (set by each draw callback) */

uint32_t ui_col(uint32_t c);
uint32_t ui_with_alpha(uint32_t c, float a);
float ui_ease_out(float t);
void ui_line(float x0, float y0, float x1, float y1, float th, uint32_t c);
void ui_slant(float x, float y, float w, float h, float slant, uint32_t left, uint32_t right);
uint32_t ui_lerp_col(uint32_t a, uint32_t b, float t);
void ui_panel(float x, float y, float w, float h, float cut, uint32_t top, uint32_t bottom);
void ui_panel_edge(float x, float y, float w, float h, float cut, float th, uint32_t col);
float ui_text_outlined(int font, float x, float y, const char *s, uint32_t top, uint32_t bottom, float scale,
                       float o, uint32_t outline);   /* o = outline thickness */
float ui_text(float x, float y, const char *s, uint32_t col, float scale);
void ui_arrow(float cx, float cy, float size, int dir, uint32_t col);
float ui_cap(float x, float cy, float h, const char *label, uint32_t fill, uint32_t ink);   /* returns the width */
float ui_key_cap(float x, float cy, float h, const char *label);   /* controller button: dark */
float ui_kb_cap(float x, float cy, float h, const char *label);    /* keyboard key: light */
