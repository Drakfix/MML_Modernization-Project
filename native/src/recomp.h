#pragma once
#include <stdint.h>

/* Recompiled game code harness ([debug] recomp=diff|replace, see recomp.c). */
void recomp_install(const char *ini);
/* [debug] recomp_census=1: once per game frame (writes recomp_census.txt now and then) */
void recomp_census_tick(void);

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
/* - detour: an entry detour on the game function `original`: fn(regs) returns 1 if it handled
     the call (the routine returns to the caller, popping `pops` bytes of stack arguments), 0 to
     let the original function run */
void recomp_register_detour(void *routine, uint32_t original, int (*fn)(const uint32_t regs[8]), int pops);

/* Our C functions that the game calls through pointers (Direct3D methods we replaced in a
   vtable) go through the generic bridge: it copies the stack and finds out how much the callee
   pops, too slow for the ones called tens of thousands of times a frame. A stdcall function
   (WINAPI) with nargs (up to 8) 32-bit arguments registered here is called straight from C
   instead; ddshim registers every method of its interfaces. */
void recomp_register_stdcall(void *fn, int nargs);

/* Inside a shim (shim.h): runs the game function at fn, stdcall with nargs 32-bit arguments, as
   recompiled code on the game's stack below the shim caller's frame; returns its EAX. How the
   portable runtime calls game callbacks (the window procedure) without machine code. */
uint32_t recomp_call_stdcall(uint32_t fn, int nargs, const uint32_t *args);
int recomp_replacing(void);   /* the recompiled game runs ([debug] recomp=replace) */
int recomp_in_shim(void);   /* recomp_call_stdcall / recomp_call_watcall can run now */
/* The same for a game function with Watcom's register convention (EAX EDX EBX ECX, then nstack
   arguments on the stack, the callee pops them): from a shim, a registered stdcall function or
   a mod's hook handler, as recompiled code. Returns EAX; every other register is kept. */
uint32_t recomp_call_watcall(uint32_t fn, const uint32_t regs[4], int nstack, const uint32_t *stack);
/* A game callback our code calls (the DirectDraw enumeration callbacks): stdcall, nargs
   32-bit arguments; as recompiled code inside a shim or a registered stdcall function, as the
   machine code otherwise (the normal game) */
uint32_t recomp_callback(uint32_t fn, int nargs, const uint32_t *args);
