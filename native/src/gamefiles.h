#pragma once
/* The player's own game files where there is no installer (Android; later Linux): are they there,
 * is the exe one we know, patch it, and the platform's importer (a copy of the player's game
 * folder into the app's storage). See gamefiles.c and the platform files (android_files.c). */
#include <stddef.h>

/* the game folder holds the patched exe (or one that patches) and the data folders */
int gamefiles_ready(const char *dir);

/* after an import: MEGAMAN.EXE checked and patched into MEGAMAN_NATIVE.EXE (the exe the game runs
   from), the bundled intro put in place; 1 = ready, else msg says what is wrong */
int gamefiles_finish(const char *dir, char *msg, size_t n);

/* ---- the platform's importer ---- */
enum { IMPORT_IDLE, IMPORT_PICKING, IMPORT_COPYING, IMPORT_DONE, IMPORT_FAILED, IMPORT_CANCELLED };
int gamefiles_can_import(void);                 /* this platform has one */
void gamefiles_import_begin(const char *dir);   /* the folder picker, then the copy into dir */
/* its state now; status: a line for the screen; progress 0..1 while copying */
int gamefiles_import_poll(char *status, size_t n, float *progress);
/* the bundled intro (Modernization\intro.mpg) into dir if it isn't there; 1 = there now */
int gamefiles_bundled_intro(const char *dir);
/* what a fresh folder lacks, never overwriting: our default mml.ini (bundled), and with the game's
   files the intro and CAPCOM.AVI (see gamefiles.c). Before anything reads mml.ini. */
void gamefiles_defaults(const char *dir);
int gamefiles_bundled_file(const char *name, const char *dir);   /* an app file into dir (platform) */
