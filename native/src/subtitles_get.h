#pragma once
/* The cutscenes' English subtitles, downloaded and converted by the start menu (subtitles_get.c). */
#include <stddef.h>

/* download, check and convert into dir/subtitles_en.txt (dir: the game's Modernization folder, as
   the system names it, there already); 1 = done, else msg says why not. Blocks: a thread's work. */
int subtitles_download(const char *dir, char *msg, size_t n);
/* the platform's download of url into the file out; 0 if it failed (linux_files.c, gamefiles.c) */
int subtitles_fetch(const char *url, const char *out);
