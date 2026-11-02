#pragma once
#include <stdint.h>

/* Recompiled game code harness ([debug] recomp=diff|replace, see recomp.c). */
void recomp_install(const char *ini);

/* Machine code that recompiled code calls returns to our bridge, so the return address it sees
   is the bridge's. Code that tells its callers apart by return address passes what it saw
   through this to get the game's own (anything else comes back unchanged). */
uint32_t __cdecl recomp_return_address(uint32_t seen);

/* Our assembly routines that the game's code CALLs or JMPs to (through our code patches) work on
   the game's registers and x87 stack, so in the recompiled game they would each need the full
   bridge into machine code, and the original code they call runs as machine code too. Their
   mods register C versions, used when the recompiled code reaches the routine:
   - st0: the routine replaces ST0 with fn(ST0) and returns (registers and stack depth kept);
   - wrap: the routine runs pre, the original function at `original`, then post with its EAX.
     regs: EAX ECX EDX EBX ESP EBP ESI EDI at the routine's entry ([ESP] = return address). */
void recomp_register_st0(void *routine, double (*fn)(double));
void recomp_register_wrap(void *routine, uint32_t original, void (*pre)(const uint32_t regs[8]),
                          void (*post)(uint32_t eax));

/* Our C functions that the game calls through pointers (Direct3D methods we replaced in a
   vtable) go through the generic bridge: it copies the stack and finds out how much the callee
   pops, too slow for the ones called tens of thousands of times a frame. A stdcall function
   (WINAPI) with nargs 32-bit arguments registered here is called straight from C instead. */
void recomp_register_stdcall(void *fn, int nargs);
