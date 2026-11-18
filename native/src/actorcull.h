#pragma once
#include <stdint.h>

/* Close actors stay in the shot: the game's per-model visibility test projects the model's origin
 * with the PS1's near saturation again (actorcull.c). */
int actorcull_install(void);
/* gte_capture.c's RTPS hook calls this right after every one-vertex projection; ret = the caller */
void actorcull_after_rtps(uint32_t ret);
