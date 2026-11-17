#pragma once
/* Culling for pictures wider than 16:9 (widecull.c). */
int widecull_install(int width);   /* the picture's width at 480 lines; does nothing up to 854 */
void widecull_set_h(float h);      /* the projection distance the scene set (game thread) */
void widecull_frame(void);         /* once per game frame (log counters) */
