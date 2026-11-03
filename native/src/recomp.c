/* Runtime for the recompiled game code (tools/recomp/recomp.py, recomp_rt.h).
 *
 * The dispatcher (rt_dispatch) runs the generated functions and whatever they reach that isn't
 * one: a Windows import our shims replace (shim.h), one of our C functions the game calls
 * through a pointer (recomp_register_stdcall), the C version of one of our assembly routines
 * (recomp_register_st0 / wrap / detour), a hook handler at a patched site (rt_hook). Our C code
 * calls the game back through recomp_call_stdcall / recomp_call_watcall. The game's cooperative
 * threads run on stacks of our own (costack.h), and the program starts from its entry point on
 * a private host stack (recomp_run, recomp_start).
 *
 * This is the part that needs no machine code of the game. mml.dll can also run the game's own
 * machine code, partly or wholly ([debug] recomp=off|diff|replace): the bridge between the two,
 * the hooked entries (roots) and the diff mode are in recomp_native.c, which the standalone
 * runner (MML_RUNNER) leaves out. */
#include <windows.h>
#include <intrin.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "hook.h"
#include "log.h"
#include "recomp.h"
#include "recomp_int.h"
#include "costack.h"
#include "shim.h"
#include "watch.h"

#ifdef RECOMP_NONE
const RFuncInfo g_recomp_funcs[1] = { { 0 } };
const int g_recomp_nfuncs = 0;
const int g_recomp_patch_aware = 0;
#endif

#define B2(n) n, n + 1, n + 1, n + 2
#define B4(n) B2(n), B2(n + 1), B2(n + 1), B2(n + 2)
#define B6(n) B4(n), B4(n + 1), B4(n + 1), B4(n + 2)
const uint8_t g_popcnt8[256] = { B6(0), B6(1), B6(1), B6(2) };   /* bits set in each byte value */
#undef B2
#undef B4
#undef B6


int g_mode;
uint8_t *g_rc_native;          /* per function (g_recomp_nfuncs): run the machine code instead (RC_CALL) */
uint32_t g_exe_lo, g_exe_hi;   /* the game's image */
uintptr_t g_mml_lo, g_mml_hi;  /* our own module's image */
/* Our own stacks: the private one, and the running thread's (costack). A stub entered while ESP is
   already on one of them (our C code called a game function directly, e.g. from a hook handler)
   carries on 256 KB below ESP: the game code uses the stack from ESP down as its own (its
   arguments are there), and jumping to g_host_free would overwrite the frames in between. */
uintptr_t g_host_free;         /* our private stack is free below this */
uintptr_t g_hs_lo, g_hs_hi, g_co_lo, g_co_hi;
int g_in_co;

/* ---- 80-bit x87 values, as FLD tbyte / FSTP qword and FLD qword / FSTP tbyte convert them ----
 * Extended: 64-bit significand with an explicit integer bit, 15-bit exponent (bias 16383).
 * To double: rounded to nearest, ties to even (the control word our code runs with); NaNs keep
 * their top fraction bits and become quiet; unnormals are invalid (the default NaN). */
double rt_f80_to_double(const uint8_t *p)
{
    uint64_t m, d, frac, rest, half;
    uint16_t se;
    int e, de, shift;
    double v;
    memcpy(&m, p, 8);
    memcpy(&se, p + 8, 2);
    e = se & 0x7FFF;
    if (e == 0x7FFF || (e && !(m >> 63))) {
        if (e == 0x7FFF && m == 1ull << 63)
            d = 0x7FF0000000000000ull;   /* infinity */
        else if (e == 0x7FFF && (m >> 63))
            d = 0x7FF8000000000000ull | (m >> 11 & 0xFFFFFFFFFFFFFull);
        else
            d = 0xFFF8000000000000ull;   /* invalid encodings: the real indefinite (sign set) */
        if (d != 0xFFF8000000000000ull)
            d |= (uint64_t)(se >> 15) << 63;
        memcpy(&v, &d, 8);
        return v;
    }
    if (!m) {
        d = 0;
    } else {
        if (!e)
            e = 1;   /* denormal (or pseudo-denormal): the smallest exponent */
        while (!(m >> 63))
            m <<= 1, e--;
        de = e - 16383 + 1023;   /* the double's biased exponent */
        if (de >= 0x7FF) {
            d = 0x7FF0000000000000ull;
        } else if (de > 0) {
            frac = m >> 11;
            rest = m & 0x7FF;
            if (rest > 0x400 || (rest == 0x400 && (frac & 1)))
                frac++;
            if (frac >> 53)
                frac >>= 1, de++;
            d = de >= 0x7FF ? 0x7FF0000000000000ull : (uint64_t)de << 52 | (frac & 0xFFFFFFFFFFFFFull);
        } else {
            shift = 12 - de;   /* a double denormal: its fraction holds the bits above this */
            if (shift > 64) {
                frac = 0;
            } else if (shift == 64) {
                frac = m > 1ull << 63;
            } else {
                frac = m >> shift;
                rest = m & ((1ull << shift) - 1);
                half = 1ull << (shift - 1);
                if (rest > half || (rest == half && (frac & 1)))
                    frac++;
            }
            d = frac;   /* (rounding up to 2^52 gives the smallest normal: exponent field 1) */
        }
    }
    d |= (uint64_t)(se >> 15) << 63;
    memcpy(&v, &d, 8);
    return v;
}

/* exact; a signalling NaN becomes quiet */
void rt_double_to_f80(double v, uint8_t *p)
{
    uint64_t d, m;
    int e;
    uint16_t se;
    memcpy(&d, &v, 8);
    e = (int)(d >> 52 & 0x7FF);
    m = d & 0xFFFFFFFFFFFFFull;
    if (e == 0x7FF) {
        se = 0x7FFF;
        m = m ? m << 11 | 3ull << 62 : 1ull << 63;
    } else if (e) {
        se = (uint16_t)(e - 1023 + 16383);
        m = m << 11 | 1ull << 63;
    } else if (m) {   /* a double denormal: m * 2^-1074 */
        e = 15361;
        m <<= 11;
        while (!(m >> 63))
            m <<= 1, e--;
        se = (uint16_t)e;
    } else {
        se = 0;
    }
    se |= (uint16_t)(d >> 63 << 15);
    memcpy(p, &m, 8);
    memcpy(p + 8, &se, 2);
}

double rt_ld80(uint32_t a)
{
    return rt_f80_to_double(HPTR(a));
}

void rt_st80(RCtx *c, uint32_t a, double v)
{
    uint8_t buf[10];
    rt_double_to_f80(v, buf);
#ifndef MML_RUNNER
    if (c->log)
        rt_store_log(c, a, 8), rt_store_log(c, a + 8, 2);
#else
    (void)c;
#endif
    memcpy(HPTR(a), buf, 10);
}

/* entry address -> function index + 1 (0: none), over the generated code's address range: a
   jump between functions looks its target up on every iteration of the dispatcher */
static uint16_t *g_func_at;
static uint32_t g_func_lo, g_func_hi;

int find_func_slow(uint32_t a)
{
    int lo = 0, hi = g_recomp_nfuncs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_recomp_funcs[m].addr == a)
            return m;
        if (g_recomp_funcs[m].addr < a)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return -1;
}

int find_func(uint32_t a)
{
    if (g_func_at)
        return a >= g_func_lo && a < g_func_hi ? (int)g_func_at[a - g_func_lo] - 1 : -1;
    return find_func_slow(a);
}

static void build_func_index(void)
{
    int i;
    if (!g_recomp_nfuncs || g_recomp_nfuncs >= 0xFFFF)
        return;
    g_func_lo = g_recomp_funcs[0].addr;
    g_func_hi = g_recomp_funcs[g_recomp_nfuncs - 1].addr + 1;
    g_func_at = (uint16_t *)calloc(g_func_hi - g_func_lo, sizeof *g_func_at);
    if (!g_func_at)
        return;
    for (i = 0; i < g_recomp_nfuncs; i++)
        g_func_at[g_recomp_funcs[i].addr - g_func_lo] = (uint16_t)(i + 1);
}

int g_rt_errors;

/* ---- our routines as the game knows them (recomp.h) ---- */
#ifdef MML_RUNNER
#define STAND_INS 1024
static void *g_stand_in[STAND_INS];
static uint32_t g_stand_base;
static int g_nstand;

uint32_t recomp_routine_address(void *routine)
{
    int k;
    for (k = 0; k < g_nstand; k++)
        if (g_stand_in[k] == routine)
            return g_stand_base + 4u * (uint32_t)k;
    if (!g_stand_base)
        g_stand_base = guest_pages(0, 4 * STAND_INS, 0);   /* reserved, never touched: only keys */
    if (!g_stand_base || g_nstand >= STAND_INS) {
        log_printf("recomp: no room for another routine address");
        return 0;
    }
    g_stand_in[g_nstand] = routine;
    return g_stand_base + 4u * (uint32_t)g_nstand++;
}

void *recomp_routine_host(uint32_t addr)
{
    uint32_t k = (addr - g_stand_base) / 4;
    return g_stand_base && addr >= g_stand_base && k < (uint32_t)g_nstand ? g_stand_in[k] : NULL;
}
#else
uint32_t recomp_routine_address(void *routine)
{
    return (uint32_t)(uintptr_t)routine;
}

void *recomp_routine_host(uint32_t addr)
{
    return (void *)(uintptr_t)addr;
}
#endif

/* ---- C versions of our assembly routines (recomp.h) ---- */
uint32_t g_rt_bypass;
static struct {
    uint32_t routine, original;
    double (*st0)(double);
    void (*pre)(const uint32_t regs[8]);
    void (*post)(uint32_t eax);
    int (*detour)(const uint32_t regs[8]);
    int pops;
} g_impl[8];
static int g_nimpl;

void recomp_register_st0(void *routine, double (*fn)(double))
{
    if (g_nimpl < 8) {
        g_impl[g_nimpl].routine = recomp_routine_address(routine);
        g_impl[g_nimpl].st0 = fn;
        g_nimpl++;
    }
}

void recomp_register_wrap(void *routine, uint32_t original, void (*pre)(const uint32_t regs[8]),
                          void (*post)(uint32_t eax))
{
    if (g_nimpl < 8) {
        g_impl[g_nimpl].routine = recomp_routine_address(routine);
        g_impl[g_nimpl].original = original;
        g_impl[g_nimpl].pre = pre;
        g_impl[g_nimpl].post = post;
        g_nimpl++;
    }
}

/* our stdcall C functions the game calls through pointers: address -> argument count */
#define DIRECT_SLOTS 2048u
void recomp_register_detour(void *routine, uint32_t original, int (*fn)(const uint32_t regs[8]), int pops)
{
    if (g_nimpl < 8) {
        g_impl[g_nimpl].routine = recomp_routine_address(routine);
        g_impl[g_nimpl].original = original;
        g_impl[g_nimpl].detour = fn;
        g_impl[g_nimpl].pops = pops;
        g_nimpl++;
    }
}

static struct {
    uint32_t fn;   /* as the game knows it (recomp_routine_address) */
    int nargs;
    void *host;
} g_direct[DIRECT_SLOTS];
static int g_ndirect;

static unsigned direct_slot(uint32_t fn)
{
    return (fn * 0x9E3779B1u) >> 21;   /* 11 bits */
}

void recomp_register_stdcall(void *fn, int nargs)
{
    uint32_t a = recomp_routine_address(fn);
    unsigned i = direct_slot(a);
    if (!a || nargs < 0 || nargs > 8 || g_ndirect >= (int)DIRECT_SLOTS / 2)
        return;
    while (g_direct[i].fn && g_direct[i].fn != a)
        i = (i + 1) & (DIRECT_SLOTS - 1);
    if (g_direct[i].fn && g_direct[i].nargs != nargs) {
        /* one function in slots with different argument counts: the bridge sees what it pops */
        if (g_direct[i].nargs >= 0)
            log_printf("recomp: %08X registered with %d and %d arguments, left to the bridge", a,
                       g_direct[i].nargs, nargs);
        g_direct[i].nargs = -1;
        return;
    }
    if (!g_direct[i].fn)
        g_ndirect++;
    g_direct[i].fn = a;
    g_direct[i].nargs = nargs;
    g_direct[i].host = fn;
}

static int direct_nargs(uint32_t fn, void **host)
{
    unsigned i = direct_slot(fn);
    if (!g_ndirect)
        return -1;
    while (g_direct[i].fn) {
        if (g_direct[i].fn == fn) {
            *host = g_direct[i].host;
            return g_direct[i].nargs;   /* -1: a conflict */
        }
        i = (i + 1) & (DIRECT_SLOTS - 1);
    }
    return -1;
}

typedef uint32_t(__stdcall *std0)(void);
typedef uint32_t(__stdcall *std1)(uint32_t);
typedef uint32_t(__stdcall *std2)(uint32_t, uint32_t);
typedef uint32_t(__stdcall *std3)(uint32_t, uint32_t, uint32_t);
typedef uint32_t(__stdcall *std4)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(__stdcall *std5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(__stdcall *std6)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(__stdcall *std7)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(__stdcall *std8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* a stdcall machine-code function (ours or a game callback's native copy) with n arguments */
static uint32_t call_stdcall(void *fn, int n, const uint32_t *a)
{
    switch (n) {
    case 0: return ((std0)fn)();
    case 1: return ((std1)fn)(a[0]);
    case 2: return ((std2)fn)(a[0], a[1]);
    case 3: return ((std3)fn)(a[0], a[1], a[2]);
    case 4: return ((std4)fn)(a[0], a[1], a[2], a[3]);
    case 5: return ((std5)fn)(a[0], a[1], a[2], a[3], a[4]);
    case 6: return ((std6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7: return ((std7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    default: return ((std8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    }
}

static RCtx *g_shim_ctx;   /* the recompiled game's context while our C code it called runs (a shim, a
                              registered stdcall, a hook handler): recomp_call_stdcall / watcall */

/* returns 1 if target is a shimmed Windows import (and its shim has run) */
static int rt_shim(RCtx *c, uint32_t target)
{
    shim_fn fn;
    int n;
    uint32_t esp, ret, r, a[12];
    RCtx *outer = g_shim_ctx;
    if (!shim_find(target, &fn, &n))
        return 0;
    esp = R32(c, REG_ESP);
    ret = LD32(esp);
    memcpy(a, HPTR(esp + 4), 4 * (size_t)n);
    g_shim_ctx = c;
    r = fn(a);
    g_shim_ctx = outer;
    R32_SET(c, REG_EAX, r);
    R32_SET(c, REG_ESP, esp + 4 + 4 * (uint32_t)n);   /* stdcall: the shim's arguments are popped */
    c->ret = ret;
    return 1;
}

uint32_t recomp_callback(uint32_t fn, int nargs, const uint32_t *args)
{
    if (g_shim_ctx)
        return recomp_call_stdcall(fn, nargs, args);
#ifndef MML_RUNNER
    return call_stdcall((void *)(uintptr_t)fn, nargs, args);   /* the game's machine code */
#else
    if (g_rt_errors++ < 20)
        log_printf("recomp: game callback %08X called outside the recompiled game", fn);
    return 0;
#endif
}

int recomp_replacing(void)
{
    return g_mode == MODE_REPLACE;
}

int recomp_in_shim(void)
{
    return g_shim_ctx != NULL;
}

uint32_t recomp_call_stdcall(uint32_t fn, int nargs, const uint32_t *args)
{
    static const unsigned keep[6] = { REG_ECX, REG_EDX, REG_EBX, REG_EBP, REG_ESI, REG_EDI };
    RCtx *c = g_shim_ctx;
    uint32_t esp, sp, saved[6], eax;
    int k;
    if (!c) {
        if (g_rt_errors++ < 20)
            log_printf("recomp: game function %08X called outside a shim", fn);
        return 0;
    }
    esp = R32(c, REG_ESP);
    for (k = 0; k < 6; k++)
        saved[k] = R32(c, keep[k]);
    /* below the shim caller's frame: its return address and arguments stay where they are */
    sp = esp - 4 * (uint32_t)nargs - 4;
    ST32(c, sp, 0);   /* the return address: rt_dispatch returns once the function does */
    for (k = 0; k < nargs; k++)
        ST32(c, sp + 4 + 4 * (uint32_t)k, args[k]);
    R32_SET(c, REG_ESP, sp);
    rt_dispatch(c, fn);
    eax = R32(c, REG_EAX);
    R32_SET(c, REG_ESP, esp);
    for (k = 0; k < 6; k++)
        R32_SET(c, keep[k], saved[k]);
    return eax;
}

uint32_t recomp_call_watcall(uint32_t fn, const uint32_t regs[4], int nstack, const uint32_t *stack)
{
    static const unsigned keep[7] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_EBP, REG_ESI, REG_EDI };
    RCtx *c = g_shim_ctx;
    uint32_t esp, sp, saved[7], eax;
    int k;
    if (!c) {
        if (g_rt_errors++ < 20)
            log_printf("recomp: game function %08X called outside the recompiled game", fn);
        return 0;
    }
    esp = R32(c, REG_ESP);
    for (k = 0; k < 7; k++)
        saved[k] = R32(c, keep[k]);
    sp = esp - 4 * (uint32_t)nstack - 4;
    ST32(c, sp, 0);   /* the return address: rt_dispatch returns once the function does */
    for (k = 0; k < nstack; k++)
        ST32(c, sp + 4 + 4 * (uint32_t)k, stack[k]);
    R32_SET(c, REG_ESP, sp);
    R32_SET(c, REG_EAX, regs[0]);
    R32_SET(c, REG_EDX, regs[1]);
    R32_SET(c, REG_EBX, regs[2]);
    R32_SET(c, REG_ECX, regs[3]);
    rt_dispatch(c, fn);
    eax = R32(c, REG_EAX);
    R32_SET(c, REG_ESP, esp);
    for (k = 0; k < 7; k++)
        R32_SET(c, keep[k], saved[k]);
    return eax;
}

#ifndef MML_RUNNER
static int rt_direct_has(uint32_t target)   /* (census) */
{
    void *host;
    return direct_nargs(target, &host) >= 0;
}
#endif

static uint32_t g_direct_ret;   /* the game's return address while a direct call runs */

/* returns 1 if target is a registered stdcall function (and has been run) */
static int rt_direct(RCtx *c, uint32_t target)
{
    void *host = NULL;
    int n = direct_nargs(target, &host);
    uint32_t esp, ret, r, a[8];
    RCtx *outer = g_shim_ctx;
    uint32_t outer_ret = g_direct_ret;
    if (n < 0)
        return 0;
    esp = R32(c, REG_ESP);
    ret = LD32(esp);
    memcpy(a, HPTR(esp + 4), 4 * (size_t)n);
    g_shim_ctx = c;   /* it may call the game back (recomp_callback) */
    g_direct_ret = ret;   /* the game's return address (recomp_return_address) */
    r = call_stdcall(host, n, a);
    g_direct_ret = outer_ret;
    g_shim_ctx = outer;
    R32_SET(c, REG_EAX, r);
    R32_SET(c, REG_ESP, esp + 4 + 4 * (uint32_t)n);
    c->ret = ret;
    return 1;
}

#ifndef MML_RUNNER
static int rt_impl_has(uint32_t target)   /* (census) */
{
    int k;
    for (k = 0; k < g_nimpl; k++)
        if (g_impl[k].routine == target)
            return 1;
    return 0;
}
#endif

/* returns 1 if target is one of them (and has been run) */
static int rt_impl(RCtx *c, uint32_t target)
{
    int k;
    for (k = 0; k < g_nimpl; k++) {
        uint32_t esp;
        if (g_impl[k].routine != target)
            continue;
        esp = R32(c, REG_ESP);
        if (g_impl[k].st0) {
            RF_SET(c, 0x1100, g_impl[k].st0(RF(c, 0x1100)));
            c->ret = LD32(esp);
            R32_SET(c, REG_ESP, esp + 4);
            return 1;
        }
        {
            static const unsigned off[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI };
            uint32_t regs[8];
            int i;
            RCtx *outer = g_shim_ctx;
            for (i = 0; i < 8; i++)
                regs[i] = R32(c, off[i]);
            if (g_impl[k].detour) {   /* handled: return to the caller; not: the original runs */
                int handled;
                g_shim_ctx = c;
                handled = g_impl[k].detour(regs);
                g_shim_ctx = outer;
                if (handled) {
                    c->ret = LD32(esp);
                    R32_SET(c, REG_ESP, esp + 4 + (uint32_t)g_impl[k].pops);
                    return 1;
                }
                g_rt_bypass = g_impl[k].original;
                rt_dispatch(c, g_impl[k].original);
                g_rt_bypass = 0;
                return 1;
            }
            g_shim_ctx = c;
            g_impl[k].pre(regs);
            g_shim_ctx = outer;
            g_rt_bypass = g_impl[k].original;
            rt_dispatch(c, g_impl[k].original);   /* it returns to our caller, as the routine would */
            g_rt_bypass = 0;
            g_shim_ctx = c;
            g_impl[k].post(R32(c, REG_EAX));
            g_shim_ctx = outer;
            return 1;
        }
    }
    return 0;
}

uint32_t __cdecl recomp_return_address(uintptr_t seen)
{
#ifndef MML_RUNNER
    uint32_t r = native_return_address((uint32_t)seen);   /* machine code the bridge ran */
    if (r)
        return r;
#endif
    if (g_direct_ret && seen >= g_mml_lo && seen < g_mml_hi)   /* called by rt_direct (call_stdcall) */
        return g_direct_ret;
    return (uint32_t)seen;
}

void rt_dispatch_from(RCtx *c, uint32_t target)
{
    if (target)
        rt_dispatch(c, target);
}

uint32_t g_recent[16];   /* recent indirect / machine-code targets (crash reports) */
unsigned g_nrecent;

uint32_t rt_dispatch(RCtx *c, uint32_t target)
{
    while (target) {
        int i = find_func(target);
        g_recent[g_nrecent++ & 15] = target;
        if (i >= 0 && !g_rc_native[i]) {
            target = g_recomp_funcs[i].fn(c);
            continue;
        }
        /* A function whose entry mml.dll may patch with a JMP checks for one and follows it. When
           the mod left it alone (the intro detour without intro.mpg), the JMP there is our root's,
           and following it would re-enter the same check: run the root's function past it. */
#ifndef MML_RUNNER
        if (i < 0 && native_root(c, &target))
            continue;
#endif
        /* one of the exe's import thunks (JMP [slot]): where the slot points now (Windows, or the
           function a mod put there), so a shim or registered function is found by that address */
        if (i < 0 && target >= g_exe_lo && target < g_exe_hi && LD8(target) == 0xFF && LD8(target + 1) == 0x25) {
            uint32_t slot = LD32(target + 2);
            if (slot >= g_exe_lo && slot < g_exe_hi) {
                target = LD32(slot);
                continue;
            }
        }
#ifndef MML_RUNNER
        if (g_census) {
            uint32_t caller = LD32(R32(c, REG_ESP));
            shim_fn sf;
            int sn;
            if (i >= 0)
                census(CK_MODDED, target, caller);
            else if (shim_find(target, &sf, &sn))
                census(CK_SHIM, target, caller);
            else if (rt_direct_has(target))
                census(CK_DIRECT, target, caller);
            else if (rt_impl_has(target))
                census(CK_IMPL, target, caller);
            else
                census(host_callable(target) ? CK_HOST : CK_NATIVE, target, caller);
        }
        if (c->log) {   /* diff mode can't undo what machine code does */
            if (g_rt_errors++ < 20)
                log_printf("recomp: diff run reached machine code at %08X", target);
            return 0;
        }
#endif
        /* a jump into the game's code that no generated function starts at: the machine code there
           may not return to us (system DLL entries reached through import thunks are fine) */
        if (i < 0 && target >= g_recomp_funcs[0].lo && target < g_recomp_funcs[g_recomp_nfuncs - 1].hi &&
            g_rt_errors++ < 20)
            log_printf("recomp: jump to %08X, outside every recompiled function", target);
        if (rt_shim(c, target) || rt_direct(c, target) || rt_impl(c, target))
            ;
#ifndef MML_RUNNER
        else if (host_callable(target))
            rt_host_call(c, target);
#endif
        else
            rt_call_native(c, target);   /* the runner: none, so it stops there (recomp_native.c) */
        target = 0;
    }
    return 0;
}

void rt_call(RCtx *c, uint32_t target)
{
    rt_dispatch(c, target);
}

/* a jump out of the function: like a call, except the return address on the stack is our
   caller's, which the machine code returns through */
void rt_jump(RCtx *c, uint32_t target)
{
    rt_call(c, target);
}

#ifdef MML_RUNNER
/* Nothing translated or replaced is at target, and the runner has no machine code to run
   instead (the DLL's bridge does, recomp_native.c): say what and where, then stop. */
void rt_call_native(RCtx *c, uint32_t target)
{
    uint32_t esp = R32(c, REG_ESP);
    unsigned k;
    log_printf("recomp: nothing runs %08X (no recompiled function, shim or registered routine), called from %08X",
               target, LD32(esp));
    for (k = 1; k <= 16; k++)
        log_printf("recomp:   recent target %08X", g_recent[(g_nrecent - k) & 15]);
    guest_dump();
    ExitProcess(3);
}
#endif

/* ---- mml.dll's hooks, called from recompiled code ---- */

uint32_t flags_from_ctx(const RCtx *c)
{
    return 0x202u | R8(c, 0x200) | (uint32_t)R8(c, 0x202) << 2 | (uint32_t)R8(c, 0x204) << 4 |
           (uint32_t)R8(c, 0x206) << 6 | (uint32_t)R8(c, 0x207) << 7 | (uint32_t)R8(c, REG_DF) << 10 |
           (uint32_t)R8(c, 0x20B) << 11;
}

void flags_to_ctx(RCtx *c, uint32_t f)
{
    R8_SET(c, 0x200, (uint8_t)(f & 1));
    R8_SET(c, 0x202, (uint8_t)(f >> 2 & 1));
    R8_SET(c, 0x204, (uint8_t)(f >> 4 & 1));
    R8_SET(c, 0x206, (uint8_t)(f >> 6 & 1));
    R8_SET(c, 0x207, (uint8_t)(f >> 7 & 1));
    R8_SET(c, REG_DF, (uint8_t)(f >> 10 & 1));
    R8_SET(c, 0x20B, (uint8_t)(f >> 11 & 1));
}

void rt_hook(RCtx *c, uint32_t site)
{
    hook_fn fn = hook_find(site);
    regs_t r;
    if (!fn)
        return;   /* that setting is off this session */
    CENSUS(CK_HOOK, site, LD32(R32(c, REG_ESP)));
    r.eax = R32(c, REG_EAX), r.ecx = R32(c, REG_ECX), r.edx = R32(c, REG_EDX), r.ebx = R32(c, REG_EBX);
    r.ebp = R32(c, REG_EBP), r.esi = R32(c, REG_ESI), r.edi = R32(c, REG_EDI);
    r.esp = R32(c, REG_ESP) - 4;   /* as the stub's PUSHAD saw it (handlers use r->esp + 4) */
    r.eflags = flags_from_ctx(c);
    {
        RCtx *outer = g_shim_ctx;
        g_shim_ctx = c;   /* the handler may call the game (recomp_call_watcall) */
        fn(&r);
        g_shim_ctx = outer;
    }
    R32_SET(c, REG_EAX, r.eax), R32_SET(c, REG_ECX, r.ecx), R32_SET(c, REG_EDX, r.edx);
    R32_SET(c, REG_EBX, r.ebx), R32_SET(c, REG_EBP, r.ebp), R32_SET(c, REG_ESI, r.esi);
    R32_SET(c, REG_EDI, r.edi);
    flags_to_ctx(c, r.eflags);
}

uint32_t rt_cpuid(uint32_t leaf)
{
    static uint32_t out;   /* in guest memory: the generated code reads it there */
    int r[4];
    if (!out)
        out = guest_alloc(16);
    __cpuid(r, (int)leaf);
    GVAR(uint32_t, out) = (uint32_t)r[0];   /* EAX, EBX, EDX, ECX */
    GVAR(uint32_t, out + 4) = (uint32_t)r[1];
    GVAR(uint32_t, out + 8) = (uint32_t)r[3];
    GVAR(uint32_t, out + 12) = (uint32_t)r[2];
    return out;
}

uint32_t rt_port_in(RCtx *c, uint32_t port)
{
    (void)c;
    if (g_rt_errors++ < 20)
        log_printf("recomp: port read %X", port & 0xFFFF);
    return 0xFFFFFFFFu;
}

void rt_port_out(RCtx *c, uint32_t port)
{
    (void)c;
    if (g_rt_errors++ < 20)
        log_printf("recomp: port write %X", port & 0xFFFF);
}

void rt_unsupported(RCtx *c, uint32_t at, const char *what)
{
    CENSUS(CK_UNSUPPORTED, at, LD32(R32(c, REG_ESP)));
    if (g_rt_errors++ < 20)
        log_printf("recomp: unsupported at %08X: %s", at, what);
}

/* ---- the program start, as the portable runtime will do it ----
 * The exe's entry point (JMP to the Watcom C runtime start) jumps to start_thunk instead: onto our
 * private host stack, then recomp_start never returns. The game gets a stack of its own (1 MB;
 * the exe asks for 128 KB) and a thread block of its own for FS: (the C runtime keeps its
 * exception frame chain at FS:[0] and finds its stack bounds with VirtualQuery). Every later
 * context (coroutines, machine-code entries) uses the same thread block. */
#define GUEST_STACK (1u << 20)
static uint32_t g_guest_tib;

uint32_t guest_fs_base(void)
{
    return g_guest_tib ? g_guest_tib : (uint32_t)(uintptr_t)NtCurrentTeb();
}

static __declspec(noreturn) void __cdecl recomp_start(void)
{
    RCtx *c = (RCtx *)_alloca(sizeof(RCtx));
    uint32_t base = guest_pages(0, GUEST_STACK, 1), top = base + GUEST_STACK, tib = guest_alloc(0x1000), esp;
    if (!base || !tib) {
        log_printf("recomp: no memory for the game's stack");
        ExitProcess(1);
    }
    memset(c, 0, sizeof *c);
    {   /* the thread runs on our private stack now: exception dispatch checks handler frames
           against the thread block's stack bounds (C++ exceptions inside drivers went unhandled) */
        NT_TIB *t = (NT_TIB *)NtCurrentTeb();
        t->StackBase = (void *)(uintptr_t)g_hs_hi;
        t->StackLimit = (void *)(uintptr_t)(g_hs_lo + 4096);   /* above the guard page */
    }
    g_host_free = (uintptr_t)c - 256;   /* machine-code entries nest below us */
    /* NT_TIB: exception list (none), stack base (top), stack limit, self */
    ST32(c, tib, 0xFFFFFFFFu);
    ST32(c, tib + 4, top);
    ST32(c, tib + 8, base);
    ST32(c, tib + 0x18, tib);
    g_guest_tib = tib;
    R32_SET(c, REG_FS_BASE, tib);
    R16_SET(c, REG_FPUCW, 0x27F);   /* what Windows gives a new thread */
    esp = top - 16;
    ST32(c, esp, 0);   /* the entry point's return address: never used (ExitProcess) */
    R32_SET(c, REG_ESP, esp);
    log_printf("recomp: the game starts recompiled (stack %08X-%08X, thread block %08X)", base, top, tib);
    rt_dispatch(c, G_CRT_START);
    log_printf("recomp: the game's start returned");
    ExitProcess(0);
}

__declspec(naked) void start_thunk(void)
{
    __asm {
        mov esp, g_host_free
        and esp, 0xFFFFFFF0
        call recomp_start
    }
}

/* the runner's start: the same, from C (never returns) */
void recomp_run(void)
{
    start_thunk();
}

/* ---- the game's cooperative threads, on host stacks of their own (costack.h) ----
 *
 * Two thread slots; slot i: state word at 0x5513FC + 8i (0 free, 1 sleeping, 2 ready), sleep
 * counter at 0x5513FE + 8i, saved stack pointer at 0x551400 + 8i; the running slot (times 8)
 * at 0x55140C; each slot's 16 KB stack ends at [0x551414] - 0x4000 i. The game switches
 * between them by loading ESP. Here each slot runs on a host stack of its own (and has its own
 * guest context); the slot fields in memory are kept as the game's code leaves them. */
#define CO_SLOTS 2
#define CO_STATE(i) (*(volatile uint16_t *)HPTR(0x5513FCu + 8u * (unsigned)(i)))
#define CO_COUNT(i) (*(volatile uint16_t *)HPTR(0x5513FEu + 8u * (unsigned)(i)))
#define CO_SP(i) (*(volatile uint32_t *)HPTR(0x551400u + 8u * (unsigned)(i)))
#define CO_CUR (*(volatile uint32_t *)HPTR(0x55140Cu))
#define CO_TOP (*(volatile uint32_t *)HPTR(0x551414u))

static costack *g_co_stack[CO_SLOTS];
static int g_co_fresh[CO_SLOTS];          /* the slot needs a new stack (created, restarted or ended) */
static uint32_t g_co_entry[CO_SLOTS];
static uintptr_t g_co_host_free[CO_SLOTS], g_main_host_free;
static uint16_t g_co_fpucw = 0x027F;
static RCtx g_co_ctx[CO_SLOTS];
static uintptr_t g_co_range[CO_SLOTS][2];   /* each slot's host stack */

static uint32_t co_top(int i)
{
    return CO_TOP - (uint32_t)i * 0x4000u;
}

static int co_slot(void)
{
    return (int)(CO_CUR / 8) & (CO_SLOTS - 1);
}

static void co_pop_return(RCtx *c)   /* the callee's RET */
{
    uint32_t esp = R32(c, REG_ESP);
    c->ret = LD32(esp);
    R32_SET(c, REG_ESP, esp + 4);
}

static void co_to_main(void)
{
    costack_switch(g_co_stack[co_slot()], costack_main());
}

void rt_co_exit(RCtx *c)
{
    int i = co_slot();
    (void)c;
    CO_STATE(i) = 0;
    CO_SP(i) = 0;
    g_co_fresh[i] = 1;
    co_to_main();   /* never resumed: the scheduler drops this stack */
}

static void co_thread(void *param)
{
    int i = (int)(intptr_t)param, fi = find_func(g_co_entry[i]);
    RCtx *c = &g_co_ctx[i];
    {
        size_t lo, hi;
        costack_bounds(g_co_stack[i], &lo, &hi);
        g_co_range[i][0] = lo;
        g_co_range[i][1] = hi;
        g_co_lo = g_co_range[i][0];
        g_co_hi = g_co_range[i][1];
    }
    memset(c, 0, sizeof *c);
    R16_SET(c, REG_FPUCW, g_co_fpucw);
    R32_SET(c, REG_FS_BASE, guest_fs_base());
    c->fsw0 = 0;
    c->fdepth0 = 0;
    if (fi >= 0 && !g_rc_native[fi]) {
        R32_SET(c, REG_ESP, co_top(i));   /* where the game's scheduler leaves ESP for a new thread */
        rt_dispatch_from(c, g_recomp_funcs[fi].fn(c));
    } else {
        R32_SET(c, REG_ESP, co_top(i) - 4);
        rt_call_native(c, g_co_entry[i]);
    }
    if (g_rt_errors++ < 20)
        log_printf("recomp: thread %d (%08X) returned", i, g_co_entry[i]);
    rt_co_exit(c);
}

void rt_co_create(RCtx *c)
{
    int i = (int)R32(c, REG_EAX) & (CO_SLOTS - 1);
    uint32_t sp = co_top(i) - 0x1C;
    CO_STATE(i) = 2;
    CO_SP(i) = sp;
    *(uint32_t *)HPTR(sp + 0x18) = R32(c, REG_EDX);
    g_co_entry[i] = R32(c, REG_EDX);
    g_co_fresh[i] = 1;
    g_co_fpucw = R16(c, REG_FPUCW);
    R32_SET(c, REG_EAX, sp);
    co_pop_return(c);
}

void rt_co_exec(RCtx *c)
{
    int i = co_slot();
    uint32_t sp = co_top(i) - 0x1C;
    CO_STATE(i) = 2;
    CO_SP(i) = sp;
    *(uint32_t *)HPTR(sp + 0x18) = R32(c, REG_EAX);
    g_co_entry[i] = R32(c, REG_EAX);
    g_co_fresh[i] = 1;
    co_to_main();   /* never resumed: the scheduler starts the new entry on a new stack */
}

void rt_co_yield(RCtx *c)
{
    int i = co_slot();
    CO_STATE(i) = 1;
    CO_COUNT(i) = R16(c, REG_EAX);
    CO_SP(i) = R32(c, REG_ESP) - 0x18;
    g_co_host_free[i] = g_host_free;
    co_to_main();
    g_host_free = g_co_host_free[i];
    co_pop_return(c);
}

void rt_co_schedule(RCtx *c)
{
    int i;
    for (i = 0; i < CO_SLOTS; i++) {
        CO_CUR = (uint32_t)i * 8;
        if (CO_STATE(i) == 0) {
            if (g_co_stack[i] && g_co_fresh[i]) {
                costack_free(g_co_stack[i]);
                g_co_stack[i] = NULL;
            }
            continue;
        }
        if (CO_STATE(i) == 1) {
            if (--CO_COUNT(i) != 0)
                continue;
            CO_STATE(i) = 2;
        }
        if (g_co_fresh[i] || !g_co_stack[i]) {
            costack_free(g_co_stack[i]);
            /* 4 MB, fully committed: a stub entered on this stack carries on 256 KB below ESP
               (pick_stack_asm) */
            g_co_stack[i] = costack_new(4u << 20, co_thread, (void *)(intptr_t)i);
            g_co_fresh[i] = 0;
            {
                static int logged;
                if (logged++ < 20)
                    log_printf("recomp: thread %d starts at %08X", i, g_co_entry[i]);
            }
#ifndef MML_RUNNER
            native_co_new(i);
#endif
            if (!g_co_stack[i])
                continue;
        }
        g_main_host_free = g_host_free;
#ifndef MML_RUNNER
        native_co_enter(i);
#endif
        g_in_co = 1;
        g_co_lo = g_co_range[i][0];
        g_co_hi = g_co_range[i][1];
        costack_switch(costack_main(), g_co_stack[i]);
        g_in_co = 0;
#ifndef MML_RUNNER
        native_co_leave(i);
#endif
        g_host_free = g_main_host_free;
    }
    co_pop_return(c);
}

/* while recompiled code is being tried, say where the game crashed */
static LONG CALLBACK on_crash(EXCEPTION_POINTERS *ep)
{
    static int n;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) && n++ < 5) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
#ifdef _M_X64
        uintptr_t pc = ep->ContextRecord->Rip, sp0 = ep->ContextRecord->Rsp;
#else
        uintptr_t pc = ep->ContextRecord->Eip, sp0 = ep->ContextRecord->Esp;
#endif
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)pc, &m) && m)
            GetModuleFileNameA(m, name, sizeof name);
        log_printf("recomp: exception %08X at %p (%s+%X, sp %p, data %p, last call %08X)", code, (void *)pc,
                   strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name, (unsigned)(pc - (uintptr_t)m), (void *)sp0,
                   ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL,
#ifndef MML_RUNNER
                   native_last_target()
#else
                   0u
#endif
        );
        {   /* return addresses into our module near the top of the stack (map them with the .map) */
            const uintptr_t *sp = (const uintptr_t *)sp0;
            char buf[1024];
            size_t k, len = 0;
            buf[0] = 0;
            for (k = 0; k < 400 && len < sizeof buf - 16; k++) {
                uintptr_t v;
                if (IsBadReadPtr(sp + k, sizeof *sp))
                    break;
                v = sp[k] - g_mml_lo;
                if (v < g_mml_hi - g_mml_lo)
                    len += (size_t)_snprintf_s(buf + len, sizeof buf - len, _TRUNCATE, " %X", (unsigned)v);
            }
            log_printf("recomp: stack (our module's offsets):%s", buf);
            len = 0;
            for (k = 0; k < 12 && !IsBadReadPtr(sp + k, sizeof *sp); k++)
                len += (size_t)_snprintf_s(buf + len, sizeof buf - len, _TRUNCATE, " %p", (void *)sp[k]);
            log_printf("recomp: stack words:%s", buf);
            len = 0;
            for (k = 0; k < 16; k++)
                len += (size_t)_snprintf_s(buf + len, sizeof buf - len, _TRUNCATE, " %08X",
                                           g_recent[(g_nrecent - 1 - k) & 15]);
            log_printf("recomp: recent indirect targets (newest first):%s", buf);
#ifdef MML_RUNNER
            if (pc >= G_MEMBASE + g_exe_lo && pc < G_MEMBASE + g_exe_hi)   /* nothing of the game is machine code here */
                log_printf("recomp: our code called the game's %08X as machine code (from %p: mml_run.map); "
                           "it needs recomp_call_watcall / recomp_callback", (unsigned)(pc - G_MEMBASE), (void *)sp[0]);
            else if (pc >= g_exe_lo && pc < g_exe_hi)
                log_printf("recomp: our code called the game's %08X as machine code (from %p: mml_run.map); "
                           "it needs recomp_call_watcall / recomp_callback", (unsigned)pc, (void *)sp[0]);
            guest_dump();
#endif
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* our own module's image (crash reports; the DLL's direct calls) */
static void self_range(void)
{
    MEMORY_BASIC_INFORMATION mi;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS *nt;
    if (!VirtualQuery((LPCVOID)self_range, &mi, sizeof mi))
        return;
    dos = (const IMAGE_DOS_HEADER *)mi.AllocationBase;
    nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)dos + dos->e_lfanew);
    g_mml_lo = (uintptr_t)dos;
    g_mml_hi = g_mml_lo + nt->OptionalHeader.SizeOfImage;
}

void recomp_install(const char *ini)
{
    char mode[32];
    uint8_t *host_stack;
    const size_t host_size = 32u << 20;
    GetPrivateProfileStringA("debug", "recomp", "off", mode, sizeof mode, ini);
    g_mode = _stricmp(mode, "diff") == 0 ? MODE_DIFF : _stricmp(mode, "replace") == 0 ? MODE_REPLACE : MODE_OFF;
#ifdef MML_RUNNER
    g_mode = MODE_REPLACE;   /* there is no machine code to run */
#endif
    if (g_mode == MODE_OFF)
        return;
#ifndef RECOMP_DIFF
    if (g_mode == MODE_DIFF) {
        log_printf("recomp: diff mode needs a build with RECOMP_DIFF=1 (memory writes aren't logged otherwise)");
        return;
    }
#endif
    if (!g_recomp_nfuncs) {
        log_printf("recomp: this build has no recompiled code");
        return;
    }
    build_func_index();
    self_range();
    {   /* the game's image, in game addresses */
#ifdef MML_RUNNER
        const uint8_t *exe = HPTR(0x400000);
#else
        const uint8_t *exe = (const uint8_t *)GetModuleHandleA(NULL);
#endif
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
        g_exe_lo = nt->OptionalHeader.ImageBase;
        g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    }
    g_rc_native = (uint8_t *)calloc((size_t)g_recomp_nfuncs, 1);
    if (!g_rc_native) {
        log_printf("recomp: no memory for the function table");
        return;
    }
    /* our private stack: 32 MB with a guard page at the bottom, so an overflow stops right there */
    host_stack = (uint8_t *)VirtualAlloc(NULL, host_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!host_stack) {
        log_printf("recomp: no memory for the private stack");
        return;
    }
    {
        DWORD old;
        VirtualProtect(host_stack, 4096, PAGE_NOACCESS, &old);
    }
    g_host_free = ((uintptr_t)(host_stack + host_size) - 64) & ~(uintptr_t)15;
    g_hs_lo = (uintptr_t)host_stack;
    g_hs_hi = (uintptr_t)(host_stack + host_size);
    AddVectoredExceptionHandler(0, on_crash);
    if (GetPrivateProfileIntA("debug", "recomp_watch_self", 0, ini)) {   /* who writes our thread table */
        char w[64];
        _snprintf_s(w, sizeof w, _TRUNCATE, "%p,4", (void *)&g_co_entry[0]);
        watch_configure(w);
        log_printf("recomp: watching %s", w);
    }
    if (g_mode == MODE_REPLACE)
        shims_install(ini, NULL);
#ifndef MML_RUNNER
    native_install(ini);   /* entries from machine code, the game's threads, the program start */
#else
    log_printf("recomp: %d generated functions", g_recomp_nfuncs);   /* the runner calls recomp_run */
#endif
}
