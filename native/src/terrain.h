#pragma once

/* Terrain near-plane clipping (floor holes). */
int terrain_clip_install(float near_scale);   /* [video] terrain_clip=1 (default) */
/* doubled ([debug] far_terrain=1, default): terrain drawn twice as far (needs terrain_clip_install first) */
int terrain_range_install(int doubled);
/* [video] draw_distance = full (default) | fog: with distance fog on, the range ends just past the fog */
void terrain_draw_distance_load(const char *ini);
void terrain_fog_end(float end_z);   /* fog.c, every frame: the fog's full depth (packet z), 0 = no fog */
void terrain_census_enable(void);   /* dev: count behind-camera polygons per call site */
void terrain_frame(void);           /* call once per frame, after display_frame */
