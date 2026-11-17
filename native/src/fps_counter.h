#pragma once
/* On-screen frame counter ([video] show_fps): how many frames reach the screen per second,
 * counted where they are presented, shown in the top-right corner. See fps_counter.c. */
void fps_counter_install(int on);   /* registers its UI draw (once); on = shown */
void fps_counter_set(int on);
/* one frame went to the screen (any thread: the presenting one) */
void fps_counter_present(void);
