#pragma once
/* Our own versions of the Windows functions the game imports, for the recompiled game (the
 * portable runtime has no Windows to call). A shim takes the game's arguments as 32-bit values
 * (pointers are guest addresses: HPTR) and returns EAX; the runtime pops its arguments
 * (stdcall) and returns to the game. Found by the address the game calls: the import slot's
 * value or one of the exe's import thunks (JMP [slot]). */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t (*shim_fn)(const uint32_t *args);

/* the import dll!name (case-insensitive dll) of the game's exe runs fn, with nargs arguments */
int shim_add(const char *dll, const char *name, int nargs, shim_fn fn);

/* recomp.c: target is a shimmed import (or thunk): fills in its function and argument count */
int shim_find(uint32_t target, shim_fn *fn, int *nargs);

/* the game's last-error value (GetLastError / SetLastError) */
void shim_set_error(uint32_t e);

/* install all shim groups ([debug] shims=0 leaves the game on Windows' own functions);
   ini: the settings file, game_dir: folder the game's relative paths are under */
void shims_install(const char *ini, const char *game_dir);
void shim_kernel32_install(void);
void shim_process_install(void);

/* memory the game can address, for what a shim hands it (strings, blocks): on 32-bit Windows
   any host memory is; the portable runtime takes it from its reserved guest block (M3 item 4) */
uint32_t shim_guest_alloc(size_t n);
/* the game folder (the current directory the game runs in) */
const char *shim_game_dir(void);
void shim_kernel32_log(int on);   /* [debug] shim_log=1: log every file the game opens */
