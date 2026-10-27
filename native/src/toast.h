#pragma once

/* Short notices drawn into the game's own HUD bucket (letters and spaces only:
 * the font tiles we know are A-Z, a-z and space). */
void toast_show(int on);   /* "ORBIT ON" / "ORBIT OFF" */
void toast_message(const char *text, int r, int g, int b);   /* dot colour 0..255 */
void toast_frame(void);   /* call once per frame before the display list is walked */
int toast_owns(const void *pkt);   /* is this display-list packet one of ours? */
