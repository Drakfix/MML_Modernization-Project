#pragma once
#include <stdint.h>

/* Recompiled game code harness ([debug] recomp=diff|replace, see recomp.c). */
void recomp_install(const char *ini);

/* Machine code that recompiled code calls returns to our bridge, so the return address it sees
   is the bridge's. Code that tells its callers apart by return address passes what it saw
   through this to get the game's own (anything else comes back unchanged). */
uint32_t __cdecl recomp_return_address(uint32_t seen);
