#pragma once
/* Frame interpolation core (60 fps from the game's 30): portable C, no
 * Windows or x86 specifics, so it carries over to the native port.
 *
 * Every 3D vertex the game projects is reported with an identity (which code
 * drew it, its model-space coordinates, and how many times that pair came up
 * earlier in the frame) plus its camera-space position. The same identity in
 * the previous frame gives where that vertex was one game tick ago, so an
 * in-between frame can place it halfway. Draw time looks vertices up by their
 * screen position, which is what ends up in the display list. */
#include <stdint.h>

typedef struct {
    float x, y, z;            /* camera space */
} interp_vec3;

/* capture side: one projected vertex (sx, sy = the projector's raw output) */
void interp_vertex(uint32_t site, const int16_t model[3], const float cam[3], float sx, float sy);

/* the world -> camera transform of this frame (rotation row-major, then
 * translation), as used for the static world: cam = R * world + T */
void interp_camera(const float rot[9], const float trans[3]);

/* for a vertex that wasn't captured (built in screen space, or projected by
 * other code): where its camera-space position cur was one frame earlier,
 * assuming it's fixed in the world, plus the extra motion of the nearest
 * captured vertex (so parts of moving characters move with them).
 * sx, sy = its projector-space screen position. Returns 0 if unknown. */
int interp_estimate_prev(const interp_vec3 *cur, float sx, float sy, interp_vec3 *prev);

/* the frame's projections are done and it's about to be drawn */
void interp_frame_end(void);

/* draw side: camera-space position of the vertex projected to (sx, sy) in the
 * frame being drawn, and where the same vertex was the frame before.
 * Returns 0 if unknown, 1 if only the current position is known, 2 if both. */
int interp_lookup(float sx, float sy, interp_vec3 *cur, interp_vec3 *prev);

/* nearest vertex (within max_dist, projector units) of the frame being drawn
 * that has a previous position too: for geometry built in screen space around
 * a projected point (grass, sprites), which then moves with that point */
int interp_nearest(float sx, float sy, float max_dist, float *nsx, float *nsy,
                   interp_vec3 *cur, interp_vec3 *prev);

typedef struct {
    unsigned projected;       /* vertices reported last frame */
    unsigned collisions;      /* screen positions shared by different vertices */
    unsigned lookups, found, matched;
} interp_stats;
void interp_get_stats(interp_stats *s, int reset);

/* debug: nearest projected screen position in the frame being drawn */
int interp_debug_nearest(float sx, float sy, float *nsx, float *nsy, float *dist);
int interp_debug_in_prev(float sx, float sy);
int interp_debug_in_build(float sx, float sy);
unsigned interp_debug_count(void);
void interp_debug_get(unsigned i, float *sx, float *sy, interp_vec3 *cam);
