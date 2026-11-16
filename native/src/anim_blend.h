#pragma once
/* Animation blending: Mega Man's animation changes cross-fade instead of snapping (anim_blend.c). */

/* hooks the skeleton builder; returns 1 when in place */
int anim_blend_install(void);
/* on (1) or off (0): [video] anim_blend, live */
void anim_blend_set(int on);
