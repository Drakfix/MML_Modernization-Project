#pragma once

/* "ORBIT ON/OFF" indicator drawn into the game's own HUD bucket. */
void toast_show(int on);
void toast_frame(void);   /* call once per frame before the display list is walked */
