#pragma once
/* Frame recorder for interpolation (portable, sits on gfx.h only).
 *
 * While interpolation is on, the renderer sends its gfx calls through here:
 * they're drawn right away into scene target 0 (the game's own frame) and
 * also recorded. At present time the recording is replayed into target 1
 * with every vertex that has a known previous position moved part of the
 * way back, that in-between frame is shown first, and the game's frame
 * follows half a game tick later. */
#include "gfx.h"

typedef struct {
    float x, y, z;       /* where this vertex was on screen one game tick ago */
    int valid;
} rec_prev;

void rec_enable(int on);
/* debug: show only the in-between frames; log present timing */
void rec_debug(int mid_only, int timing);
int rec_enabled(void);

void rec_state(const gfx_state *s);
void rec_texture(gfx_tex *t);
void rec_clear(const RECT *rect, int color_too, uint32_t color, int depth_too, float depth);
/* prev may be NULL (nothing moves), else one entry per vertex */
void rec_draw(int prim, const gfx_vertex *v, const rec_prev *prev, int count);
void rec_tex_destroy(gfx_tex *t);

/* show the in-between frame(s), then the game's frame.
 * steps = game ticks of display time this frame covers in 60 Hz units
 * (2 at 30 fps: one in-between frame; 4 at 15 fps: three). */
void rec_present(int steps);
