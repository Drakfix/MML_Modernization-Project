#pragma once

/* Every difficulty from the start ([general] all_difficulties): the title menu offers Easy, Normal
 * and Hard without a cleared save, and nothing of it reaches the save files (difficulty.c). */
int difficulty_install(const char *ini);
/* reads [general] all_difficulties again (the menus call it when it changes) */
void difficulty_load(const char *ini);
