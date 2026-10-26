#pragma once

/* Modern controls: free-orbit camera (right stick), camera-relative movement
 * (left stick), right-stick aim, directional dodge on X, RT shoot / LT aim,
 * D-pad as left stick, gamepad intro skip. `modern` = 0 keeps stock controls
 * (only the D-pad mapping and intro skip stay on). */
void controls_install(int modern, int intro_pad_skip);

/* Dev aid: accept injected pad input from Local\MML_DEBUG_PAD ([debug] remote_pad=1). */
void controls_debug_pad(void);
