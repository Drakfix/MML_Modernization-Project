#pragma once
/* The Japanese names in the US version's text ([general] names=japanese): Rock for Mega Man, Kobun,
   Digouter, Reaverbird, Deflector; see names.c. */
#include <stddef.h>
#include <stdint.h>

void names_install(int on);   /* on: the Japanese names (US version only); patches the program's own labels */
int names_active(void);
/* a data file the game opens (its path and contents): a copy with the dialogue's names swapped,
   malloc'd, or NULL when nothing in it changes */
uint8_t *names_file(const char *path, const uint8_t *data, size_t size, size_t *out_size);
/* an ASCII line (the subtitles) with the names swapped into out */
void names_ascii(const char *in, char *out, size_t n);
