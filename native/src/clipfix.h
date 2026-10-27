#pragma once
#include "gfx.h"

/* Exact texture coordinates where the near plane cuts a model polygon
 * (the game rounds them to whole texels, which made close walls swim). */
int clipfix_install(void);
void clipfix_frame(void);
void clipfix_report(int on);   /* also feed the vertices it projects to interp.c */   /* once per frame, before the display list is drawn */
/* the draw's vertices with the cut points' UVs corrected (v itself if none) */
const gfx_vertex *clipfix_apply(const gfx_vertex *v, const gfx_vertex *raw, unsigned n);
