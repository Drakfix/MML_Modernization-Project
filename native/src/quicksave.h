#pragma once

/* Quick save ([debug] quick_save=1): F6 saves to the 10th slot from normal
 * play, loadable from the game's Load Data screen. Never overwrites a save
 * it didn't make itself. */
void quicksave_install(const char *game_dir);
void quicksave_frame(void);   /* game thread, once per frame */
