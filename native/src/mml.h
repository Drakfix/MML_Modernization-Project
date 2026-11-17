#pragma once

#define MML_NATIVE_BUILD "0.9.2-beta"

/* Horizontal factor that keeps 2D elements correctly proportioned on the
 * current display (0.75 on 16:9; 1.0 when widescreen is off). */
float mml_hud_ratio(void);

/* Projection distance the game's perspective divide uses for X (the
 * anamorphic mode pre-squeezes X through a shadow copy of H). */
float mml_h_x(void);

/* View zoom (1 = none, > 1 = narrower view), game thread; a scene setting H resets it. */
void mml_zoom(float z);
float mml_zoom_get(void);

/* Room transitions (doors, ladders: the game's loader job) run this many times faster, 1 = as
 * released ([general] transition_speed). */
void mml_set_transition_speed(int speed);
/* ddshim's presents: a fade's Blt (returns 0 when a fast transition holds this one back: the next
 * dimming step draws over it, so every step still darkens the picture) and a normal Flip */
int mml_transition_blt_present(void);
void mml_transition_flip_present(void);

/* Sleep with sub-millisecond precision (high-resolution waitable timer). */
void mml_sleep_ms(double ms);
/* everything mml installs (the DLL's DllMain; the standalone runner after loading the game) */
void mml_init(void);
/* old settings in mml.ini to their current form (aspect 16:9 -> fill): before anything reads it */
void mml_ini_migrate(const char *ini);
#include "gfx.h"
void mml_gfx_options(const char *ini, const char *game_dir, gfx_options *o);
