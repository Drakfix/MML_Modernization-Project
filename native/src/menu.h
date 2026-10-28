#pragma once

/* In-game options menu (F1, or Back + Start). See menu.c. */
void menu_install(const char *ini, const char *game_dir);
void menu_frame(void);          /* once per game frame, game thread */
int menu_blocks_input(void);    /* open (or just closed, until everything is let go) */
void menu_filter_buttons(unsigned short *buttons);   /* pad poll: keeps the Back chords from the game */
int menu_take_quicksave(void);   /* Back + RB was pressed (once per press) */
