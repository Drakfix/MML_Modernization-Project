#pragma once

/* "ORBIT ON/OFF" indicator drawn into the game's own HUD bucket. */
void toast_show(int on);
void toast_frame(void);   /* call once per frame before the display list is walked */
int toast_owns(const void *pkt);   /* is this display-list packet one of ours? */
