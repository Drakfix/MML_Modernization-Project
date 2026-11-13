#pragma once
/* The player's own game files where there is no installer (Android, Linux): are they there, is the
 * exe one we know, patch it, and the platform's importer (a copy of the player's game folder into
 * the app's storage). See gamefiles.c and the platform files (android_files.c, linux_files.c). */
#include <stddef.h>

/* the game folder holds the patched exe (or one that patches) and the data folders */
int gamefiles_ready(const char *dir);
void gamefiles_sha256(const void *data, size_t n, char hex[65]);   /* lower-case hex */

/* Where the player imports their own files, both versions can be there: the US one (Mega Man
   Legends) in the game folder itself, the Japanese one (Rockman DASH) in a folder of its own in it
   that holds its program, data and saves; settings and mods are shared (compat_set_edition_dir). */
#define GAMEFILES_JP_DIR "Japanese"
void gamefiles_dir(const char *root, int jp, char *out, size_t n);   /* a version's folder */
/* that version is there to play (its program and data in its folder; on Windows also the exe Setup
   patched in place) */
int gamefiles_version_ready(const char *root, int jp);
/* the version to play: [general] edition (us / japanese) where it is there, else the one that is;
   0 = US, 1 = Japanese (always 0 without an importer: an installed folder holds one version) */
int gamefiles_edition(const char *root);

/* after an import: MEGAMAN.EXE (or ROCKMAN.EXE) checked and patched into MEGAMAN_NATIVE.EXE (the exe
   the game runs from) in dir, a version's folder; 1 = ready, else msg says what is wrong. Then
   gamefiles_defaults on the game folder. */
int gamefiles_finish(const char *dir, char *msg, size_t n);

/* ---- the platform's importer ---- */
enum { IMPORT_IDLE, IMPORT_PICKING, IMPORT_COPYING, IMPORT_DONE, IMPORT_FAILED, IMPORT_CANCELLED };
int gamefiles_can_import(void);                 /* this platform has one */
/* the folder picker, then the copy into dir (a version's folder; jp: the Japanese version) */
void gamefiles_import_begin(const char *dir, int jp);
/* its state now; status: a line for the screen; progress 0..1 while copying */
int gamefiles_import_poll(char *status, size_t n, float *progress);
/* the bundled intro (Modernization\intro.mpg) into dir if it isn't there; 1 = there now */
int gamefiles_bundled_intro(const char *dir);
/* what a fresh game folder lacks, never overwriting: our default mml.ini (bundled), and with a
   version's files the intro and CAPCOM.AVI (see gamefiles.c). Before anything reads mml.ini. */
void gamefiles_defaults(const char *dir);
/* Windows: the app installed into the player's game folder: that game made ready to play there (once) */
void gamefiles_adopt(const char *root);
#define GAMEFILES_APP_MARKER "Modernization\\app-folder.txt"   /* (Setup's: the app's folder) */
int gamefiles_bundled_file(const char *name, const char *dir);   /* an app file into dir (platform) */
#ifdef _WIN32
/* the Windows start menu: mml.dll and the wrapper DLLs from the app's folder (root) beside the
   program in dir, a version's folder (nothing if it is root), for the Classic engine (win_files.c) */
void gamefiles_sync_runtime(const char *root, const char *dir);
#endif
#if defined(__linux__) && !defined(__ANDROID__)
/* Linux: the game folder (--game-dir, else MML_GAME_DIR, else the app's own in the user's data
   folder: $XDG_DATA_HOME/mml-modernization), made if missing */
const char *gamefiles_home(int argc, char **argv);
#endif

/* ---- the platform's extras for the US version, one at a time (where there is no installer) ---- */
enum { TASK_IDLE, TASK_RUNNING, TASK_DONE, TASK_FAILED };
/* the English subtitles for the cutscenes, downloaded into dir\Modernization (Subtitles.java) */
void gamefiles_subtitles_begin(const char *dir);
/* the Japanese voices (SPJ, DAJ) built into us_dir from the Japanese version in jp_dir (voice_build.h) */
void gamefiles_voices_begin(const char *us_dir, const char *jp_dir);
/* the running one's state; status: a line for the screen; progress 0..1 */
int gamefiles_task_poll(char *status, size_t n, float *progress);
