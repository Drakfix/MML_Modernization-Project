#pragma once
#include <stdint.h>

#include "input.h"

/* On-screen touch controls (Android; see touch.c): a virtual controller drawn over the game whose
 * state joins the physical controller's in input.c, so the game, the start menu and the in-game
 * menu read it as a gamepad. [controls] touch_controls = auto (only while no controller is
 * connected), on or off. Elsewhere these do nothing. */
void touch_load(const char *ini);   /* the setting (again after a change); first call: the overlay's drawing */
int touch_visible(void);            /* 1 while the overlay is shown and takes touches */

/* plat_sdl.c, from SDL's finger events (game thread): kind 0 down, 1 motion, 2 up or cancelled;
   x, y normalized to the window (0..1) */
enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };
void touch_finger(int kind, uint64_t id, float x, float y);
void touch_release_all(void);       /* the app lost the focus */

/* input.c's SDL thread, after each controller read: whether a physical controller is connected
   (Auto follows it), and the touch controls' buttons and sticks added to the pad; *connected
   becomes 1 while the overlay is shown */
void touch_merge(pad_t *p, int *connected);
