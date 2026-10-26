#pragma once

/* Append a line to mml.log in the game folder. Safe to call from DllMain. */
void log_open(const char *game_dir);
void log_printf(const char *fmt, ...);
