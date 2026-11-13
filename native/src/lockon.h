#pragma once

/* Z-targeting target selection and reticle (see lockon.c). */
int lockon_install(void);
void lockon_want(int want);          /* lock wanted this frame (aim held, orbit driving) */
void lockon_switch(int dir);         /* +1 = next enemy to the right, -1 = left */
int lockon_locked(void);             /* an enemy is locked */
int lockon_had(void);                /* a lock happened during the current hold */
void lockon_pos(int *x, int *y, int *z);
void lockon_frame(void);             /* once per frame before the walker: draws the reticle */
int lockon_owns(const void *pkt);    /* reticle packets */
void lockon_test_reticle(int on);    /* dev: draw the reticle on Mega Man ([debug] lock_test) */
unsigned lockon_alpha(void);         /* reticle opacity the renderer applies (0..255) */
void lockon_set_color(unsigned rgb);  /* 0xRRGGBB */
unsigned lockon_color(void);         /* the reticle colour, 0xRRGGBB (the crosshair uses it too) */
/* the enemies in range this frame (hit volume centres, one per enemy, world units): their count */
int lockon_volumes(int (*xyz)[3], int max);
/* world point -> the game's 320x240 screen through the current camera; 0 = behind it */
int lockon_project(double wx, double wy, double wz, double *sx, double *sy);
