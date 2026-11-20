#pragma once

/* Shot homing: the original game's buster targeting, and how much of it shots aimed with the
   crosshair keep (homing.c). */
enum { HOMING_OFF, HOMING_LOW, HOMING_HIGH };   /* [controls] shot_homing = off, low, high (the original) */
void homing_frame(void);              /* each frame, right after the game spawned this frame's shots (lockon.c) */
int homing_read(const char *ini);     /* the setting in mml.ini (unset: as the combat style the aiming settings match) */
void homing_set(int mode);
void homing_load(const char *ini);    /* homing_set(homing_read(ini)) */
