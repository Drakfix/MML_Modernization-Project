#pragma once
/* Our own versions of the Windows functions the game imports, for the recompiled game (the
 * portable runtime has no Windows to call). A shim takes the game's arguments as 32-bit values
 * (pointers are guest addresses: HPTR) and returns EAX; the runtime pops its arguments
 * (stdcall) and returns to the game. Found by the address the game calls: the import slot's
 * value or one of the exe's import thunks (JMP [slot]). */
#include <stddef.h>
#include <stdint.h>

#include "guest.h"

typedef uint32_t (*shim_fn)(const uint32_t *args);

/* the import dll!name (case-insensitive dll; "#n" for one imported by ordinal) of the game's
   exe runs fn, with nargs arguments */
int shim_add(const char *dll, const char *name, int nargs, shim_fn fn);

/* an address the game calls (a function of ours a mod put in an import slot): runs fn */
void shim_add_target(uint32_t addr, int nargs, shim_fn fn);

/* a method of an object we hand the game (COM vtables): a guest address that only means "run
   fn" to the runtime (machine code calling it faults), stdcall with nargs arguments, this first */
uint32_t shim_method(int nargs, shim_fn fn);

/* recomp.c: target is a shimmed import (or thunk): fills in its function and argument count */
int shim_find(uint32_t target, shim_fn *fn, int *nargs);

/* the game's last-error value (GetLastError / SetLastError) */
void shim_set_error(uint32_t e);

/* install all shim groups ([debug] shims=0 leaves the game on Windows' own functions);
   ini: the settings file, game_dir: folder the game's relative paths are under */
void shims_install(const char *ini, const char *game_dir);
void shim_kernel32_install(void);
void shim_process_install(void);
void shim_user32_install(void);   /* USER32 / GDI32: the window, messages, window procedure */
void shim_sync_install(void);     /* events and waits */
int shim_dsound_install(const char *ini, const char *game_dir);   /* DirectSound on SDL3 audio; 0 without SDL3 */
int shim_dinput_install(void);    /* DirectInput: platform keyboard, SDL controller; 0 without SDL input */

/* events (shim_sync.c): our handles; set may be called from any thread */
uint32_t shim_event_create(int manual, int initial);
void shim_event_set(uint32_t h);
int shim_event_close(uint32_t h);   /* 1 if h was one of ours */

/* the game folder (the current directory the game runs in) */
const char *shim_game_dir(void);
void shim_kernel32_log(int on);   /* [debug] shim_log=1: log every file the game opens */
