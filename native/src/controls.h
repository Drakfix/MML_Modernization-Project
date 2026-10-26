#pragma once
#include "input.h"

/* Modern controls: free-orbit camera (right stick), camera-relative movement
 * (left stick), right-stick aim, directional dodge on X, RT shoot / LT aim,
 * D-pad as left stick, gamepad intro skip. `modern` = 0 keeps stock controls
 * (only the D-pad mapping and intro skip stay on). */
void controls_install(int modern, int intro_pad_skip);

/* Dev aid: accept injected pad input from Local\MML_DEBUG_PAD ([debug] remote_pad=1). */
void controls_debug_pad(void);

/* The modern-controls rewrite of what the game sees from the pad (D-pad ->
 * left stick, RT shoot, LT aim, dodge injection, aim stick swap, forward
 * injection). Runs once per pad poll, whichever backend read the pad. */
void controls_filter(pad_t *p);

/* Apply the debug remote pad (if active) over a physical read. Returns 1 if
 * the result counts as a connected pad. */
int controls_debug_overlay(pad_t *p, int connected);
