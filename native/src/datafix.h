#pragma once
/* Mistakes in the game's data files, put right as the game loads them; see datafix.c. */
#include <stddef.h>
#include <stdint.h>

int datafix_wants(const char *path);   /* a file one of the fixes is for */
/* fixes the file's contents in place; 1 when something was changed */
int datafix_file(const char *path, uint8_t *data, size_t size);
