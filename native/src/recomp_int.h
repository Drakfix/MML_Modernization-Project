#pragma once
/* Internals shared by the runtime (recomp.c) and mml.dll's machine-code side (recomp_native.c,
 * left out of the standalone runner). */
#include <stdint.h>

#include "recomp_rt.h"

enum { MODE_OFF, MODE_DIFF, MODE_REPLACE };
extern int g_mode;

extern const RFuncInfo g_recomp_funcs[];
extern const int g_recomp_nfuncs;
extern const int g_recomp_patch_aware;   /* the code calls our hooks and reads patched bytes itself */

extern int g_rt_errors;
extern uint32_t g_rt_bypass;
extern uint32_t g_exe_lo, g_exe_hi;      /* the game's image */
extern uintptr_t g_mml_lo, g_mml_hi;     /* our own module's image */
extern uintptr_t g_host_free;            /* our private stack is free below this */
extern uintptr_t g_hs_lo, g_hs_hi;       /* the private stack */
extern uintptr_t g_co_lo, g_co_hi;       /* the running game thread's stack (costack) */
extern int g_in_co;                      /* a game thread runs */
extern uint32_t g_recent[16];            /* recent indirect / machine-code targets (crash reports) */
extern unsigned g_nrecent;

/* The exe's entry point: JMP to the Watcom C runtime's startup */
#define G_ENTRY      0x4FA900u
#define G_CRT_START  0x4FFB41u

int find_func(uint32_t a);        /* index into g_recomp_funcs of the function starting at a, or -1 */
int find_func_slow(uint32_t a);   /* the same, without the index (before it exists) */
void rt_dispatch_from(RCtx *c, uint32_t target);   /* rt_dispatch unless target is 0 */
uint32_t flags_from_ctx(const RCtx *c);
void flags_to_ctx(RCtx *c, uint32_t f);
uint32_t guest_fs_base(void);
void start_thunk(void);   /* onto the private stack, then the program runs recompiled (never returns) */
double rt_f80_to_double(const uint8_t *p);   /* 80-bit x87 value -> double, as FLD tbyte + FSTP qword */
void rt_double_to_f80(double v, uint8_t *p);

#ifndef MML_RUNNER
/* recomp_native.c */
enum { CK_DIRECT, CK_IMPL, CK_HOST, CK_NATIVE, CK_MODDED, CK_ENTRY, CK_UNSUPPORTED, CK_HOOK, CK_SHIM, CK_KINDS };
extern int g_census;
void census(int kind, uint32_t addr, uint32_t caller);
#define CENSUS(kind, addr, caller) do { if (g_census) census(kind, addr, caller); } while (0)
void native_install(const char *ini);   /* after the shims: roots, thread entries, the program start */
int native_root(RCtx *c, uint32_t *target);   /* target is a root's entry stub: run the root */
int host_callable(uint32_t target);   /* our C function, called from C with the game's stack copied */
void rt_host_call(RCtx *c, uint32_t target);
uint32_t native_return_address(uint32_t seen);   /* the game's return address, or 0 */
uint32_t native_last_target(void);   /* the last machine code the bridge ran (crash reports) */
void native_co_new(int slot);   /* the game thread's stack is new: no open bridges */
void native_co_enter(int slot);   /* the scheduler switches to the thread / back from it */
void native_co_leave(int slot);
#else
#define CENSUS(kind, addr, caller) ((void)(caller))
#endif
