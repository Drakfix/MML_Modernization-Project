/* Shim table: see shim.h. On Windows the exe's imports are bound by the loader, so a shim is
 * keyed by what its import slot holds and by the exe's thunks that jump through that slot; the
 * game's machine code that still runs (its startup) keeps calling Windows itself. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "recomp.h"
#include "shim.h"

#define SHIM_SLOTS 1024u
static struct {
    uint32_t addr;
    shim_fn fn;
    int nargs;
} g_shims[SHIM_SLOTS];
static int g_nshims;

static uint32_t g_last_error;
static char g_game_dir[MAX_PATH];

const char *shim_game_dir(void)
{
    return g_game_dir;
}

void shim_set_error(uint32_t e)
{
    g_last_error = e;
}

static uint32_t sh_get_last_error(const uint32_t *a)
{
    (void)a;
    return g_last_error;
}

static uint32_t sh_set_last_error(const uint32_t *a)
{
    g_last_error = a[0];
    return 0;
}

static unsigned slot_of(uint32_t addr)
{
    return (addr * 0x9E3779B1u) >> 22;   /* 10 bits */
}

static void put(uint32_t addr, shim_fn fn, int nargs)
{
    unsigned i = slot_of(addr);
    while (g_shims[i].addr && g_shims[i].addr != addr)
        i = (i + 1) & (SHIM_SLOTS - 1);
    if (!g_shims[i].addr)
        g_nshims++;
    g_shims[i].addr = addr;
    g_shims[i].fn = fn;
    g_shims[i].nargs = nargs;
}

int shim_find(uint32_t target, shim_fn *fn, int *nargs)
{
    unsigned i = slot_of(target);
    if (!g_nshims)
        return 0;
    while (g_shims[i].addr) {
        if (g_shims[i].addr == target) {
            *fn = g_shims[i].fn;
            *nargs = g_shims[i].nargs;
            return 1;
        }
        i = (i + 1) & (SHIM_SLOTS - 1);
    }
    return 0;
}

static const uint8_t *g_exe;
static const IMAGE_NT_HEADERS *g_nt;
#ifndef MML_RUNNER
static uint32_t g_self_lo, g_self_hi;   /* mml.dll's image */
#endif

/* a slot holding one of our routines: a mod replaced the import (recomp.h: in mml.dll our own
   address, in the runner a stand-in for it, never in our image's range) */
static int ours(uint32_t a)
{
#ifdef MML_RUNNER
    return recomp_routine_host(a) != NULL;
#else
    return a >= g_self_lo && a < g_self_hi;
#endif
}

/* the exe's import slot for dll!name, or 0 */
static uint32_t import_slot(const char *dll, const char *name)
{
    const IMAGE_DATA_DIRECTORY *dir = &g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const IMAGE_IMPORT_DESCRIPTOR *d;
    for (d = (const IMAGE_IMPORT_DESCRIPTOR *)(g_exe + dir->VirtualAddress); d->Name; d++) {
        const IMAGE_THUNK_DATA32 *names, *slots;
        if (_stricmp((const char *)(g_exe + d->Name), dll) != 0)
            continue;
        names = (const IMAGE_THUNK_DATA32 *)(g_exe + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        slots = (const IMAGE_THUNK_DATA32 *)(g_exe + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            const IMAGE_IMPORT_BY_NAME *ibn;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                if (name[0] == '#' && (uint32_t)atoi(name + 1) == IMAGE_ORDINAL32(names->u1.Ordinal))
                    return GADDR(&slots->u1.Function);
                continue;
            }
            ibn = (const IMAGE_IMPORT_BY_NAME *)(g_exe + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, name) == 0)
                return GADDR(&slots->u1.Function);
        }
    }
    return 0;
}

/* method addresses: a reserved, inaccessible range nothing else uses */
#define METHOD_SPACE 0x10000u
static uint32_t g_method_base, g_methods;

uint32_t shim_method(int nargs, shim_fn fn)
{
    uint32_t a;
    if (!g_method_base)
        g_method_base = guest_pages(0, METHOD_SPACE, 0);   /* reserved, never touched: only a key */
    if (!g_method_base || g_methods >= METHOD_SPACE / 4) {
        log_printf("shims: no room for another method");
        return 0;
    }
    a = g_method_base + 4 * g_methods++;
    put(a, fn, nargs);
    return a;
}

void shim_add_target(uint32_t addr, int nargs, shim_fn fn)
{
    put(addr, fn, nargs);
}

int shim_add(const char *dll, const char *name, int nargs, shim_fn fn)
{
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(g_nt);
    uint32_t slot = import_slot(dll, name), real;
    int k;
    if (!slot) {
        log_printf("shims: the game doesn't import %s!%s", dll, name);
        return 0;
    }
    real = GVAR(uint32_t, slot);
    if (ours(real)) {
        /* one of our mods replaced this import (GetTickCount: the frame pacing); its function
           stays what the game calls */
        log_printf("shims: %s!%s is hooked by mml.dll, left to the hook", dll, name);
        return 0;
    }
#ifdef MML_RUNNER
    GVAR(uint32_t, slot) = shim_method(nargs, fn);   /* nothing bound the import: ours is the function */
#else
    put(real, fn, nargs);
#endif
    /* thunks in the code sections: JMP [slot] (FF 25 slot) */
    for (k = 0; k < g_nt->FileHeader.NumberOfSections; k++, sec++) {
        const uint8_t *p, *end;
        if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE))
            continue;
        p = g_exe + sec->VirtualAddress;
        /* the exe's headers say VirtualSize 0 for every section (Windows maps SizeOfRawData) */
        end = p + (sec->Misc.VirtualSize > sec->SizeOfRawData ? sec->Misc.VirtualSize : sec->SizeOfRawData) - 6;
        for (; p < end; p++)
            if (p[0] == 0xFF && p[1] == 0x25 && *(const uint32_t *)(p + 2) == slot)
                put(GADDR(p), fn, nargs);
    }
    return 1;
}

#ifdef MML_RUNNER
static uint32_t sh_missing(const uint32_t *a)
{
    static int logged;
    (void)a;
    if (logged++ < 10)
        log_printf("shims: the game called an import we don't have (see the list at start)");
    shim_set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

static uint32_t sh_missing_com(const uint32_t *a)   /* the DirectX entry points return an HRESULT */
{
    sh_missing(a);
    return 0x80004005u;   /* E_FAIL */
}

/* every import nothing bound (still the loader's name reference) goes to sh_missing */
static void bind_missing(void)
{
    const IMAGE_DATA_DIRECTORY *dir = &g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const IMAGE_IMPORT_DESCRIPTOR *d;
    int n = 0;
    for (d = (const IMAGE_IMPORT_DESCRIPTOR *)(g_exe + dir->VirtualAddress); d->Name; d++) {
        const IMAGE_THUNK_DATA32 *names = (const IMAGE_THUNK_DATA32 *)(g_exe + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32 *slots = (IMAGE_THUNK_DATA32 *)(g_exe + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            int dummy_n;
            shim_fn dummy;
            if (slots->u1.Function != names->u1.AddressOfData || shim_find(slots->u1.Function, &dummy, &dummy_n))
                continue;   /* bound */
            /* stdcall argument count unknown: the stub pops none (none of these is expected) */
            {
                const char *dll = (const char *)(g_exe + d->Name);
                int com = _strnicmp(dll, "DSOUND", 6) == 0 || _strnicmp(dll, "DINPUT", 6) == 0 ||
                          _strnicmp(dll, "DDRAW", 5) == 0;
                slots->u1.Function = shim_method(0, com ? sh_missing_com : sh_missing);
            }
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))
                log_printf("shims: no %s!#%u", (const char *)(g_exe + d->Name), IMAGE_ORDINAL32(names->u1.Ordinal));
            else
                log_printf("shims: no %s!%s", (const char *)(g_exe + d->Name),
                           (const char *)((const IMAGE_IMPORT_BY_NAME *)(g_exe + names->u1.AddressOfData))->Name);
            n++;
        }
    }
    log_printf("shims: %d imports have no version of ours", n);
}
#endif

void shims_install(const char *ini, const char *game_dir)
{
    (void)game_dir;
    if (!GetPrivateProfileIntA("debug", "shims", 1, ini)) {
        log_printf("shims: off, the recompiled game calls Windows itself");
        return;
    }
    if (game_dir)
        strcpy_s(g_game_dir, sizeof g_game_dir, game_dir);
    else
        GetCurrentDirectoryA(sizeof g_game_dir, g_game_dir);
#ifdef MML_RUNNER
    g_exe = (const uint8_t *)GUEST(0x400000);   /* the image the runner loaded */
#else
    g_exe = (const uint8_t *)GetModuleHandleA(NULL);
#endif
    g_nt = (const IMAGE_NT_HEADERS *)(g_exe + ((const IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
#ifndef MML_RUNNER
    {
        HMODULE self;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)shims_install, &self)) {
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)self +
                                                                    ((const IMAGE_DOS_HEADER *)self)->e_lfanew);
            g_self_lo = (uint32_t)(uintptr_t)self;
            g_self_hi = g_self_lo + nt->OptionalHeader.SizeOfImage;
        }
    }
#endif
    shim_add("KERNEL32.dll", "GetLastError", 0, sh_get_last_error);
    shim_add("KERNEL32.dll", "SetLastError", 1, sh_set_last_error);
    shim_kernel32_install();
    shim_process_install();
    shim_user32_install();
    /* the events are DirectSound's: Windows' DirectSound needs Windows' events */
    if (GetPrivateProfileIntA("debug", "shim_dsound", 1, ini) && shim_dsound_install(ini, g_game_dir)) {
        shim_sync_install();
        log_printf("shims: DirectSound is ours (SDL3 audio)");
    }
    if (GetPrivateProfileIntA("debug", "shim_dinput", 1, ini) && shim_dinput_install())
        log_printf("shims: DirectInput is ours (keyboard, SDL controller)");
    shim_kernel32_log(GetPrivateProfileIntA("debug", "shim_log", 0, ini));
#ifdef MML_RUNNER
    bind_missing();
#endif
    log_printf("shims: %d call targets run our own Windows functions", g_nshims);
}
