/* KERNEL32 shims: files on the C library (the same code serves the portable runtime) and memory.
 * The game reaches files through its C runtime (open/read/lseek/close over CreateFileA,
 * ReadFile, SetFilePointer, CloseHandle), so the Windows semantics it relies on are the ones
 * kept here: creation dispositions, the last-error codes the runtime turns into errno, a read
 * at the end giving 0 bytes. Handles we didn't make (events, standard handles, files the
 * game's own startup opened as machine code) go to Windows. */
#define _CRT_SECURE_NO_WARNINGS     /* fopen / stat: the portable runtime uses these as they are */
#define _CRT_NONSTDC_NO_WARNINGS
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"
#include "recomp_rt.h"
#include "shim.h"

#define MAX_FILES  64
#define HANDLE_BASE 0x7F000000u   /* far above any kernel handle value */
static FILE *g_files[MAX_FILES];
static int g_log;

static void put32(uint32_t guest, uint32_t v)
{
    memcpy(HPTR(guest), &v, 4);
}

static FILE *file_of(uint32_t h)
{
    uint32_t i = (h - HANDLE_BASE) / 4;
    if (h < HANDLE_BASE || (h & 3) || i >= MAX_FILES)
        return NULL;
    return g_files[i];
}

/* the game's path as a host path (Windows: as it is; elsewhere: '/' separators) */
static void host_path(uint32_t guest, char *out, size_t n)
{
    const char *p = (const char *)HPTR(guest);
#ifdef _WIN32
    size_t k;
    for (k = 0; k + 1 < n && p[k]; k++)
        out[k] = p[k];
    out[k] = 0;
#else
    /* the path as the system has it: any letter case, and the version's own folder for its files
       (compat_set_edition_dir) */
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s", compat_path(p, out, n));
    snprintf(out, n, "%s", tmp);
#endif
}

#ifdef _WIN32
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

/* a missing file, or a missing folder on the way to it */
static uint32_t not_found(const char *path)
{
    char dir[MAX_PATH];
    struct stat st;
    char *cut;
    strcpy_s(dir, sizeof dir, path);
    cut = strrchr(dir, '\\');
    if (!cut || strrchr(dir, '/') > cut)
        cut = strrchr(dir, '/');
    if (!cut)
        return ERROR_FILE_NOT_FOUND;
    *cut = 0;
    return stat(dir, &st) == 0 ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
}

static uint32_t error_from_errno(void)
{
    switch (errno) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case EACCES: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    case EMFILE: return ERROR_TOO_MANY_OPEN_FILES;
    default: return ERROR_GEN_FAILURE;
    }
}

/* CreateFileA(path, access, share, security, disposition, flags, template) */
static uint32_t sh_create_file(const uint32_t *a)
{
    char path[MAX_PATH];
    uint32_t access = a[1], disp = a[4];
    int rd = (access & (GENERIC_READ | GENERIC_ALL)) != 0, wr = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;
    struct stat st;
    int exists, i;
    FILE *f = NULL;
    host_path(a[0], path, sizeof path);
    exists = stat(path, &st) == 0;
    for (i = 0; i < MAX_FILES && g_files[i]; i++)
        ;
    if (i == MAX_FILES) {
        shim_set_error(ERROR_TOO_MANY_OPEN_FILES);
        return 0xFFFFFFFFu;
    }
    errno = ENOENT;
    switch (disp) {
    case CREATE_NEW:
        if (exists) {
            shim_set_error(ERROR_FILE_EXISTS);
            return 0xFFFFFFFFu;
        }
        f = fopen(path, rd ? "w+b" : "wb");
        break;
    case CREATE_ALWAYS:
    case TRUNCATE_EXISTING:
        if (disp == TRUNCATE_EXISTING && !exists)
            break;
        f = fopen(path, rd ? "w+b" : "wb");
        break;
    case OPEN_ALWAYS:
        f = exists ? fopen(path, wr ? "r+b" : "rb") : fopen(path, "w+b");
        break;
    default:   /* OPEN_EXISTING */
        if (exists)
            f = fopen(path, wr ? "r+b" : "rb");
        break;
    }
    if (g_log)
        log_printf("shims: CreateFileA \"%s\" access %08X disposition %u -> %s", path, access, disp,
                   f ? "open" : "failed");
    if (!f) {
        shim_set_error(errno == ENOENT ? not_found(path) : error_from_errno());
        return 0xFFFFFFFFu;
    }
    g_files[i] = f;
    shim_set_error(exists && (disp == OPEN_ALWAYS || disp == CREATE_ALWAYS) ? ERROR_ALREADY_EXISTS : 0);
    return HANDLE_BASE + 4u * (uint32_t)i;
}

/* ReadFile / WriteFile(handle, buffer, count, done, overlapped) */
static uint32_t sh_read_file(const uint32_t *a)
{
    FILE *f = file_of(a[0]);
    size_t n;
    if (!f) {
        DWORD done = 0;
        BOOL ok = ReadFile(HOST_HANDLE(a[0]), HPTR(a[1]), a[2], &done, NULL);
        if (a[3])
            put32(a[3], done);
        shim_set_error(ok ? 0 : GetLastError());
        return (uint32_t)ok;
    }
    n = fread(HPTR(a[1]), 1, a[2], f);
    if (a[3])
        put32(a[3], (uint32_t)n);
    if (n < a[2] && ferror(f)) {
        clearerr(f);
        shim_set_error(ERROR_READ_FAULT);
        return 0;
    }
    clearerr(f);
    return 1;
}

static uint32_t sh_write_file(const uint32_t *a)
{
    FILE *f = file_of(a[0]);
    size_t n;
    if (!f) {
        DWORD done = 0;
        BOOL ok = WriteFile(HOST_HANDLE(a[0]), HPTR(a[1]), a[2], &done, NULL);
        if (a[3])
            put32(a[3], done);
        shim_set_error(ok ? 0 : GetLastError());
        return (uint32_t)ok;
    }
    n = fwrite(HPTR(a[1]), 1, a[2], f);
    if (a[3])
        put32(a[3], (uint32_t)n);
    if (n < a[2]) {
        shim_set_error(ERROR_WRITE_FAULT);
        return 0;
    }
    return 1;
}

/* SetFilePointer(handle, low, high (pointer, may be 0), method): the new position's low half.
   With a high half the distance is the 64-bit hi:lo, and the new high half goes back there;
   without one the distance is a signed 32-bit value and the result must stay below 4 GB. */
static uint32_t sh_set_file_pointer(const uint32_t *a)
{
    FILE *f = file_of(a[0]);
    int64_t off = (int32_t)a[1], pos;
    int whence = a[3] == FILE_CURRENT ? SEEK_CUR : a[3] == FILE_END ? SEEK_END : SEEK_SET;
    if (!f) {
        LONG hi = a[2] ? *(LONG *)HPTR(a[2]) : 0;
        DWORD r = SetFilePointer(HOST_HANDLE(a[0]), (LONG)a[1], a[2] ? &hi : NULL, a[3]);
        if (a[2])
            memcpy(HPTR(a[2]), &hi, 4);
        shim_set_error(r == INVALID_SET_FILE_POINTER ? GetLastError() : 0);
        return r;
    }
    if (a[2]) {
        uint32_t hi;
        memcpy(&hi, HPTR(a[2]), 4);
        off = (int64_t)(((uint64_t)hi << 32) | a[1]);
    }
    if (whence == SEEK_CUR)
        off += ftell64(f);
    else if (whence == SEEK_END) {
        int64_t here = ftell64(f), end;
        fseek64(f, 0, SEEK_END);
        end = ftell64(f);
        fseek64(f, here, SEEK_SET);
        off += end;
    }
    if (off < 0 || (!a[2] && off > 0xFFFFFFFFll)) {
        shim_set_error(off < 0 ? ERROR_NEGATIVE_SEEK : ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFFu;
    }
    if (fseek64(f, off, SEEK_SET) != 0 || (pos = ftell64(f)) < 0) {
        shim_set_error(ERROR_NEGATIVE_SEEK);
        return 0xFFFFFFFFu;
    }
    if (a[2])
        put32(a[2], (uint32_t)((uint64_t)pos >> 32));
    shim_set_error(0);
    return (uint32_t)pos;
}

static uint32_t sh_get_file_type(const uint32_t *a)
{
    if (file_of(a[0]))
        return FILE_TYPE_DISK;
    return GetFileType(HOST_HANDLE(a[0]));
}

static uint32_t sh_close_handle(const uint32_t *a)
{
    FILE *f = file_of(a[0]);
    BOOL ok;
    if (shim_event_close(a[0]))
        return 1;
    if (f) {
        fclose(f);
        g_files[(a[0] - HANDLE_BASE) / 4] = NULL;
        return 1;
    }
    ok = CloseHandle(HOST_HANDLE(a[0]));
    shim_set_error(ok ? 0 : GetLastError());
    return (uint32_t)ok;
}

static uint32_t sh_flush_file_buffers(const uint32_t *a)
{
    FILE *f = file_of(a[0]);
    if (f)
        return fflush(f) == 0;
    return (uint32_t)FlushFileBuffers(HOST_HANDLE(a[0]));
}

static uint32_t sh_delete_file(const uint32_t *a)
{
    char path[MAX_PATH];
    host_path(a[0], path, sizeof path);
    if (remove(path) != 0) {
        shim_set_error(error_from_errno());
        return 0;
    }
    return 1;
}

static uint32_t sh_move_file(const uint32_t *a)
{
    char from[MAX_PATH], to[MAX_PATH];
    host_path(a[0], from, sizeof from);
    host_path(a[1], to, sizeof to);
    if (rename(from, to) != 0) {
        shim_set_error(error_from_errno());
        return 0;
    }
    return 1;
}

static uint32_t sh_get_file_attributes(const uint32_t *a)
{
    char path[MAX_PATH];
    struct stat st;
    host_path(a[0], path, sizeof path);
    if (stat(path, &st) != 0) {
        shim_set_error(error_from_errno());
        return INVALID_FILE_ATTRIBUTES;
    }
    return (st.st_mode & S_IFDIR) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
}

/* memory: on 32-bit Windows guest addresses are host addresses, so these hand over to Windows;
   the portable runtime allocates inside its reserved guest block instead (M3 item 4) */
static uint32_t sh_virtual_alloc(const uint32_t *a)
{
#ifdef MML_RUNNER
    uint32_t g = guest_virtual_alloc(a[0], a[1], a[2]);
    shim_set_error(g ? 0 : ERROR_NOT_ENOUGH_MEMORY);
    return g;
#else
    void *p = VirtualAlloc((void *)(uintptr_t)a[0], a[1], a[2], a[3]);
    shim_set_error(p ? 0 : GetLastError());
    return (uint32_t)(uintptr_t)p;
#endif
}

static uint32_t sh_virtual_free(const uint32_t *a)
{
#ifdef MML_RUNNER
    BOOL ok = guest_virtual_free(a[0], a[1], a[2]);
#else
    BOOL ok = VirtualFree((void *)(uintptr_t)a[0], a[1], a[2]);
#endif
    shim_set_error(ok ? 0 : GetLastError());
    return (uint32_t)ok;
}

static uint32_t sh_virtual_query(const uint32_t *a)
{
#ifdef MML_RUNNER
    return guest_virtual_query(a[0], a[1], a[2]);
#else
    return (uint32_t)VirtualQuery((const void *)(uintptr_t)a[0], (MEMORY_BASIC_INFORMATION *)HPTR(a[1]), a[2]);
#endif
}

void shim_kernel32_install(void)
{
    static const struct { const char *name; int nargs; shim_fn fn; } list[] = {
        { "CreateFileA", 7, sh_create_file },
        { "ReadFile", 5, sh_read_file },
        { "WriteFile", 5, sh_write_file },
        { "SetFilePointer", 4, sh_set_file_pointer },
        { "GetFileType", 1, sh_get_file_type },
        { "CloseHandle", 1, sh_close_handle },
        { "FlushFileBuffers", 1, sh_flush_file_buffers },
        { "DeleteFileA", 1, sh_delete_file },
        { "MoveFileA", 2, sh_move_file },
        { "GetFileAttributesA", 1, sh_get_file_attributes },
        { "VirtualAlloc", 4, sh_virtual_alloc },
        { "VirtualFree", 3, sh_virtual_free },
        { "VirtualQuery", 3, sh_virtual_query },
    };
    size_t k;
    for (k = 0; k < sizeof list / sizeof list[0]; k++)
        shim_add("KERNEL32.dll", list[k].name, list[k].nargs, list[k].fn);
}

void shim_kernel32_log(int on)
{
    g_log = on;
}
