#pragma once

/* Horizontal factor that keeps 2D elements correctly proportioned on the
 * current display (0.75 on 16:9; 1.0 when widescreen is off). */
float mml_hud_ratio(void);

/* Projection distance the game's perspective divide uses for X (the
 * anamorphic mode pre-squeezes X through a shadow copy of H). */
float mml_h_x(void);

/* Sleep with sub-millisecond precision (high-resolution waitable timer). */
void mml_sleep_ms(double ms);
