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

/* capture side, for geometry fixed in the world (the ground): only where it
 * is now; draw time derives where it was from the camera motion */
void interp_vertex_static(const float cam[3], float sx, float sy);

/* the world -> camera transform of this frame (rotation row-major, then
 * translation), as used for the static world: cam = R * world + T */
void interp_camera(const float rot[9], const float trans[3]);

/* for a vertex that wasn't captured (built in screen space, or projected by
 * other code): where its camera-space position cur was one frame earlier,
 * assuming it's fixed in the world. Returns 0 if unknown. */
int interp_static_prev(const interp_vec3 *cur, interp_vec3 *prev);

/* how far a captured vertex moved beyond what the camera motion explains
 * (zero for the static world, the character's own motion for its parts) */
int interp_residual(const interp_vec3 *cur, const interp_vec3 *prev, interp_vec3 *resid);

/* the same for the nearest captured vertex around (sx, sy) at about depth z,
 * for screen-space geometry with no captured vertex of its own (grass cards,
 * sprites built around a projected point). Returns 0 if there is none. */
int interp_borrow_residual(float sx, float sy, float z, interp_vec3 *resid);

/* the frame's projections are done and it's about to be drawn (a camera cut
 * since the previous one leaves the frame without previous positions) */
void interp_frame_end(void);

/* draw side: camera-space position of the vertex projected to (sx, sy) in the
 * frame being drawn, and where the same vertex was the frame before.
 * Returns 0 if unknown, 1 if only the current position is known, 2 if both,
 * 3 if it's static geometry (cur known; the previous one is interp_static_prev). */
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
    float cam_move, cam_turn;
    unsigned rematched;        /* vertices matched to another copy of their model than the same-numbered one */  /* largest camera move (world units) and turn (degrees) per frame */
} interp_stats;
void interp_get_stats(interp_stats *s, int reset);

/* debug: nearest projected screen position in the frame being drawn */
int interp_debug_nearest(float sx, float sy, float *nsx, float *nsy, float *dist);
int interp_debug_in_prev(float sx, float sy);
int interp_debug_in_build(float sx, float sy);
unsigned interp_debug_count(void);
/* the last interp_lookup() that returned 2 took another copy of its model */
int interp_debug_last_rematched(void);
void interp_debug_get(unsigned i, float *sx, float *sy, interp_vec3 *cam);
