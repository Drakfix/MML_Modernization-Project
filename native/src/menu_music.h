#pragma once
/* The start menu's music, from the player's soundtrack (see menu_music.c). */
void menu_music_start(const char *game_dir, const char *ini);
void menu_music_frame(float volume);   /* each frame: keep it looping, at volume 0..1 */
void menu_music_stop(void);
