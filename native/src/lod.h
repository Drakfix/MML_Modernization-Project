#pragma once

/* Always draw the full-detail version of every model (no LOD swaps). */
int lod_install(int small_tris);   /* small_tris: also keep sub-pixel triangles */
