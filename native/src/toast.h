#pragma once

/* Short notices ("ORBIT ON", "QUICK SAVED"...). With our renderer they're
 * drawn by gfx_ui in the settings menu's style; otherwise into the game's own
 * HUD bucket in its font (letters and spaces only there). */
enum { TOAST_ICON_PAD, TOAST_ICON_SAVE, TOAST_ICON_WARN };
enum { TOAST_GOOD, TOAST_OFF, TOAST_BAD };
void toast_post(int icon, int tone, const char *title, const char *badge, const char *detail);
void toast_show(int on);   /* modern controls (the orbit) on / off */
void toast_use_ui(void);   /* our renderer is up: draw with gfx_ui from now on */
void toast_frame(void);    /* call once per frame before the display list is walked */
int toast_owns(const void *pkt);   /* is this display-list packet one of ours? */
