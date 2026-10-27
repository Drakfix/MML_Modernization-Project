#pragma once

/* Dev tool ([debug] sampler=1): samples the calling (game) thread every
 * millisecond and logs its hottest code regions every 5000 samples. */
void sampler_start(void);
