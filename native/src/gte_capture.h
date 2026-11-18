#pragma once

/* Reports every vertex the game's software GTE projects to interp.c. */
int gte_capture_install(int census);   /* census: log match rates every 60 frames */
void gte_capture_frame_end(void);      /* call once per frame, right before the display list is walked */
int gte_rtps_hook(void);               /* only the shared RTPS hook (actorcull.c); the install places it too */
