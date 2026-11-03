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
 * argument or return their result in ST0.
 *
 * In replace mode, recompiled code can call machine code: Windows, mml.dll, and
 * functions mml.dll patches (those keep running as machine code so our changes
 * still apply, g_rc_native). rt_call_native runs them on the guest stack and
 * comes back; they may call into recompiled functions again, so the private
 * stack is used from g_host_free down. */
#include <windows.h>
#include <intrin.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hook.h"
#include "log.h"
#include "recomp.h"
#include "recomp_rt.h"
#include "shim.h"
#include "watch.h"

#ifdef RECOMP_NONE
static const RFuncInfo g_recomp_funcs[1] = { { 0 } };
static const int g_recomp_nfuncs = 0;
static const int g_recomp_patch_aware = 0;
#else
extern const RFuncInfo g_recomp_funcs[];
extern const int g_recomp_nfuncs;
extern const int g_recomp_patch_aware;   /* the code calls our hooks and reads patched bytes itself */
#endif

#define B2(n) n, n + 1, n + 1, n + 2
#define B4(n) B2(n), B2(n + 1), B2(n + 1), B2(n + 2)
#define B6(n) B4(n), B4(n + 1), B4(n + 1), B4(n + 2)
const uint8_t g_popcnt8[256] = { B6(0), B6(1), B6(1), B6(2) };   /* bits set in each byte value */
#undef B2
#undef B4
#undef B6

/* FNSAVE image: CW at 0, SW at 4, tag word at 8, ST0..ST7 (80-bit) from 28 */
#define FS_SW 4
#define FS_TW 8
#define FS_ST 28
#define MAX_ROOTS 8192
#define MAX_LOG (1 << 20)
#define EXEC_SIZE (1 << 20)   /* stubs + trampolines: 48 bytes per root */

enum { MODE_OFF, MODE_DIFF, MODE_REPLACE };

/* thunk state (the stubs read and write these by name) */
static uint32_t t_id, t_eax, t_ecx, t_edx, t_ebx, t_esp, t_ebp, t_esi, t_edi, t_efl;
static uint32_t t_new_esp, t_target, t_post_addr;
static uint32_t g_host_free;   /* our private stack is free below this */
/* Our own stacks: the private one, and the running thread fiber's. A stub entered while ESP is
   already on one of them (our C code called a game function directly, e.g. from a hook handler)
   carries on 256 KB below ESP: the game code uses the stack from ESP down as its own (its
   arguments are there), and jumping to g_host_free would overwrite the frames in between. */
static uint32_t g_hs_lo, g_hs_hi, g_in_co, g_co_lo, g_co_hi;
uint8_t *g_rc_native;          /* per function (g_recomp_nfuncs): run the machine code instead (RC_CALL) */
static uint8_t t_fsave[108];   /* FNSAVE image: CW, SW, TW, pointers, then ST0..ST7 as 80-bit values */
static uint32_t t_ds, t_es;    /* as the machine code had them (ES is 0 until the Watcom startup code loads
                                  it, and that now runs recompiled): we always hand over flat ones */

static int g_mode;
static uint8_t *g_exec;   /* stubs and trampolines */
static size_t g_exec_used;
static struct {
    int info;              /* index into g_recomp_funcs */
    uint32_t tramp;        /* original entry (stolen bytes + jump back) */
    rfunc fn;              /* what runs instead (the recompiled function, or a runtime replacement) */
} g_roots[MAX_ROOTS];
static int g_nroots;

/* diff mode */
static RCtx g_ctx;
static struct { uint32_t a; uint8_t n, old[8], now[8]; } g_log[MAX_LOG];
static int g_nlog, g_log_overflow;
static int g_busy, g_pending, g_pending_root;
static uint32_t g_real_ret, g_exp[8], g_exp_esp;
static unsigned g_calls, g_ok, g_bad, g_reported;
static uint8_t *g_modded;      /* per function: reaches code mml.dll patched (differs by design) */

static uint8_t *exec_alloc(size_t n)
{
    uint8_t *p;
    if (!g_exec) {
        g_exec = (uint8_t *)VirtualAlloc(NULL, EXEC_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_exec)
            return NULL;
    }
    if (g_exec_used + n > EXEC_SIZE)
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

/* entry address -> function index + 1 (0: none), over the generated code's address range: a
   jump between functions looks its target up on every iteration of the dispatcher */
static uint16_t *g_func_at;
static uint32_t g_func_lo, g_func_hi;

static int find_func_slow(uint32_t a)
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

static int find_func(uint32_t a)
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

static int g_rt_errors;
static int host_callable(uint32_t target);
static void rt_host_call(RCtx *c, uint32_t target);

/* ---- C versions of our assembly routines (recomp.h) ---- */
uint32_t g_rt_bypass;
static struct {
    uint32_t routine, original;
    double (*st0)(double);
    void (*pre)(const uint32_t regs[8]);
    void (*post)(uint32_t eax);
} g_impl[8];
static int g_nimpl;

void recomp_register_st0(void *routine, double (*fn)(double))
{
    if (g_nimpl < 8) {
        g_impl[g_nimpl].routine = (uint32_t)(uintptr_t)routine;
        g_impl[g_nimpl].st0 = fn;
        g_nimpl++;
    }
}

void recomp_register_wrap(void *routine, uint32_t original, void (*pre)(const uint32_t regs[8]),
                          void (*post)(uint32_t eax))
{
    if (g_nimpl < 8) {
        g_impl[g_nimpl].routine = (uint32_t)(uintptr_t)routine;
        g_impl[g_nimpl].original = original;
        g_impl[g_nimpl].pre = pre;
        g_impl[g_nimpl].post = post;
        g_nimpl++;
    }
}

static struct {
    uint32_t fn;
    int nargs;
} g_direct[8];
static int g_ndirect;

void recomp_register_stdcall(void *fn, int nargs)
{
    int k;
    for (k = 0; k < g_ndirect; k++)
        if (g_direct[k].fn == (uint32_t)(uintptr_t)fn)
            return;
    if (g_ndirect < 8 && nargs >= 0 && nargs <= 6) {
        g_direct[g_ndirect].fn = (uint32_t)(uintptr_t)fn;
        g_direct[g_ndirect].nargs = nargs;
        g_ndirect++;
    }
}

typedef uint32_t(__stdcall *std0)(void);
typedef uint32_t(__stdcall *std1)(uint32_t);
typedef uint32_t(__stdcall *std2)(uint32_t, uint32_t);
typedef uint32_t(__stdcall *std3)(uint32_t, uint32_t, uint32_t);
typedef uint32_t(__stdcall *std4)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(__stdcall *std5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t(__stdcall *std6)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* returns 1 if target is a shimmed Windows import (and its shim has run) */
static int rt_shim(RCtx *c, uint32_t target)
{
    shim_fn fn;
    int n;
    uint32_t esp, ret, a[8];
    if (!shim_find(target, &fn, &n))
        return 0;
    esp = R32(c, REG_ESP);
    ret = LD32(esp);
    memcpy(a, HPTR(esp + 4), 4 * (size_t)n);
    R32_SET(c, REG_EAX, fn(a));
    R32_SET(c, REG_ESP, esp + 4 + 4 * (uint32_t)n);   /* stdcall: the shim's arguments are popped */
    c->ret = ret;
    return 1;
}

static int rt_direct_has(uint32_t target)
{
    int k;
    for (k = 0; k < g_ndirect; k++)
        if (g_direct[k].fn == target)
            return 1;
    return 0;
}

/* returns 1 if target is a registered stdcall function (and has been run) */
static int rt_direct(RCtx *c, uint32_t target)
{
    int k;
    for (k = 0; k < g_ndirect; k++) {
        uint32_t esp, ret, r, a[6];
        int n = g_direct[k].nargs;
        if (g_direct[k].fn != target)
            continue;
        esp = R32(c, REG_ESP);
        ret = LD32(esp);
        memcpy(a, HPTR(esp + 4), 4 * (size_t)n);
        switch (n) {
        case 0: r = ((std0)(uintptr_t)target)(); break;
        case 1: r = ((std1)(uintptr_t)target)(a[0]); break;
        case 2: r = ((std2)(uintptr_t)target)(a[0], a[1]); break;
        case 3: r = ((std3)(uintptr_t)target)(a[0], a[1], a[2]); break;
        case 4: r = ((std4)(uintptr_t)target)(a[0], a[1], a[2], a[3]); break;
        case 5: r = ((std5)(uintptr_t)target)(a[0], a[1], a[2], a[3], a[4]); break;
        default: r = ((std6)(uintptr_t)target)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        }
        R32_SET(c, REG_EAX, r);
        R32_SET(c, REG_ESP, esp + 4 + 4 * (uint32_t)n);
        c->ret = ret;
        return 1;
    }
    return 0;
}

static int rt_impl_has(uint32_t target)
{
    int k;
    for (k = 0; k < g_nimpl; k++)
        if (g_impl[k].routine == target)
            return 1;
    return 0;
}

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
            for (i = 0; i < 8; i++)
                regs[i] = R32(c, off[i]);
            g_impl[k].pre(regs);
            g_rt_bypass = g_impl[k].original;
            rt_dispatch(c, g_impl[k].original);   /* it returns to our caller, as the routine would */
            g_rt_bypass = 0;
            g_impl[k].post(R32(c, REG_EAX));
            return 1;
        }
    }
    return 0;
}

static void rt_dispatch_from(RCtx *c, uint32_t target)
{
    if (target)
        rt_dispatch(c, target);
}

static uint32_t g_recent[16];   /* recent indirect / machine-code targets (crash reports) */
static unsigned g_nrecent;

/* ---- [debug] recomp_census=1: everything the recompiled game still hands to machine code ----
 * The inventory for the portable runtime (no x86 to fall back to): every distinct target the
 * recompiled code leaves to something else, every entry into it from machine code, with counts,
 * the first caller and where it lives. Written to recomp_census.txt in the game folder every few
 * seconds; an existing file is read back first, so the inventory grows over play sessions. */
enum { CK_DIRECT, CK_IMPL, CK_HOST, CK_NATIVE, CK_MODDED, CK_ENTRY, CK_UNSUPPORTED, CK_HOOK, CK_SHIM, CK_KINDS };
static const char *const g_ck_names[CK_KINDS] = {
    "direct",        /* our stdcall C function, called from C (recomp_register_stdcall) */
    "impl",          /* C version of one of our assembly routines (recomp_register_st0 / wrap) */
    "host",          /* mml.dll C function through the stack-copying bridge (host_call_asm) */
    "native",        /* machine code: Windows / DirectX, our assembly, untranslated game code */
    "modded",        /* a recompiled game function run as machine code instead (patched by mml.dll) */
    "entry",         /* machine code entering recompiled code (a hooked root) */
    "unsupported",   /* a function the recompiler couldn't translate */
    "hook",          /* one of mml.dll's hooks called from the generated code (C, regs_t) */
    "shim",          /* a Windows import the game calls, run by our own version (shim.c) */
};
enum { CF_GAME = 1, CF_MML = 2, CF_OTHER = 4 };
typedef struct {
    uint32_t addr, caller;
    unsigned n;
    uint8_t kind, from;   /* from: CF_ bits of the callers seen */
} cen_ent;
#define CEN_SIZE 8192
static cen_ent g_cen[CEN_SIZE];
static int g_census, g_cen_used;
static uint32_t g_exe_lo, g_exe_hi;           /* the game's image */
static uint32_t g_mml_lo, g_mml_hi;           /* mml.dll's image */
static char g_cen_path[MAX_PATH];

static int cen_from(uint32_t a)
{
    if (a >= g_exe_lo && a < g_exe_hi)
        return CF_GAME;
    if (a >= g_mml_lo && a < g_mml_hi)
        return CF_MML;
    return CF_OTHER;
}

static cen_ent *cen_slot(int kind, uint32_t addr)
{
    uint32_t h = (addr * 2654435761u ^ (uint32_t)kind * 0x9E3779B1u) & (CEN_SIZE - 1);
    for (;;) {
        cen_ent *e = &g_cen[h];
        if (!e->n || (e->addr == addr && e->kind == kind))
            return e;
        h = (h + 1) & (CEN_SIZE - 1);
    }
}

static void census(int kind, uint32_t addr, uint32_t caller)
{
    cen_ent *e = cen_slot(kind, addr);
    if (!e->n) {
        if (g_cen_used >= CEN_SIZE * 3 / 4)
            return;
        g_cen_used++;
        e->addr = addr;
        e->kind = (uint8_t)kind;
        e->caller = caller;
    }
    e->n++;
    e->from |= (uint8_t)cen_from(caller);
}
#define CENSUS(kind, addr, caller) do { if (g_census) census(kind, addr, caller); } while (0)

/* the exported name at or before address a in module m, and the distance from it */
static const char *nearest_export(HMODULE m, uint32_t a, uint32_t *off)
{
    const uint8_t *base = (const uint8_t *)m;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const IMAGE_EXPORT_DIRECTORY *ed;
    const DWORD *funcs, *names;
    const WORD *ords;
    const char *best = NULL;
    uint32_t rva = a - (uint32_t)(uintptr_t)m, best_rva = 0;
    DWORD i;
    if (!dir->VirtualAddress)
        return NULL;
    ed = (const IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
    funcs = (const DWORD *)(base + ed->AddressOfFunctions);
    names = (const DWORD *)(base + ed->AddressOfNames);
    ords = (const WORD *)(base + ed->AddressOfNameOrdinals);
    for (i = 0; i < ed->NumberOfNames; i++) {
        uint32_t f = funcs[ords[i]];
        if (f <= rva && f >= best_rva && (f < dir->VirtualAddress || f >= dir->VirtualAddress + dir->Size)) {
            best = (const char *)(base + names[i]);
            best_rva = f;
        }
    }
    *off = rva - best_rva;
    return best;
}

/* "module!export+off", "MEGAMAN_NATIVE.EXE:function+off" or "module+rva" */
static void cen_describe(uint32_t a, char *out, size_t n)
{
    HMODULE m;
    char path[MAX_PATH], *name;
    uint32_t off;
    const char *ex;
    int fi;
    if (!a || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)(uintptr_t)a, &m)) {
        _snprintf_s(out, n, _TRUNCATE, "?");
        return;
    }
    GetModuleFileNameA(m, path, sizeof path);
    name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    if (a >= g_exe_lo && a < g_exe_hi && (fi = find_func_slow(a)) >= 0) {
        _snprintf_s(out, n, _TRUNCATE, "%s:%s+%X", name, g_recomp_funcs[fi].name, a - g_recomp_funcs[fi].addr);
        return;
    }
    if (a >= g_mml_lo && a < g_mml_hi) {   /* our own: the map file names it (census_report.py) */
        _snprintf_s(out, n, _TRUNCATE, "%s+%X", name, a - g_mml_lo);
        return;
    }
    ex = nearest_export(m, a, &off);
    if (ex && off < 0x10000)
        _snprintf_s(out, n, _TRUNCATE, "%s!%s+%X", name, ex, off);
    else
        _snprintf_s(out, n, _TRUNCATE, "%s+%X", name, a - (uint32_t)(uintptr_t)m);
}

#define CEN_OLD 2048
static char *g_cen_old[CEN_OLD];   /* "kind | target" of earlier sessions, outside the game's image */
static int g_cen_nold;

static int cen_order(const void *x, const void *y)
{
    const cen_ent *a = *(const cen_ent *const *)x, *b = *(const cen_ent *const *)y;
    if (a->kind != b->kind)
        return a->kind - b->kind;
    return a->n < b->n ? 1 : a->n > b->n ? -1 : 0;
}

static void census_write(void)
{
    static cen_ent *list[CEN_SIZE];
    int i, k = 0;
    FILE *f;
    for (i = 0; i < CEN_SIZE; i++)
        if (g_cen[i].n)
            list[k++] = &g_cen[i];
    qsort(list, (size_t)k, sizeof list[0], cen_order);
    if (fopen_s(&f, g_cen_path, "w") || !f)
        return;
    fprintf(f, "# recomp census: kind addr count caller from | target ; first caller\n"
               "# from: G = game code, M = mml.dll, O = other (Windows, DirectX)\n");
    for (i = 0; i < k; i++) {
        char what[160], who[160];
        cen_describe(list[i]->addr, what, sizeof what);
        cen_describe(list[i]->caller, who, sizeof who);
        fprintf(f, "%s %08X %u %08X %s%s%s | %s ; %s\n", g_ck_names[list[i]->kind], list[i]->addr, list[i]->n,
                list[i]->caller, list[i]->from & CF_GAME ? "G" : "", list[i]->from & CF_MML ? "M" : "",
                list[i]->from & CF_OTHER ? "O" : "", what, who);
    }
    /* earlier sessions' targets outside the game that this one hasn't reached */
    for (i = 0; i < g_cen_nold; i++) {
        int j, seen = 0;
        for (j = 0; j < k && !seen; j++) {
            char what[160];
            size_t kl = strlen(g_ck_names[list[j]->kind]);
            cen_describe(list[j]->addr, what, sizeof what);
            seen = !strncmp(g_cen_old[i], g_ck_names[list[j]->kind], kl) && strstr(g_cen_old[i], what) != NULL;
        }
        if (!seen)
            fputs(g_cen_old[i], f);
    }
    fclose(f);
}

/* an earlier session's file: its entries count too (addresses in mml.dll or system DLLs can
   differ between sessions, so only game addresses and ours by kind are worth merging) */
static void census_load(void)
{
    FILE *f;
    char line[512];
    if (fopen_s(&f, g_cen_path, "r") || !f)
        return;
    while (fgets(line, sizeof line, f)) {
        char kind[32], from[8] = "";
        unsigned addr, n, caller;
        int k;
        if (line[0] == '#' || sscanf_s(line, "%31s %x %u %x %7s", kind, (unsigned)sizeof kind, &addr, &n, &caller,
                                       from, (unsigned)sizeof from) < 4)
            continue;
        for (k = 0; k < CK_KINDS && strcmp(kind, g_ck_names[k]); k++)
            ;
        if (k == CK_KINDS) {
            continue;
        } else if (!(addr >= g_exe_lo && addr < g_exe_hi)) {
            if (g_cen_nold < CEN_OLD)
                g_cen_old[g_cen_nold++] = _strdup(line);   /* kept as text, see census_write */
        } else {
            cen_ent *e = cen_slot(k, addr);
            if (!e->n) {
                if (g_cen_used >= CEN_SIZE * 3 / 4)
                    break;
                g_cen_used++;
                e->addr = addr;
                e->kind = (uint8_t)k;
                e->caller = caller;
            }
            e->n += n;
            e->from |= (uint8_t)((strchr(from, 'G') ? CF_GAME : 0) | (strchr(from, 'M') ? CF_MML : 0) |
                                 (strchr(from, 'O') ? CF_OTHER : 0));
        }
    }
    fclose(f);
}

void recomp_census_tick(void)
{
    static unsigned calls;
    if (g_census && ++calls % 300 == 0)
        census_write();
}

uint32_t rt_dispatch(RCtx *c, uint32_t target)
{
    while (target) {
        int i = find_func(target);
        g_recent[g_nrecent++ & 15] = target;
        if (i >= 0 && !g_rc_native[i]) {
            target = g_recomp_funcs[i].fn(c);
            continue;
        }
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
        /* a jump into the game's code that no generated function starts at: the machine code there
           may not return to us (system DLL entries reached through import thunks are fine) */
        if (i < 0 && target >= g_recomp_funcs[0].lo && target < g_recomp_funcs[g_recomp_nfuncs - 1].hi &&
            g_rt_errors++ < 20)
            log_printf("recomp: jump to %08X, outside every recompiled function", target);
        if (rt_shim(c, target) || rt_direct(c, target) || rt_impl(c, target))
            ;
        else if (host_callable(target))
            rt_host_call(c, target);
        else
            rt_call_native(c, target);
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

/* ---- the native bridge ---- */

static void x87_from_ctx(const RCtx *c, uint8_t *img);
static void x87_to_ctx(RCtx *c, const uint8_t *img);
static int x87_depth(const uint8_t *img);

static uint32_t b_target, b_esp, b_eax, b_ecx, b_edx, b_ebx, b_ebp, b_esi, b_edi;
static uint8_t b_fsave[108];

/* where each open bridge left our stack: bridges nest (machine code calls back into recompiled
   code, which calls machine code again), and each fiber has its own */
typedef struct {
    uint32_t sp[512];
    int depth;
} bridge_stack;
static bridge_stack g_bs_main;
static uint32_t *g_bsp = g_bs_main.sp;   /* the running fiber's */
static int g_bdepth;

static __declspec(naked) void bridge_back(void)
{
    __asm {
        mov b_eax, eax
        mov b_ecx, ecx
        mov b_edx, edx
        mov b_ebx, ebx
        mov b_ebp, ebp
        mov b_esi, esi
        mov b_edi, edi
        mov b_esp, esp
        cld
        fnsave b_fsave
        mov ax, ss
        mov ds, ax
        mov es, ax
        mov eax, g_bdepth
        dec eax
        mov g_bdepth, eax
        mov ecx, g_bsp
        mov esp, [ecx + eax * 4]
        pop g_host_free
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

static __declspec(naked) void bridge_go(void)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        push g_host_free
        mov eax, g_bdepth
        mov ecx, g_bsp
        mov [ecx + eax * 4], esp
        inc eax
        mov g_bdepth, eax
        lea eax, [esp - 256]
        mov g_host_free, eax
        mov ax, ss
        mov ds, ax
        mov es, ax
        frstor b_fsave
        mov eax, b_eax
        mov ecx, b_ecx
        mov edx, b_edx
        mov ebx, b_ebx
        mov ebp, b_ebp
        mov esi, b_esi
        mov edi, b_edi
        mov esp, b_esp
        jmp dword ptr [b_target]
    }
}

static uint32_t g_bridge_ret;   /* the game's return address of the innermost open bridge */

static uint32_t g_hc_after;   /* the return address our direct calls leave (host_call_asm) */

uint32_t __cdecl recomp_return_address(uint32_t seen)
{
    if ((seen == (uint32_t)(uintptr_t)bridge_back || (g_hc_after && seen == g_hc_after)) && g_bridge_ret)
        return g_bridge_ret;
    return seen;
}

/* ---- direct calls into mml.dll ----
   The game calls our C functions: the Direct3D device methods (ddshim and the hooks on its
   vtable), replaced imports (GetTickCount, XInputGetState). Through the bridge each call saved
   and converted the whole x87 state, reloaded segments and switched stacks; those functions are
   plain stdcall/cdecl C and need none of it. They are called straight from C with the game's
   arguments copied over (16 dwords is more than any of them takes), and the bytes the function
   popped (stdcall) are popped from the game's stack too. The exceptions are the targets of our
   code patches (a CALL or JMP pointed at an assembly routine that works on the game's registers
   or x87 stack): those keep the bridge. */
static uint32_t g_asm_targets[64];
static int g_nasm_targets;
static uint32_t hc_fn, hc_args, hc_ecx, hc_edx, hc_eax_out, hc_edx_out, hc_popped;

static __declspec(naked) void host_call_asm(void)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov ebp, esp                    ; (callee-saved: survives a nested direct call)
        mov g_hc_after, offset after_call
        sub esp, 64
        mov esi, hc_args
        mov edi, esp
        mov ecx, 16
        cld
        rep movsd
        mov ebx, esp                    ; the stack as the callee gets it
        mov ecx, hc_ecx
        mov edx, hc_edx
        call dword ptr [hc_fn]
    after_call:
        mov hc_eax_out, eax
        mov hc_edx_out, edx
        mov eax, esp
        sub eax, ebx                    ; what the callee popped
        mov hc_popped, eax
        mov esp, ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

static int host_callable(uint32_t target)
{
    int k;
    if (target < g_mml_lo || target >= g_mml_hi)
        return 0;
    for (k = 0; k < g_nasm_targets; k++)
        if (g_asm_targets[k] == target)
            return 0;
    return 1;
}

static void host_call_setup(void)
{
    MEMORY_BASIC_INFORMATION mi;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS *nt;
    size_t k;
    if (!VirtualQuery((LPCVOID)host_call_setup, &mi, sizeof mi))
        return;
    dos = (const IMAGE_DOS_HEADER *)mi.AllocationBase;
    nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)dos + dos->e_lfanew);
    g_mml_lo = (uint32_t)(uintptr_t)dos;
    g_mml_hi = g_mml_lo + nt->OptionalHeader.SizeOfImage;
    for (k = 0; k < mem_patch_count(); k++) {   /* our CALL / JMP patches: where they point */
        uintptr_t a;
        size_t len;
        const uint8_t *b;
        mem_patch_get(k, &a, &len);
        b = (const uint8_t *)a;
        if (len >= 5 && (b[0] == 0xE8 || b[0] == 0xE9) && g_nasm_targets < 64) {
            uint32_t t = (uint32_t)a + 5 + *(const uint32_t *)(b + 1);
            if (t >= g_mml_lo && t < g_mml_hi)
                g_asm_targets[g_nasm_targets++] = t;
        }
    }
    log_printf("recomp: direct calls into mml.dll (%08X-%08X), %d assembly routines keep the bridge", g_mml_lo,
               g_mml_hi, g_nasm_targets);
}

static void rt_host_call(RCtx *c, uint32_t target)
{
    uint32_t esp = R32(c, REG_ESP), ret = LD32(esp), outer_ret = g_bridge_ret, eax, edx, popped;
    hc_fn = target;
    hc_args = (uint32_t)(uintptr_t)HPTR(esp + 4);
    hc_ecx = R32(c, REG_ECX);
    hc_edx = R32(c, REG_EDX);
    g_bridge_ret = ret;
    host_call_asm();
    shim_set_error(GetLastError());   /* (the game reads it through our GetLastError) */
    eax = hc_eax_out, edx = hc_edx_out, popped = hc_popped;
    g_bridge_ret = outer_ret;
    R32_SET(c, REG_EAX, eax);
    R32_SET(c, REG_EDX, edx);
    R32_SET(c, REG_ESP, esp + 4 + popped);   /* the return address, then what the callee popped */
    c->ret = ret;
}

void rt_call_native(RCtx *c, uint32_t target)
{
    uint32_t outer_ret = g_bridge_ret;
    uint32_t esp = R32(c, REG_ESP), ret = LD32(esp);
    uint8_t img[108];
    if (g_bdepth >= (int)(sizeof g_bs_main.sp / sizeof g_bs_main.sp[0]) - 1) {   /* machine code that never returns */
        log_printf("recomp: native bridges nested %d deep (calling %08X), giving up", g_bdepth, target);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    *(uint32_t *)HPTR(esp) = (uint32_t)(uintptr_t)bridge_back;   /* it returns to us */
    memset(img, 0, sizeof img);
    x87_from_ctx(c, img);
    memcpy(b_fsave, img, sizeof img);
    b_target = target;
    b_esp = esp;
    b_eax = R32(c, REG_EAX), b_ecx = R32(c, REG_ECX), b_edx = R32(c, REG_EDX), b_ebx = R32(c, REG_EBX);
    b_ebp = R32(c, REG_EBP), b_esi = R32(c, REG_ESI), b_edi = R32(c, REG_EDI);
    g_bridge_ret = ret;
    bridge_go();
    shim_set_error(GetLastError());
    g_bridge_ret = outer_ret;
    R32_SET(c, REG_EAX, b_eax), R32_SET(c, REG_ECX, b_ecx), R32_SET(c, REG_EDX, b_edx);
    R32_SET(c, REG_EBX, b_ebx), R32_SET(c, REG_EBP, b_ebp), R32_SET(c, REG_ESI, b_esi);
    R32_SET(c, REG_EDI, b_edi), R32_SET(c, REG_ESP, b_esp);
    memcpy(img, b_fsave, sizeof img);
    x87_to_ctx(c, img);   /* the stack now starts from the machine code's state */
    c->fsw0 = *(uint16_t *)(img + FS_SW);
    c->fdepth0 = x87_depth(img);
    c->fdelta = 0;
    c->ret = ret;
}

/* ---- mml.dll's hooks, called from recompiled code ---- */

static uint32_t flags_from_ctx(const RCtx *c)
{
    return 0x202u | R8(c, 0x200) | (uint32_t)R8(c, 0x202) << 2 | (uint32_t)R8(c, 0x204) << 4 |
           (uint32_t)R8(c, 0x206) << 6 | (uint32_t)R8(c, 0x207) << 7 | (uint32_t)R8(c, REG_DF) << 10 |
           (uint32_t)R8(c, 0x20B) << 11;
}

static void flags_to_ctx(RCtx *c, uint32_t f)
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
    fn(&r);
    R32_SET(c, REG_EAX, r.eax), R32_SET(c, REG_ECX, r.ecx), R32_SET(c, REG_EDX, r.edx);
    R32_SET(c, REG_EBX, r.ebx), R32_SET(c, REG_EBP, r.ebp), R32_SET(c, REG_ESI, r.esi);
    R32_SET(c, REG_EDI, r.edi);
    flags_to_ctx(c, r.eflags);
}

uint32_t rt_cpuid(uint32_t leaf)
{
    static uint32_t out[4];
    int r[4];
    __cpuid(r, (int)leaf);
    out[0] = (uint32_t)r[0];   /* EAX, EBX, EDX, ECX */
    out[1] = (uint32_t)r[1];
    out[2] = (uint32_t)r[3];
    out[3] = (uint32_t)r[2];
    return (uint32_t)(uintptr_t)out;
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

/* ---- x87 state ---- */


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
    int i, depth = x87_depth(img);   /* usually 0 or 1 at a call: only those need converting */
    R16_SET(c, REG_FPUCW, *(const uint16_t *)img);
    for (i = 0; i < 8; i++)
        RF_SET(c, 0x1100 + 0x10 * i, i < depth ? f80_to_d(img + FS_ST + 10 * i) : 0.0);
}

/* the recompiled code's x87 stack into an FNSAVE image */
static void x87_from_ctx(const RCtx *c, uint8_t *img)
{
    unsigned sw = c->fsw0, top = (sw >> 11) & 7, ntop = (top - (unsigned)c->fdelta) & 7, tw = 0, ph;
    int depth = c->fdepth0 + c->fdelta, i;
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
    for (i = 0; i < depth; i++)   /* the empty slots' contents don't matter */
        d_to_f80(RF(c, 0x1100 + 0x10 * i), img + FS_ST + 10 * i);
}

/* ---- the C side of the stubs (runs on our own stack) ---- */

/* the parts of the register space the generated code uses (general registers, segments and
   FS base, flags and temporaries, x87 control/status, x87 stack): clearing just these on
   every entry from machine code instead of the whole context (8.7 KB) */
static const struct { uint16_t lo, hi; } g_ctx_used[] = {
    { 0x0, 0x20 }, { 0x100, 0x118 }, { 0x200, 0x218 }, { 0x280, 0x288 }, { 0x1090, 0x10B0 }, { 0x1100, 0x1180 },
};

static void ctx_from_cpu(RCtx *c)
{
    size_t k;
    for (k = 0; k < sizeof g_ctx_used / sizeof g_ctx_used[0]; k++)
        memset(c->R + g_ctx_used[k].lo, 0, g_ctx_used[k].hi - g_ctx_used[k].lo);
    c->log = 0;
    c->ret = 0;
    c->fdelta = 0;
    c->fsw0 = *(const uint16_t *)(t_fsave + FS_SW);
    c->fdepth0 = x87_depth(t_fsave);
    R32_SET(c, REG_FS_BASE, (uint32_t)(uintptr_t)NtCurrentTeb());
    R32_SET(c, REG_EAX, t_eax);
    R32_SET(c, REG_ECX, t_ecx);
    R32_SET(c, REG_EDX, t_edx);
    R32_SET(c, REG_EBX, t_ebx);
    R32_SET(c, REG_ESP, t_esp);
    R32_SET(c, REG_EBP, t_ebp);
    R32_SET(c, REG_ESI, t_esi);
    R32_SET(c, REG_EDI, t_edi);
    flags_to_ctx(c, t_efl);
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
        uint32_t r[8], saved_free = g_host_free, efl = t_efl;
        RCtx *c = (RCtx *)_alloca(sizeof(RCtx));
        CENSUS(CK_ENTRY, info->addr, LD32(t_esp));
        ctx_from_cpu(c);
        g_host_free = (uint32_t)(uintptr_t)c - 256;   /* nested entries go below us */
        rt_dispatch_from(c, g_roots[root].fn(c));
        g_host_free = saved_free;
        x87_from_ctx(c, t_fsave);   /* (after the call: a nested entry reused the globals) */
        ctx_regs(c, r);
        t_eax = r[0], t_ecx = r[1], t_edx = r[2], t_ebx = r[3];
        t_ebp = r[5], t_esi = r[6], t_edi = r[7];
        t_new_esp = r[4];
        t_target = c->ret;
        t_efl = efl;
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
    g_ctx.log = 1;
    rt_dispatch_from(&g_ctx, info->fn(&g_ctx));
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
        int depth = x87_depth(t_fsave), want = g_ctx.fdepth0 + g_ctx.fdelta;
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

static __declspec(naked) void pick_stack_asm(void)
{
    __asm {
        pop ecx                     /* our return address */
        mov eax, esp
        cmp eax, g_hs_lo
        jb not_private
        cmp eax, g_hs_hi
        jae not_private
        jmp below_here
    not_private:
        cmp g_in_co, 0
        je use_free
        cmp eax, g_co_lo
        jb use_free
        cmp eax, g_co_hi
        jae use_free
    below_here:
        sub eax, 40000h             /* room for the game code using this stack as its own */
        and eax, 0FFFFFFF0h
        mov esp, eax
        jmp ecx
    use_free:
        mov esp, g_host_free
        jmp ecx
    }
}

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
        mov word ptr t_ds, ds
        mov word ptr t_es, es
        mov ax, ss
        mov ds, ax
        mov es, ax
        call pick_stack_asm
        call on_enter
        mov esp, t_new_esp
        mov ax, ss
        mov ds, ax
        mov es, ax
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
        mov word ptr t_ds, ds
        mov word ptr t_es, es
        mov ax, ss
        mov ds, ax
        mov es, ax
        call pick_stack_asm
        call on_post
        mov esp, t_new_esp
        mov ax, ss
        mov ds, ax
        mov es, ax
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

static int install_root_fn(uint32_t addr, rfunc fn);

static int is_root(uint32_t addr)
{
    int r;
    for (r = 0; r < g_nroots; r++)
        if (g_recomp_funcs[g_roots[r].info].addr == addr)
            return 1;
    return 0;
}

static int install_root(uint32_t addr)
{
    return install_root_fn(addr, NULL);
}

static int install_root_bytes(uint32_t addr, rfunc fn, int fi, unsigned steal, const uint8_t *entry_bytes);

static int install_root_fn(uint32_t addr, rfunc fn)
{
    int fi = find_func(addr);
    const RFuncInfo *info;
    if (fi < 0 || g_nroots >= MAX_ROOTS) {
        log_printf("recomp: %08X isn't in the generated code", addr);
        return 0;
    }
    info = &g_recomp_funcs[fi];
    if (info->steal < 5 || !mem_expect(addr, info->entry_bytes, info->steal)) {
        log_printf("recomp: can't hook %s (%08X)", info->name, addr);
        return 0;
    }
    return install_root_bytes(addr, fn, fi, info->steal, info->entry_bytes);
}

/* The exe's entry point (Windows calls it, nothing CALLs it, so the generated table doesn't
   offer it as a hook): JMP to the Watcom C runtime's startup. As a root, the whole program runs
   recompiled from its first instruction, the C runtime's startup included. Replace mode only:
   the trampoline (the original JMP, relative) is never run. */
#define G_ENTRY      0x4FA900u
#define G_CRT_START  0x4FFB41u
static void install_entry_root(void)
{
    uint8_t jmp[5];
    int fi = find_func(G_ENTRY);
    jmp[0] = 0xE9;
    *(int32_t *)(jmp + 1) = (int32_t)(G_CRT_START - (G_ENTRY + 5));
    if (fi < 0 || !mem_expect(G_ENTRY, jmp, 5) || !install_root_bytes(G_ENTRY, NULL, fi, 5, jmp))
        log_printf("recomp: the entry point isn't the expected JMP, the C runtime starts as machine code");
}

static int install_root_bytes(uint32_t addr, rfunc fn, int fi, unsigned steal, const uint8_t *entry_bytes)
{
    int r = g_nroots;
    uint8_t *stub, *tramp, patch[16];
    struct { unsigned steal; const uint8_t *entry_bytes; } x = { steal, entry_bytes }, *info = &x;
    if (g_nroots >= MAX_ROOTS)
        return 0;
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
    g_roots[r].fn = fn ? fn : g_recomp_funcs[fi].fn;
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)((uintptr_t)stub - (addr + 5));
    if (!mem_write(addr, patch, info->steal))
        return 0;
    g_nroots++;
    if (g_mode == MODE_DIFF)
        log_printf("recomp: %s (%08X) checked against the original", g_recomp_funcs[fi].name, addr);
    return 1;
}

/* ---- the game's cooperative threads, on host fibers ----
 *
 * Two thread slots; slot i: state word at 0x5513FC + 8i (0 free, 1 sleeping, 2 ready), sleep
 * counter at 0x5513FE + 8i, saved stack pointer at 0x551400 + 8i; the running slot (times 8)
 * at 0x55140C; each slot's 16 KB stack ends at [0x551414] - 0x4000 i. The game switches
 * between them by loading ESP. Here each slot runs on a fiber (its own host stack and guest
 * context); the slot fields in memory are kept as the game's code leaves them. */
#define CO_SLOTS 2
#define CO_STATE(i) (*(volatile uint16_t *)HPTR(0x5513FCu + 8u * (unsigned)(i)))
#define CO_COUNT(i) (*(volatile uint16_t *)HPTR(0x5513FEu + 8u * (unsigned)(i)))
#define CO_SP(i) (*(volatile uint32_t *)HPTR(0x551400u + 8u * (unsigned)(i)))
#define CO_CUR (*(volatile uint32_t *)HPTR(0x55140Cu))
#define CO_TOP (*(volatile uint32_t *)HPTR(0x551414u))

static void *g_main_fiber, *g_co_fiber[CO_SLOTS];
static int g_co_fresh[CO_SLOTS];          /* the slot needs a new fiber (created, restarted or ended) */
static uint32_t g_co_entry[CO_SLOTS], g_co_host_free[CO_SLOTS], g_main_host_free;
static uint16_t g_co_fpucw = 0x027F;
static RCtx g_co_ctx[CO_SLOTS];
static bridge_stack g_bs_co[CO_SLOTS];
static uint32_t g_co_range[CO_SLOTS][2];   /* each fiber's own stack */

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
    SwitchToFiber(g_main_fiber);
}

void rt_co_exit(RCtx *c)
{
    int i = co_slot();
    (void)c;
    CO_STATE(i) = 0;
    CO_SP(i) = 0;
    g_co_fresh[i] = 1;
    co_to_main();   /* never resumed: the scheduler drops this fiber */
}

static void CALLBACK co_fiber(void *param)
{
    int i = (int)(intptr_t)param, fi = find_func(g_co_entry[i]);
    RCtx *c = &g_co_ctx[i];
    {
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        g_co_range[i][1] = (uint32_t)(uintptr_t)tib->StackBase;
        g_co_range[i][0] = g_co_range[i][1] - (4u << 20);
        g_co_lo = g_co_range[i][0];
        g_co_hi = g_co_range[i][1];
    }
    memset(c, 0, sizeof *c);
    R16_SET(c, REG_FPUCW, g_co_fpucw);
    R32_SET(c, REG_FS_BASE, (uint32_t)(uintptr_t)NtCurrentTeb());
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
    co_to_main();   /* never resumed: the scheduler starts the new entry on a new fiber */
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
    if (!g_main_fiber) {
        g_main_fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        if (!g_main_fiber) {
            log_printf("recomp: no fiber for the main thread (%lu)", GetLastError());
            co_pop_return(c);
            return;
        }
    }
    for (i = 0; i < CO_SLOTS; i++) {
        CO_CUR = (uint32_t)i * 8;
        if (CO_STATE(i) == 0) {
            if (g_co_fiber[i] && g_co_fresh[i]) {
                DeleteFiber(g_co_fiber[i]);
                g_co_fiber[i] = NULL;
            }
            continue;
        }
        if (CO_STATE(i) == 1) {
            if (--CO_COUNT(i) != 0)
                continue;
            CO_STATE(i) = 2;
        }
        if (g_co_fresh[i] || !g_co_fiber[i]) {
            if (g_co_fiber[i])
                DeleteFiber(g_co_fiber[i]);
            /* fully committed: a stub entered on this stack carries on 256 KB below ESP (pick_stack_asm),
               past the guard page that grows a stack on demand */
            g_co_fiber[i] = CreateFiberEx(4 << 20, 4 << 20, FIBER_FLAG_FLOAT_SWITCH, co_fiber, (void *)(intptr_t)i);
            g_co_fresh[i] = 0;
            g_bs_co[i].depth = 0;   /* a new fiber has no open bridges */
            if (!g_co_fiber[i]) {
                log_printf("recomp: can't create a fiber (%lu)", GetLastError());
                continue;
            }
        }
        g_main_host_free = g_host_free;
        g_bs_main.depth = g_bdepth;
        if (g_co_fresh[i] == 0 && g_bs_co[i].depth < 0)
            g_bs_co[i].depth = 0;
        g_bsp = g_bs_co[i].sp;
        g_bdepth = g_bs_co[i].depth;
        g_in_co = 1;
        g_co_lo = g_co_range[i][0];
        g_co_hi = g_co_range[i][1];
        SwitchToFiber(g_co_fiber[i]);
        g_in_co = 0;
        g_bs_co[i].depth = g_bdepth;
        g_bsp = g_bs_main.sp;
        g_bdepth = g_bs_main.depth;
        g_host_free = g_main_host_free;
    }
    co_pop_return(c);
}

/* the same, entered from machine code (hooked entries) */
static uint32_t co_schedule_root(RCtx *c) { rt_co_schedule(c); return 0; }
static uint32_t co_create_root(RCtx *c) { rt_co_create(c); return 0; }
static uint32_t co_yield_root(RCtx *c) { rt_co_yield(c); return 0; }
static uint32_t co_exec_root(RCtx *c) { rt_co_exec(c); return 0; }
static uint32_t co_exit_root(RCtx *c) { rt_co_exit(c); return 0; }

/* functions containing a byte mml.dll patched, and everything that calls them: their recompiled
   copy (made from the unpatched exe) is meant to differ from the running original */
static void mark_modded(void)
{
    size_t k;
    int i, j, changed = 1, n = 0;
    g_modded = (uint8_t *)calloc((size_t)g_recomp_nfuncs, 1);
    if (!g_modded)
        return;
    for (k = 0; k < mem_patch_count(); k++) {
        uintptr_t a;
        size_t len;
        mem_patch_get(k, &a, &len);
        for (i = 0; i < g_recomp_nfuncs; i++)
            if (a < g_recomp_funcs[i].hi && a + len > g_recomp_funcs[i].lo)
                g_modded[i] = 1;
    }
    while (changed) {
        changed = 0;
        for (i = 0; i < g_recomp_nfuncs; i++)
            for (j = 0; !g_modded[i] && j < g_recomp_funcs[i].ncallees; j++)
                if (g_modded[g_recomp_funcs[i].callees[j]])
                    g_modded[i] = 1, changed = 1;
    }
    for (i = 0; i < g_recomp_nfuncs; i++)
        n += g_modded[i];
    log_printf("recomp: %d functions reach code patched by mml.dll (%u patches)", n, (unsigned)mem_patch_count());
    {   /* the list, for the translator (Modernization\recomp_patches.txt: address, length, new bytes) */
        char path[MAX_PATH], *slash;
        FILE *f = NULL;
        GetModuleFileNameA(NULL, path, sizeof path);
        slash = strrchr(path, '\\');
        if (slash)
            strcpy_s(slash + 1, sizeof path - (size_t)(slash + 1 - path), "Modernization\\recomp_patches.txt");
        if (fopen_s(&f, path, "w") == 0 && f) {
            for (k = 0; k < mem_patch_count(); k++) {
                uintptr_t a;
                size_t len, b;
                mem_patch_get(k, &a, &len);
                fprintf(f, "%08X %u %c", (unsigned)a, (unsigned)len, mem_patch_kind(k));
                for (b = 0; b < len && b < 64; b++)
                    fprintf(f, " %02X", ((const uint8_t *)a)[b]);
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }
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
        uint32_t eip = ep->ContextRecord->Eip;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)eip, &m) && m)
            GetModuleFileNameA(m, name, sizeof name);
        log_printf("recomp: exception %08X at %08X (%s+%X, esp %08X, data %08X, last call %08X)", code, eip,
                   strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name, eip - (uint32_t)(uintptr_t)m,
                   ep->ContextRecord->Esp,
                   ep->ExceptionRecord->NumberParameters > 1 ? (unsigned)ep->ExceptionRecord->ExceptionInformation[1] : 0,
                   b_target);
        {   /* return addresses into mml.dll near the top of the stack (map them with mml.map) */
            HMODULE self = NULL;
            const uint32_t *sp = (const uint32_t *)(uintptr_t)ep->ContextRecord->Esp;
            char buf[256];
            size_t k, len = 0;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)on_crash, &self);
            buf[0] = 0;
            for (k = 0; k < 256 && len < sizeof buf - 16; k++) {
                uint32_t v;
                if (IsBadReadPtr(sp + k, 4))
                    break;
                v = sp[k] - (uint32_t)(uintptr_t)self;
                if (v < 0x800000u)
                    len += (size_t)_snprintf_s(buf + len, sizeof buf - len, _TRUNCATE, " %X", v);
            }
            log_printf("recomp: stack (mml.dll offsets):%s", buf);
            len = 0;
            for (k = 0; k < 12 && !IsBadReadPtr(sp + k, 4); k++)
                len += (size_t)_snprintf_s(buf + len, sizeof buf - len, _TRUNCATE, " %08X", sp[k]);
            log_printf("recomp: stack words:%s", buf);
            len = 0;
            for (k = 0; k < 16; k++)
                len += (size_t)_snprintf_s(buf + len, sizeof buf - len, _TRUNCATE, " %08X",
                                           g_recent[(g_nrecent - 1 - k) & 15]);
            log_printf("recomp: recent indirect targets (newest first):%s", buf);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void recomp_install(const char *ini)
{
    static char roots[4096];
    char mode[32], *p, *next = NULL;
    uint8_t *host_stack;
    const size_t host_size = 32u << 20;
    GetPrivateProfileStringA("debug", "recomp", "off", mode, sizeof mode, ini);
    g_mode = _stricmp(mode, "diff") == 0 ? MODE_DIFF : _stricmp(mode, "replace") == 0 ? MODE_REPLACE : MODE_OFF;
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
    host_call_setup();
    {
        const uint8_t *exe = (const uint8_t *)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
        g_exe_lo = (uint32_t)(uintptr_t)exe;
        g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    }
    if (GetPrivateProfileIntA("debug", "recomp_census", 0, ini)) {
        char *slash;
        strcpy_s(g_cen_path, sizeof g_cen_path, ini);
        slash = strrchr(g_cen_path, '\\');
        strcpy_s(slash ? slash + 1 : g_cen_path, sizeof g_cen_path - (size_t)(slash ? slash + 1 - g_cen_path : 0),
                 "recomp_census.txt");
        census_load();
        g_census = 1;
        log_printf("recomp: census on (%s, %d entries from earlier sessions)", g_cen_path, g_cen_used + g_cen_nold);
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
    g_host_free = ((uint32_t)(uintptr_t)(host_stack + host_size) - 64) & ~15u;
    g_hs_lo = (uint32_t)(uintptr_t)host_stack;
    g_hs_hi = (uint32_t)(uintptr_t)(host_stack + host_size);
    AddVectoredExceptionHandler(0, on_crash);
    if (GetPrivateProfileIntA("debug", "recomp_watch_self", 0, ini)) {   /* who writes our thread table */
        char w[64];
        _snprintf_s(w, sizeof w, _TRUNCATE, "%X,4", (unsigned)(uintptr_t)&g_co_entry[0]);
        watch_configure(w);
        log_printf("recomp: watching %s", w);
    }
    t_post_addr = (uint32_t)(uintptr_t)stub_post;
    if (g_mode == MODE_REPLACE)
        shims_install(ini, NULL);
    GetPrivateProfileStringA("debug", "recomp_roots", "", roots, sizeof roots, ini);
    mark_modded();
    if (g_mode == MODE_REPLACE) {   /* the threads run on fibers */
        static const struct { uint32_t a; rfunc fn; } co[] = {
            { 0x4EED08, co_schedule_root }, { 0x4EED79, co_create_root }, { 0x4EEDA4, co_yield_root },
            { 0x4EEDCA, co_exec_root }, { 0x4EEDFD, co_exit_root },
        };
        int k;
        for (k = 0; k < (int)(sizeof co / sizeof co[0]); k++)
            install_root_fn(co[k].a, co[k].fn);        if (GetPrivateProfileIntA("debug", "recomp_entry", 1, ini))
            install_entry_root();
    }
    if (g_mode == MODE_REPLACE && g_modded && g_recomp_patch_aware) {
        memset(g_modded, 0, (size_t)g_recomp_nfuncs);   /* the recompiled code applies our patches itself */
        log_printf("recomp: the generated code applies mml.dll's hooks and patches itself");
    }
    if (g_mode == MODE_REPLACE && g_modded) {
        int i;
        for (i = 0; i < g_recomp_nfuncs; i++)
            g_rc_native[i] = g_modded[i];
    }
    {   /* [debug] recomp_native=lo-hi,...: these run as machine code (for bisecting a bad translation) */
        char ranges[512], *q, *nx = NULL;
        int i, n = 0;
        GetPrivateProfileStringA("debug", "recomp_native", "", ranges, sizeof ranges, ini);
        for (q = strtok_s(ranges, ", ", &nx); q; q = strtok_s(NULL, ", ", &nx)) {
            uint32_t lo = (uint32_t)strtoul(q, &q, 16), hi = *q == '-' ? (uint32_t)strtoul(q + 1, NULL, 16) : lo;
            for (i = 0; i < g_recomp_nfuncs; i++)
                /* by the containing function's start: its extra entry points (jump targets inside it)
                   go with it, or recompiled code would jump into machine code mid-function. Code
                   that is only jumped to (shared tails) stays recompiled for the same reason. */
                if (g_recomp_funcs[i].lo >= lo && g_recomp_funcs[i].lo <= hi && !g_rc_native[i] &&
                    find_func(g_recomp_funcs[i].lo) >= 0 && g_recomp_funcs[find_func(g_recomp_funcs[i].lo)].steal) {
                    g_rc_native[i] = 1;
                    if (g_modded)
                        g_modded[i] = 1;   /* and not hooked */
                    n++;
                }
        }
        if (n)
            log_printf("recomp: %d functions forced to machine code%s", n,
#ifdef RECOMP_BISECT
                       ""
#else
                       " (only where reached by jumps or dynamic calls: direct calls need a RECOMP_BISECT=1 build)"
#endif
            );
    }
    if (_stricmp(roots, "all") == 0) {   /* every function that can be hooked (diff: pure and not modded) */
        int i;
        for (i = 0; i < g_recomp_nfuncs; i++)
            if (g_recomp_funcs[i].steal >= 5 && !g_modded[i] && (g_mode == MODE_REPLACE || g_recomp_funcs[i].pure) &&
                !is_root(g_recomp_funcs[i].addr))
                install_root(g_recomp_funcs[i].addr);
    } else {
        for (p = strtok_s(roots, ", ", &next); p; p = strtok_s(NULL, ", ", &next))
            install_root((uint32_t)strtoul(p, NULL, 16));
    }
    log_printf("recomp: %d generated functions, %d roots hooked", g_recomp_nfuncs, g_nroots);
}
