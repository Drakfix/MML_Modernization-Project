#pragma once

/* Terrain near-plane clipping (floor holes). */
int terrain_clip_install(float near_scale);   /* [video] terrain_clip=1 (default) */
void terrain_census_enable(void);   /* dev: count behind-camera polygons per call site */
void terrain_frame(void);           /* call once per frame, after display_frame */
