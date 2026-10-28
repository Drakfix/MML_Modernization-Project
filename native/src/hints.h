#pragma once

/* Shortcut card on the title screen (see hints.c). */
void hints_install(const char *ini);   /* our renderer only (draws with gfx_ui) */
void hints_frame(void);                /* once per game frame */
