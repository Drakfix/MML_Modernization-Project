/* Harness for recompiled game code (tools/recomp/recomp.py, recomp_rt.h).
 *
 * [debug] recomp=diff|replace, recomp_roots=0xADDR[,0xADDR...] (entries of the
 * generated table). The entry of each root is patched with a jump to a small
 * stub, which hands the CPU state to C:
 *   - replace: the recompiled function runs instead of the original, and the
 *     game continues at its return address with the resulting registers.
 *   - diff: the recompiled function runs first with every memory write
 *     logged, the writes are undone, then the original runs from a trampoline.
 *     Its return address is swapped for ours, so when it returns we compare
 *     registers and every byte the recompiled run wrote. Mismatches are logged.
 *
 * The recompiled code pushes onto the game's stack (guest ESP is the real
 * ESP), so the stubs keep their state in globals and run the C side on a
 * private stack: nothing of ours lives below the game's stack pointer. They
 * also FNSAVE the x87 state (which hands our C code a clean FPU) and pass the
 * x87 stack to and from the recompiled code: several math helpers take their
 * argument or return their result in ST0. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hook.h"
#include "log.h"
#include "recomp.h"
#include "recomp_rt.h"

#ifdef RECOMP_NONE
static const RFuncInfo g_recomp_funcs[1] = { { 0 } };
static const int g_recomp_nfuncs = 0;
#else
extern const RFuncInfo g_recomp_funcs[];
extern const int g_recomp_nfuncs;
#endif

#define MAX_ROOTS 64
#define MAX_LOG 65536

enum { MODE_OFF, MODE_DIFF, MODE_REPLACE };

/* thunk state (the stubs read and write these by name) */
static uint32_t t_id, t_eax, t_ecx, t_edx, t_ebx, t_esp, t_ebp, t_esi, t_edi, t_efl;
static uint32_t t_new_esp, t_target, t_host_top, t_post_addr;
static uint8_t t_fsave[108];   /* FNSAVE image: CW, SW, TW, pointers, then ST0..ST7 as 80-bit values */

static int g_mode;
static uint8_t *g_exec;   /* stubs and trampolines */
static size_t g_exec_used;
static struct {
    int info;              /* index into g_recomp_funcs */
    uint32_t tramp;        /* original entry (stolen bytes + jump back) */
} g_roots[MAX_ROOTS];
static int g_nroots;

/* diff mode */
static RCtx g_ctx;
static struct { uint32_t a; uint8_t n, old[8], now[8]; } g_log[MAX_LOG];
static int g_nlog, g_log_overflow;
static int g_busy, g_pending, g_pending_root;
static uint32_t g_real_ret, g_exp[8], g_exp_esp;
static int g_depth0;           /* x87 stack depth at the entry */
static unsigned g_calls, g_ok, g_bad, g_reported;

static uint8_t *exec_alloc(size_t n)
{
    uint8_t *p;
    if (!g_exec) {
        g_exec = (uint8_t *)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_exec)
            return NULL;
    }
    if (g_exec_used + n > 65536)
        return NULL;
    p = g_exec + g_exec_used;
    g_exec_used += (n + 15) & ~(size_t)15;
    return p;
}

/* ---- runtime services for the generated code ---- */

void rt_store_log(RCtx *c, uint32_t a, unsigned n)
{
    (void)c;
    if (g_nlog >= MAX_LOG) {
        g_log_overflow = 1;
        return;
    }
    g_log[g_nlog].a = a;
    g_log[g_nlog].n = (uint8_t)n;
    memcpy(g_log[g_nlog].old, HPTR(a), n);
    g_nlog++;
}

double rt_ld80(uint32_t a)
{
    double d;
    const uint8_t *p = HPTR(a);
    __asm {
        mov eax, p
        fld tbyte ptr [eax]
        fstp d
    }
    return d;
}

void rt_st80(RCtx *c, uint32_t a, double v)
{
    uint8_t buf[10];
    uint8_t *p = buf;
    __asm {
        fld v
        mov eax, p
        fstp tbyte ptr [eax]
    }
    if (c->log)
        rt_store_log(c, a, 8), rt_store_log(c, a + 8, 2);
    memcpy(HPTR(a), buf, 10);
}

static int find_func(uint32_t a)
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

static int g_rt_errors;

void rt_call(RCtx *c, uint32_t target)
{
    int i = find_func(target);
    if (i >= 0) {
        g_recomp_funcs[i].fn(c);
        return;
    }
    if (g_rt_errors++ < 20)
        log_printf("recomp: call to %08X, which isn't recompiled", target);
}

void rt_jump(RCtx *c, uint32_t target)
{
    (void)c;
    if (g_rt_errors++ < 20)
        log_printf("recomp: jump to %08X outside the recompiled code", target);
}

void rt_unsupported(RCtx *c, uint32_t at, const char *what)
{
    (void)c;
    if (g_rt_errors++ < 20)
        log_printf("recomp: unsupported at %08X: %s", at, what);
}

/* ---- x87 state ---- */

#define FS_SW 4
#define FS_TW 8
#define FS_ST 28

static double f80_to_d(const uint8_t *p)
{
    double d;
    __asm {
        mov eax, p
        fld tbyte ptr [eax]
        fstp d
    }
    return d;
}

static void d_to_f80(double v, uint8_t *p)
{
    __asm {
        fld v
        mov eax, p
        fstp tbyte ptr [eax]
    }
}

static int x87_depth(const uint8_t *img)
{
    unsigned sw = *(const uint16_t *)(img + FS_SW), tw = *(const uint16_t *)(img + FS_TW), top = (sw >> 11) & 7;
    int i;
    for (i = 0; i < 8; i++)
        if (((tw >> (2 * ((top + i) & 7))) & 3) == 3)
            break;
    return i;
}

static void x87_to_ctx(RCtx *c, const uint8_t *img)
{
    int i;
    R16_SET(c, REG_FPUCW, *(const uint16_t *)img);
    for (i = 0; i < 8; i++)
        RF_SET(c, 0x1100 + 0x10 * i, f80_to_d(img + FS_ST + 10 * i));
}

/* the recompiled code's x87 stack back into an FNSAVE image (replace mode) */
static void x87_from_ctx(const RCtx *c, uint8_t *img, int depth0)
{
    unsigned sw = *(uint16_t *)(img + FS_SW), top = (sw >> 11) & 7, ntop = (top - (unsigned)c->fdelta) & 7, tw = 0, ph;
    int depth = depth0 + c->fdelta, i;
    if (depth < 0)
        depth = 0;
    if (depth > 8)
        depth = 8;
    *(uint16_t *)img = R16(c, REG_FPUCW);
    *(uint16_t *)(img + FS_SW) = (uint16_t)((sw & ~0x3800u) | (ntop << 11));
    for (ph = 0; ph < 8; ph++)
        if ((int)((ph - ntop) & 7) >= depth)
            tw |= 3u << (2 * ph);
    *(uint16_t *)(img + FS_TW) = (uint16_t)tw;
    for (i = 0; i < 8; i++)
        d_to_f80(RF(c, 0x1100 + 0x10 * i), img + FS_ST + 10 * i);
}

/* ---- the C side of the stubs (runs on our own stack) ---- */

static void ctx_from_cpu(RCtx *c)
{
    memset(c, 0, sizeof *c);
    R32_SET(c, REG_EAX, t_eax);
    R32_SET(c, REG_ECX, t_ecx);
    R32_SET(c, REG_EDX, t_edx);
    R32_SET(c, REG_EBX, t_ebx);
    R32_SET(c, REG_ESP, t_esp);
    R32_SET(c, REG_EBP, t_ebp);
    R32_SET(c, REG_ESI, t_esi);
    R32_SET(c, REG_EDI, t_edi);
    R8_SET(c, REG_DF, (uint8_t)((t_efl >> 10) & 1));
    x87_to_ctx(c, t_fsave);
}

static void ctx_regs(const RCtx *c, uint32_t r[8])
{
    static const unsigned off[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI };
    int i;
    for (i = 0; i < 8; i++)
        r[i] = R32(c, off[i]);
}

static void __cdecl on_enter(void)
{
    int root = (int)t_id, fi = g_roots[root].info;
    const RFuncInfo *info = &g_recomp_funcs[fi];
    t_new_esp = t_esp;
    t_target = g_roots[root].tramp;
    if (g_mode == MODE_REPLACE) {
        uint32_t r[8];
        ctx_from_cpu(&g_ctx);
        g_depth0 = x87_depth(t_fsave);
        info->fn(&g_ctx);
        x87_from_ctx(&g_ctx, t_fsave, g_depth0);
        ctx_regs(&g_ctx, r);
        t_eax = r[0], t_ecx = r[1], t_edx = r[2], t_ebx = r[3];
        t_ebp = r[5], t_esi = r[6], t_edi = r[7];
        t_new_esp = r[4];
        t_target = g_ctx.ret;
        g_calls++;
        return;
    }
    /* diff: run ours with writes logged, undo them, let the original run */
    if (g_busy)
        return;
    g_busy = 1;
    g_nlog = 0;
    g_log_overflow = 0;
    ctx_from_cpu(&g_ctx);
    g_depth0 = x87_depth(t_fsave);
    g_ctx.log = 1;
    info->fn(&g_ctx);
    ctx_regs(&g_ctx, g_exp);
    g_exp_esp = g_exp[4];
    {
        int i;
        for (i = 0; i < g_nlog; i++)
            memcpy(g_log[i].now, HPTR(g_log[i].a), g_log[i].n);
        for (i = g_nlog - 1; i >= 0; i--)
            memcpy(HPTR(g_log[i].a), g_log[i].old, g_log[i].n);
    }
    g_real_ret = LD32(t_esp);
    if (g_ctx.ret != g_real_ret && g_reported++ < 20)
        log_printf("recomp diff %s: returned to %08X, expected %08X", info->name, g_ctx.ret, g_real_ret);
    *(uint32_t *)HPTR(t_esp) = t_post_addr;   /* the original returns to us */
    g_pending = 1;
    g_pending_root = root;
}

static void __cdecl on_post(void)
{
    static const char *names[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    const RFuncInfo *info = &g_recomp_funcs[g_roots[g_pending_root].info];
    uint32_t got[8];
    int i, bad = 0;
    char msg[512];
    size_t m = 0;
    got[0] = t_eax, got[1] = t_ecx, got[2] = t_edx, got[3] = t_ebx;
    got[4] = t_esp, got[5] = t_ebp, got[6] = t_esi, got[7] = t_edi;
    msg[0] = 0;
    for (i = 0; i < 8; i++)
        if (got[i] != g_exp[i]) {
            bad++;
            if (m < sizeof msg - 64)
                m += (size_t)_snprintf_s(msg + m, sizeof msg - m, _TRUNCATE, " %s=%08X/%08X", names[i], got[i],
                                         g_exp[i]);
        }
    for (i = 0; i < g_nlog; i++) {
        /* the stack below the original's final ESP is scratch: both sides used it the same way, but our
           own stub wrote there once the original returned */
        if (g_log[i].a < got[4] && g_log[i].a >= got[4] - 0x10000)
            continue;
        if (memcmp(HPTR(g_log[i].a), g_log[i].now, g_log[i].n) != 0) {
            bad++;
            if (m < sizeof msg - 64) {
                uint64_t want = 0, have = 0;
                memcpy(&want, g_log[i].now, g_log[i].n);
                memcpy(&have, HPTR(g_log[i].a), g_log[i].n);
                m += (size_t)_snprintf_s(msg + m, sizeof msg - m, _TRUNCATE, " [%08X]=%llX/%llX", g_log[i].a,
                                         have, want);
            }
        }
    }
    {   /* x87: stack depth and the values left on it */
        int depth = x87_depth(t_fsave), want = g_depth0 + g_ctx.fdelta;
        if (depth != want) {
            bad++;
            if (m < sizeof msg - 64)
                m += (size_t)_snprintf_s(msg + m, sizeof msg - m, _TRUNCATE, " x87depth=%d/%d", depth, want);
        } else {
            for (i = 0; i < depth && i < g_ctx.fdelta; i++) {
                double have = f80_to_d(t_fsave + FS_ST + 10 * i), exp = RF(&g_ctx, 0x1100 + 0x10 * i);
                if (have != exp && !(have != have && exp != exp)) {
                    bad++;
                    if (m < sizeof msg - 64)
                        m += (size_t)_snprintf_s(msg + m, sizeof msg - m, _TRUNCATE, " st%d=%.17g/%.17g", i, have,
                                                 exp);
                }
            }
        }
    }
    g_calls++;
    if (bad || g_log_overflow) {
        g_bad++;
        if (g_reported++ < 40)
            log_printf("recomp diff %s MISMATCH (game/ours):%s%s", info->name, msg,
                       g_log_overflow ? " (write log overflowed)" : "");
    } else {
        g_ok++;
    }
    if (g_calls % 1000 == 0)
        log_printf("recomp diff: %u calls, %u identical, %u different", g_calls, g_ok, g_bad);
    t_new_esp = t_esp;
    t_target = g_real_ret;
    g_pending = 0;
    g_busy = 0;
}

/* ---- the stubs ---- */

static __declspec(naked) void stub_enter(void)
{
    __asm {
        pop t_id
        mov t_eax, eax
        mov t_ecx, ecx
        mov t_edx, edx
        mov t_ebx, ebx
        mov t_ebp, ebp
        mov t_esi, esi
        mov t_edi, edi
        mov t_esp, esp
        pushfd
        pop t_efl
        cld
        fnsave t_fsave
        mov esp, t_host_top
        call on_enter
        mov esp, t_new_esp
        frstor t_fsave
        push t_efl
        popfd
        mov eax, t_eax
        mov ecx, t_ecx
        mov edx, t_edx
        mov ebx, t_ebx
        mov ebp, t_ebp
        mov esi, t_esi
        mov edi, t_edi
        jmp dword ptr [t_target]
    }
}

static __declspec(naked) void stub_post(void)
{
    __asm {
        mov t_eax, eax
        mov t_ecx, ecx
        mov t_edx, edx
        mov t_ebx, ebx
        mov t_ebp, ebp
        mov t_esi, esi
        mov t_edi, edi
        mov t_esp, esp
        pushfd
        pop t_efl
        cld
        fnsave t_fsave
        mov esp, t_host_top
        call on_post
        mov esp, t_new_esp
        frstor t_fsave
        push t_efl
        popfd
        mov eax, t_eax
        mov ecx, t_ecx
        mov edx, t_edx
        mov ebx, t_ebx
        mov ebp, t_ebp
        mov esi, t_esi
        mov edi, t_edi
        jmp dword ptr [t_target]
    }
}

static int install_root(uint32_t addr)
{
    int fi = find_func(addr), r = g_nroots;
    const RFuncInfo *info;
    uint8_t *stub, *tramp, patch[16];
    if (fi < 0 || g_nroots >= MAX_ROOTS) {
        log_printf("recomp: %08X isn't in the generated code", addr);
        return 0;
    }
    info = &g_recomp_funcs[fi];
    if (info->steal < 5 || !mem_expect(addr, info->entry_bytes, info->steal)) {
        log_printf("recomp: can't hook %s (%08X)", info->name, addr);
        return 0;
    }
    stub = exec_alloc(16);
    tramp = exec_alloc(32);
    if (!stub || !tramp)
        return 0;
    /* stub: push root index; jmp stub_enter */
    stub[0] = 0x68;
    *(uint32_t *)(stub + 1) = (uint32_t)r;
    stub[5] = 0xE9;
    *(int32_t *)(stub + 6) = (int32_t)((uintptr_t)stub_enter - (uintptr_t)(stub + 10));
    /* trampoline: the stolen instructions, then back into the original */
    memcpy(tramp, info->entry_bytes, info->steal);
    tramp[info->steal] = 0xE9;
    *(int32_t *)(tramp + info->steal + 1) = (int32_t)((addr + info->steal) - ((uintptr_t)tramp + info->steal + 5));
    g_roots[r].info = fi;
    g_roots[r].tramp = (uint32_t)(uintptr_t)tramp;
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)((uintptr_t)stub - (addr + 5));
    if (!mem_write(addr, patch, info->steal))
        return 0;
    g_nroots++;
    log_printf("recomp: %s (%08X) %s", info->name, addr, g_mode == MODE_DIFF ? "checked against the original" : "replaced");
    return 1;
}

/* while recompiled code is being tried, say where the game crashed */
static LONG CALLBACK on_crash(EXCEPTION_POINTERS *ep)
{
    static int n;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) && n++ < 5)
        log_printf("recomp: exception %08X at %08X (esp %08X, data %08X)", code, ep->ContextRecord->Eip,
                   ep->ContextRecord->Esp,
                   ep->ExceptionRecord->NumberParameters > 1 ? (unsigned)ep->ExceptionRecord->ExceptionInformation[1] : 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

void recomp_install(const char *ini)
{
    char mode[32], roots[512], *p, *next = NULL;
    static uint8_t host_stack[1 << 20];
    GetPrivateProfileStringA("debug", "recomp", "off", mode, sizeof mode, ini);
    g_mode = _stricmp(mode, "diff") == 0 ? MODE_DIFF : _stricmp(mode, "replace") == 0 ? MODE_REPLACE : MODE_OFF;
    if (g_mode == MODE_OFF)
        return;
    if (!g_recomp_nfuncs) {
        log_printf("recomp: this build has no recompiled code");
        return;
    }
    t_host_top = ((uint32_t)(uintptr_t)(host_stack + sizeof host_stack) - 64) & ~15u;
    AddVectoredExceptionHandler(0, on_crash);
    t_post_addr = (uint32_t)(uintptr_t)stub_post;
    GetPrivateProfileStringA("debug", "recomp_roots", "", roots, sizeof roots, ini);
    if (_stricmp(roots, "all") == 0) {   /* every function that can be hooked */
        int i;
        for (i = 0; i < g_recomp_nfuncs; i++)
            if (g_recomp_funcs[i].steal >= 5)
                install_root(g_recomp_funcs[i].addr);
    } else {
        for (p = strtok_s(roots, ", ", &next); p; p = strtok_s(NULL, ", ", &next))
            install_root((uint32_t)strtoul(p, NULL, 16));
    }
    log_printf("recomp: %d generated functions, %d roots hooked", g_recomp_nfuncs, g_nroots);
}
