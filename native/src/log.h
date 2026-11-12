#pragma once

/* Append a line to mml.log in the game folder. Safe to call from DllMain. */
void log_open(const char *game_dir);
void log_open_file(const char *path);   /* another log file (the Windows start menu's) */
void log_printf(const char *fmt, ...);
