/* KERNEL32 shims for the process and the C runtime: what the Watcom runtime's startup asks
 * (version, command line, environment, module name, code pages, standard handles, thread
 * storage) and the rest of its imports. The game runs one thread (its only thread starter,
 * 0x505744, is never called), so critical sections are no-ops and thread storage is one array.
 * Events stay with Windows until DirectSound is ours (it signals them). */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "plat.h"
#include "recomp_rt.h"
#include "shim.h"

#define GAME_BASE 0x400000u

static void put32(uint32_t guest, uint32_t v)
{
    memcpy(HPTR(guest), &v, 4);
}

/* a guest copy of a host string (made once, kept) */
static uint32_t guest_str(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t g = shim_guest_alloc(n);
    if (g)
        memcpy(HPTR(g), s, n);
    return g;
}

static uint32_t guest_wstr(const char *s)
{
    size_t n = strlen(s) + 1, k;
    uint32_t g = shim_guest_alloc(2 * n);
    if (g)
        for (k = 0; k < n; k++) {
            uint16_t w = (uint8_t)s[k];
            memcpy(HPTR(g + 2 * (uint32_t)k), &w, 2);
        }
    return g;
}

/* the exe's full path, as Windows gives it */
static void exe_path(char *out, size_t n)
{
#ifdef _WIN32
    GetModuleFileNameA(NULL, out, (DWORD)n);
#else
    snprintf(out, n, "%s\\MEGAMAN.EXE", shim_game_dir());
#endif
}

static uint32_t sh_get_version(const uint32_t *a)
{
    (void)a;
    return 0x23F00206u;   /* what Windows 8 and later report to programs without a manifest: 6.2.9200 */
}

static uint32_t g_cmdline, g_cmdline_w;

static uint32_t sh_get_command_line_a(const uint32_t *a)
{
    (void)a;
    if (!g_cmdline) {
        char path[MAX_PATH + 2];
        exe_path(path + 1, sizeof path - 2);
        path[0] = '"';
        strcat_s(path, sizeof path, "\"");
        g_cmdline = guest_str(path);
    }
    return g_cmdline;
}

static uint32_t sh_get_command_line_w(const uint32_t *a)
{
    if (!g_cmdline_w)
        g_cmdline_w = guest_wstr((const char *)HPTR(sh_get_command_line_a(a)));
    return g_cmdline_w;
}

static uint32_t sh_get_environment_strings(const uint32_t *a)
{
    static uint32_t env;
    (void)a;
    if (!env)
        env = shim_guest_alloc(2);   /* an empty block: two NULs */
    return env;
}

static uint32_t sh_set_environment_variable(const uint32_t *a)
{
    (void)a;
    return 1;
}

/* GetModuleFileNameA(module, buffer, size): length copied */
static uint32_t sh_get_module_file_name_a(const uint32_t *a)
{
    char path[MAX_PATH];
    size_t n;
    exe_path(path, sizeof path);
    n = strlen(path);
    if (!a[2])
        return 0;
    if (n >= a[2])
        n = a[2] - 1;
    memcpy(HPTR(a[1]), path, n);
    *(char *)HPTR(a[1] + (uint32_t)n) = 0;
    return (uint32_t)n;
}

static uint32_t sh_get_module_file_name_w(const uint32_t *a)
{
    char path[MAX_PATH];
    size_t n, k;
    exe_path(path, sizeof path);
    n = strlen(path);
    if (!a[2])
        return 0;
    if (n >= a[2])
        n = a[2] - 1;
    for (k = 0; k <= n; k++) {
        uint16_t w = k < n ? (uint8_t)path[k] : 0;
        memcpy(HPTR(a[1] + 2 * (uint32_t)k), &w, 2);
    }
    return (uint32_t)n;
}

static uint32_t sh_get_module_handle_a(const uint32_t *a)
{
    if (!a[0])
        return GAME_BASE;
    log_printf("shims: GetModuleHandleA(\"%s\"): not loaded", (const char *)HPTR(a[0]));
    shim_set_error(ERROR_MOD_NOT_FOUND);
    return 0;
}

static uint32_t sh_load_library_a(const uint32_t *a)
{
    log_printf("shims: LoadLibraryA(\"%s\"): no libraries here", (const char *)HPTR(a[0]));
    shim_set_error(ERROR_MOD_NOT_FOUND);
    return 0;
}

static uint32_t sh_get_proc_address(const uint32_t *a)
{
    (void)a;
    shim_set_error(ERROR_PROC_NOT_FOUND);
    return 0;
}

/* no console: no standard handles, and console calls fail */
static uint32_t sh_get_std_handle(const uint32_t *a)
{
    (void)a;
    return 0;
}

static uint32_t sh_true(const uint32_t *a)
{
    (void)a;
    return 1;
}

static uint32_t sh_no_console(const uint32_t *a)
{
    (void)a;
    shim_set_error(ERROR_INVALID_HANDLE);
    return 0;
}

/* code pages: Western European, single byte */
static uint32_t sh_get_acp(const uint32_t *a)
{
    (void)a;
    return 1252;
}

static uint32_t sh_get_oemcp(const uint32_t *a)
{
    (void)a;
    return 437;
}

/* GetCPInfo(codepage, CPINFO *) */
static uint32_t sh_get_cpinfo(const uint32_t *a)
{
    CPINFO ci;
    memset(&ci, 0, sizeof ci);
    ci.MaxCharSize = 1;
    ci.DefaultChar[0] = '?';
    memcpy(HPTR(a[1]), &ci, sizeof ci);
    return 1;
}

/* MultiByteToWideChar(codepage, flags, src, srclen, dst, dstlen): byte -> same-valued char */
static uint32_t sh_multi_byte_to_wide_char(const uint32_t *a)
{
    const uint8_t *src = (const uint8_t *)HPTR(a[2]);
    uint32_t n = (int32_t)a[3] < 0 ? (uint32_t)strlen((const char *)src) + 1 : a[3], k;
    if (!a[5])
        return n;
    if (n > a[5]) {
        shim_set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (k = 0; k < n; k++) {
        uint16_t w = src[k];
        memcpy(HPTR(a[4] + 2 * k), &w, 2);
    }
    return n;
}

/* WideCharToMultiByte(codepage, flags, src, srclen, dst, dstlen, default, used_default) */
static uint32_t sh_wide_char_to_multi_byte(const uint32_t *a)
{
    uint32_t n = a[3], k;
    if ((int32_t)n < 0) {
        uint16_t w;
        for (n = 0;; n++) {
            memcpy(&w, HPTR(a[2] + 2 * n), 2);
            if (!w)
                break;
        }
        n++;
    }
    if (!a[5])
        return n;
    if (n > a[5]) {
        shim_set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (k = 0; k < n; k++) {
        uint16_t w;
        memcpy(&w, HPTR(a[2] + 2 * k), 2);
        *(uint8_t *)HPTR(a[4] + k) = (uint8_t)(w < 256 ? w : '?');
    }
    if (a[7])
        put32(a[7], 0);
    return n;
}

static uint32_t sh_one(const uint32_t *a)
{
    (void)a;
    return 1;
}

static uint32_t sh_zero(const uint32_t *a)
{
    (void)a;
    return 0;
}

static uint32_t sh_get_current_thread(const uint32_t *a)
{
    (void)a;
    return 0xFFFFFFFEu;   /* the pseudo handle */
}

/* thread storage: one thread */
#define TLS_SLOTS 64
static uint32_t g_tls[TLS_SLOTS];
static uint8_t g_tls_used[TLS_SLOTS];

static uint32_t sh_tls_alloc(const uint32_t *a)
{
    uint32_t i;
    (void)a;
    for (i = 0; i < TLS_SLOTS; i++)
        if (!g_tls_used[i]) {
            g_tls_used[i] = 1;
            g_tls[i] = 0;
            return i;
        }
    return 0xFFFFFFFFu;   /* TLS_OUT_OF_INDEXES */
}

static uint32_t sh_tls_free(const uint32_t *a)
{
    if (a[0] >= TLS_SLOTS)
        return 0;
    g_tls_used[a[0]] = 0;
    return 1;
}

static uint32_t sh_tls_get_value(const uint32_t *a)
{
    shim_set_error(0);
    return a[0] < TLS_SLOTS ? g_tls[a[0]] : 0;
}

static uint32_t sh_tls_set_value(const uint32_t *a)
{
    if (a[0] >= TLS_SLOTS)
        return 0;
    g_tls[a[0]] = a[1];
    return 1;
}

static uint32_t sh_create_thread(const uint32_t *a)
{
    (void)a;
    log_printf("shims: the game asked for a thread (none expected)");
    shim_set_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static uint32_t sh_exit_process(const uint32_t *a)
{
    fflush(NULL);
    exit((int)a[0]);   /* (the C library's exit runs our own shutdown handlers too) */
}

static uint32_t sh_exit_thread(const uint32_t *a)
{
    return sh_exit_process(a);   /* the only thread */
}

/* GetCurrentDirectoryA(size, buffer): the game folder */
static uint32_t sh_get_current_directory_a(const uint32_t *a)
{
    const char *d = shim_game_dir();
    uint32_t n = (uint32_t)strlen(d);
    if (a[0] <= n)
        return n + 1;
    memcpy(HPTR(a[1]), d, n + 1);
    return n;
}

/* GetFullPathNameA(name, size, buffer, file_part) */
static uint32_t sh_get_full_path_name_a(const uint32_t *a)
{
    const char *name = (const char *)HPTR(a[0]);
    char full[MAX_PATH * 2], *last;
    uint32_t n;
    if ((name[0] && name[1] == ':') || name[0] == '\\' || name[0] == '/')
        strcpy_s(full, sizeof full, name);
    else
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", shim_game_dir(), name);
    n = (uint32_t)strlen(full);
    if (a[1] <= n)
        return n + 1;
    memcpy(HPTR(a[2]), full, n + 1);
    last = strrchr(full, '\\');
    if (a[3])
        put32(a[3], a[2] + (uint32_t)(last ? last + 1 - full : 0));
    return n;
}

static uint32_t sh_get_tick_count(const uint32_t *a)
{
    (void)a;
    return plat_ticks_ms();
}

static uint32_t sh_get_drive_type_a(const uint32_t *a)
{
    (void)a;
    return DRIVE_FIXED;
}

void shim_process_install(void)
{
    static const struct { const char *name; int nargs; shim_fn fn; } list[] = {
        { "GetVersion", 0, sh_get_version },
        { "GetCommandLineA", 0, sh_get_command_line_a },
        { "GetCommandLineW", 0, sh_get_command_line_w },
        { "GetEnvironmentStrings", 0, sh_get_environment_strings },
        { "SetEnvironmentVariableA", 2, sh_set_environment_variable },
        { "SetEnvironmentVariableW", 2, sh_set_environment_variable },
        { "GetModuleFileNameA", 3, sh_get_module_file_name_a },
        { "GetModuleFileNameW", 3, sh_get_module_file_name_w },
        { "GetModuleHandleA", 1, sh_get_module_handle_a },
        { "LoadLibraryA", 1, sh_load_library_a },
        { "GetProcAddress", 2, sh_get_proc_address },
        { "GetStdHandle", 1, sh_get_std_handle },
        { "SetStdHandle", 2, sh_true },
        { "GetConsoleMode", 2, sh_no_console },
        { "SetConsoleMode", 2, sh_no_console },
        { "ReadConsoleInputA", 4, sh_no_console },
        { "WriteConsoleA", 5, sh_no_console },
        { "SetConsoleCtrlHandler", 2, sh_true },
        { "GetACP", 0, sh_get_acp },
        { "GetOEMCP", 0, sh_get_oemcp },
        { "GetCPInfo", 2, sh_get_cpinfo },
        { "MultiByteToWideChar", 6, sh_multi_byte_to_wide_char },
        { "WideCharToMultiByte", 8, sh_wide_char_to_multi_byte },
        { "GetCurrentThreadId", 0, sh_one },
        { "GetCurrentProcessId", 0, sh_one },
        { "GetCurrentThread", 0, sh_get_current_thread },
        { "SetUnhandledExceptionFilter", 1, sh_zero },
        { "UnhandledExceptionFilter", 1, sh_zero },
        { "TlsAlloc", 0, sh_tls_alloc },
        { "TlsFree", 1, sh_tls_free },
        { "TlsGetValue", 1, sh_tls_get_value },
        { "TlsSetValue", 2, sh_tls_set_value },
        { "InitializeCriticalSection", 1, sh_zero },
        { "DeleteCriticalSection", 1, sh_zero },
        { "EnterCriticalSection", 1, sh_zero },
        { "LeaveCriticalSection", 1, sh_zero },
        { "CreateThread", 6, sh_create_thread },
        { "ExitProcess", 1, sh_exit_process },
        { "ExitThread", 1, sh_exit_thread },
        { "GetCurrentDirectoryA", 2, sh_get_current_directory_a },
        { "GetFullPathNameA", 4, sh_get_full_path_name_a },
        { "GetDriveTypeA", 1, sh_get_drive_type_a },
        { "GetTickCount", 0, sh_get_tick_count },
    };
    size_t k;
    for (k = 0; k < sizeof list / sizeof list[0]; k++)
        shim_add("KERNEL32.dll", list[k].name, list[k].nargs, list[k].fn);
}
