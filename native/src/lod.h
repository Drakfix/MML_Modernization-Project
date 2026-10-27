#pragma once

/* Always draw the full-detail version of every model and of the ground (no LOD swaps). */
int lod_install(int small_tris);
int lod_no_subdiv(void);   /* near ground cells drawn whole, not split 3x3 */   /* small_tris: also keep sub-pixel triangles */
