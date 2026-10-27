#pragma once
#include "input.h"

/* Modern controls: free-orbit camera (right stick), camera-relative movement
 * (left stick), right-stick aim, directional dodge on X, RT shoot / LT aim,
 * D-pad as left stick, gamepad intro skip. `modern` = 0 keeps stock controls
 * (only the D-pad mapping and intro skip stay on). */
void controls_install(int modern, int intro_pad_skip);

/* Dev aid: accept injected pad input from Local\MML_DEBUG_PAD ([debug] remote_pad=1). */
void controls_debug_pad(void);

/* Dev aid: lock-on always targets the support object ([debug] lock_test=1). */
void controls_debug_lock_test(void);

/* The modern-controls rewrite of what the game sees from the pad (D-pad ->
 * left stick, RT shoot, LT aim, dodge injection, aim stick swap, forward
 * injection). Runs once per pad poll, whichever backend read the pad. */
void controls_filter(pad_t *p);

/* Apply the debug remote pad (if active) over a physical read. Returns 1 if
 * the result counts as a connected pad. */
int controls_debug_overlay(pad_t *p, int connected);

/* Roll speed (stock cartwheel 0x200), extra roll animation speed in %, the
 * stick tilt (in %) below which Mega Man walks instead of running, and the
 * walk/run speed (in % of stock) at the low end of their tilt ranges.
 * Negative or zero = keep the default. */
void controls_tune(int roll_speed, int roll_anim_pct, int walk_pct, int walk_min_pct, int run_min_pct);

/* Orbit camera speed (% of the default, X = yaw, Y = pitch), inversion, stick
 * deadzone (% of full tilt) and whether the orbit camera starts on.
 * Zero/negative percentages keep the defaults. */
void controls_camera(int speed_x_pct, int speed_y_pct, int invert_x, int invert_y, int deadzone_pct, int orbit_start);

/* Button remapping for the orbit-camera layout: reads [controls] btn_a .. btn_start
 * (jump, shoot, special, talk, roll, aim, orbit, menu, select, none). */
void controls_load_buttons(const char *ini);

/* Keyboard + mouse for the orbit camera: on/off, mouse speed in %, inverted
 * mouse Y. controls_kbm_active: the game is in front and keyboard/mouse count. */
void controls_mouse(int enabled, int sens_pct, int invert_y);
int controls_kbm_active(void);
