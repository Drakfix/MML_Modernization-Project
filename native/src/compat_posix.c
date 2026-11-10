/* compat/windows.h over POSIX (Android; later Linux and Apple). What our code and the game's shims
 * ask of Windows, done the way Windows does it where the difference shows:
 * - paths: '\' is a separator and every component is found whatever its letter case (the game
 *   names its files in capitals and in any case); GetModuleFileNameA and GetCurrentDirectoryA give
 *   the game folder (compat_set_game_dir), so paths built from them work unchanged;
 * - settings files: GetPrivateProfile* as Windows reads them (whitespace trimmed, no comment
 *   stripping inside values, sections and keys in any case);
 * - handles: small table indices (files, searches, events, threads, timers), never pointers,
 *   since the game keeps some in 32 bits. */
#ifndef _WIN32
#include <windows.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

/* ---------------------------------------------------------------- errors */

static __thread DWORD g_last_error;

DWORD GetLastError(void)
{
    return g_last_error;
}

void SetLastError(DWORD e)
{
    g_last_error = e;
}

static void set_errno_error(void)
{
    switch (errno) {
    case ENOENT: g_last_error = ERROR_FILE_NOT_FOUND; break;
    case ENOTDIR: g_last_error = ERROR_PATH_NOT_FOUND; break;
    case EACCES: case EPERM: case EROFS: g_last_error = ERROR_ACCESS_DENIED; break;
    case EEXIST: g_last_error = ERROR_FILE_EXISTS; break;
    case EMFILE: case ENFILE: g_last_error = ERROR_TOO_MANY_OPEN_FILES; break;
    case ENOMEM: g_last_error = ERROR_NOT_ENOUGH_MEMORY; break;
    case EINVAL: g_last_error = ERROR_INVALID_PARAMETER; break;
    default: g_last_error = ERROR_GEN_FAILURE; break;
    }
}

/* ---------------------------------------------------------------- handles */

enum { H_FREE, H_FILE, H_FIND, H_EVENT, H_THREAD, H_TIMER };

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int manual, signaled;
} event;

typedef struct {
    int kind;
    int fd;                                     /* H_FILE */
    DIR *dir;                                   /* H_FIND */
    char dir_path[MAX_PATH * 2], mask[MAX_PATH];
    event ev;                                   /* H_EVENT, H_THREAD (ended), H_TIMER (due passed) */
    struct timespec due;                        /* H_TIMER */
    int armed;
    pthread_t thread;
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
} handle;

#define MAX_HANDLES 1024
#define HANDLE_BASE 0x1000u
static handle g_h[MAX_HANDLES];
static pthread_mutex_t g_h_lock = PTHREAD_MUTEX_INITIALIZER;

static HANDLE h_new(int kind)
{
    int i;
    pthread_mutex_lock(&g_h_lock);
    for (i = 1; i < MAX_HANDLES && g_h[i].kind != H_FREE; i++)
        ;
    if (i == MAX_HANDLES) {
        pthread_mutex_unlock(&g_h_lock);
        g_last_error = ERROR_TOO_MANY_OPEN_FILES;
        return NULL;
    }
    memset(&g_h[i], 0, sizeof g_h[i]);
    g_h[i].kind = kind;
    g_h[i].fd = -1;
    pthread_mutex_unlock(&g_h_lock);
    return (HANDLE)(uintptr_t)(HANDLE_BASE + 4u * (unsigned)i);
}

static handle *h_get(HANDLE hv, int kind)
{
    uintptr_t v = (uintptr_t)hv;
    unsigned i;
    if (v < HANDLE_BASE || (v - HANDLE_BASE) % 4)
        return NULL;
    i = (unsigned)((v - HANDLE_BASE) / 4);
    if (i >= MAX_HANDLES || g_h[i].kind == H_FREE || (kind && g_h[i].kind != kind))
        return NULL;
    return &g_h[i];
}

static void event_init(event *e, int manual, int signaled)
{
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->manual = manual;
    e->signaled = signaled;
}

static void event_set(event *e)
{
    pthread_mutex_lock(&e->m);
    e->signaled = 1;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}

/* waits until signalled or the absolute deadline (NULL: forever); 1 if signalled */
static int event_wait(event *e, const struct timespec *deadline)
{
    int r = 0;
    pthread_mutex_lock(&e->m);
    while (!e->signaled) {
        if (deadline) {
            if (pthread_cond_timedwait(&e->c, &e->m, deadline) == ETIMEDOUT)
                break;
        } else {
            pthread_cond_wait(&e->c, &e->m);
        }
    }
    if (e->signaled) {
        r = 1;
        if (!e->manual)
            e->signaled = 0;
    }
    pthread_mutex_unlock(&e->m);
    return r;
}

BOOL CloseHandle(HANDLE hv)
{
    handle *h = h_get(hv, 0);
    if (!h) {
        g_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (h->kind == H_FILE && h->fd >= 0)
        close(h->fd);
    if (h->kind == H_FIND && h->dir)
        closedir(h->dir);
    if (h->kind == H_THREAD)
        pthread_detach(h->thread);
    pthread_mutex_lock(&g_h_lock);
    h->kind = H_FREE;   /* (events are left initialised: their memory stays, nothing waits on a closed one) */
    pthread_mutex_unlock(&g_h_lock);
    return TRUE;
}

/* ---------------------------------------------------------------- paths */

static char g_game_dir[MAX_PATH * 2] = ".", g_game_cwd[MAX_PATH * 2];

void compat_set_game_dir(const char *dir)
{
    snprintf(g_game_dir, sizeof g_game_dir, "%s", dir);
    if (chdir(dir) != 0)
        log_printf("compat: can't enter %s", dir);
    if (!getcwd(g_game_cwd, sizeof g_game_cwd))   /* (the same folder as the system spells it) */
        g_game_cwd[0] = 0;
}

/* The Japanese version where the player imports their own files (Android): its program, data and
   saves sit in a folder of their own in the game folder, beside what both versions share (mml.ini,
   Modernization, the log). While it plays, a path to one of its files in the game folder (or
   relative to it, the current folder) goes into that folder. */
static char g_edition_dir[64];

void compat_set_edition_dir(const char *sub)
{
    snprintf(g_edition_dir, sizeof g_edition_dir, "%s", sub ? sub : "");
}

static int edition_file(const char *name, size_t len)
{
    static const char *const N[] = { "DAT", "BGM", "SPH", "CAPCOM.AVI", "ROCKMAN.EXE", "ROCKMAN_NATIVE.EXE",
                                     "ROCKMAN.CFG" };
    size_t i;
    for (i = 0; i < sizeof N / sizeof N[0]; i++)
        if (strlen(N[i]) == len && strncasecmp(name, N[i], len) == 0)
            return 1;
    return len >= 5 && strncasecmp(name, "DASH_", 5) == 0;   /* the saves (and quick-save notes) */
}

static size_t under(const char *p, const char *dir)
{
    size_t n = strlen(dir);
    return n > 1 && strncmp(p, dir, n) == 0 && p[n] == '/' ? n + 1 : 0;
}

/* p, with '/' separators: in the edition's folder if it is one of its files */
static void edition_path(char *p, size_t n)
{
    char tmp[MAX_PATH * 2];
    const char *rel, *end;
    size_t skip = 0;
    if (!g_edition_dir[0])
        return;
    if (p[0] == '/' && !(skip = under(p, g_game_dir)) && !(skip = under(p, g_game_cwd)))
        return;   /* (somewhere else) */
    rel = p + skip;
    while (rel[0] == '.' && rel[1] == '/')
        rel += 2;
    end = strchr(rel, '/');
    if (!end && strcasecmp(rel, "ROCKMAN.CFG") == 0) {
        /* The game's own settings, the controller's button bindings among them: the US version's
           file where it is there, so both versions play with the same bindings (the menus' own
           buttons are [controls] jp_buttons). Without it, the Japanese folder's own file. */
        struct stat st;
        snprintf(tmp, sizeof tmp, "%.*sMEGAMAN.CFG", (int)skip, p);
        if (stat(tmp, &st) == 0) {
            snprintf(p, n, "%s", tmp);
            return;
        }
    }
    if (!edition_file(rel, end ? (size_t)(end - rel) : strlen(rel)))
        return;
    snprintf(tmp, sizeof tmp, "%.*s%s/%s", (int)skip, p, g_edition_dir, rel);
    snprintf(p, n, "%s", tmp);
}

DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n)
{
    (void)m;   /* (the game folder with '\': the code that takes the folder part looks for it) */
    snprintf(buf, n, "%s\\mml_run.exe", g_game_dir);
    return (DWORD)strlen(buf);
}

DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf)
{
    if (!getcwd(buf, n))
        return 0;
    return (DWORD)strlen(buf);
}

BOOL SetCurrentDirectoryA(LPCSTR dir)
{
    char p[MAX_PATH * 2];
    return chdir(compat_path(dir, p, sizeof p)) == 0;
}

/* the entry of dir whose name matches name in any letter case, into out; 0 if none */
static int find_case(const char *dir, const char *name, char *out, size_t n)
{
    DIR *d = opendir(*dir ? dir : "/");
    struct dirent *e;
    int found = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (strcasecmp(e->d_name, name) == 0) {
            snprintf(out, n, "%s", e->d_name);
            found = 1;
            break;
        }
    closedir(d);
    return found;
}

/* a Windows path (\ or /, any case, relative to the current folder) as the real one */
const char *compat_path(const char *win, char *out, size_t n)
{
    char tmp[MAX_PATH * 2], *p, *comp, *save = NULL;
    size_t len = 0;
    struct stat st;
    if (!win || !n)
        return "";
    snprintf(tmp, sizeof tmp, "%s", win);
    for (p = tmp; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (tmp[0] && tmp[1] == ':')   /* a drive letter: nothing like it here */
        memmove(tmp, tmp + 2, strlen(tmp + 2) + 1);
    edition_path(tmp, sizeof tmp);
    if (stat(tmp, &st) == 0 || !tmp[0]) {   /* already right */
        snprintf(out, n, "%s", tmp);
        return out;
    }
    out[0] = 0;
    if (tmp[0] == '/') {
        out[0] = '/';
        out[1] = 0;
        len = 1;
    }
    for (comp = strtok_r(tmp, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        char real[256];
        char probe[MAX_PATH * 2];
        if (strcmp(comp, ".") == 0)
            continue;
        snprintf(probe, sizeof probe, "%s%s", out, comp);
        if (strcmp(comp, "..") != 0 && stat(probe, &st) != 0 &&
            find_case(len ? out : ".", comp, real, sizeof real))
            comp = real;
        len += (size_t)snprintf(out + len, len < n ? n - len : 0, "%s/", comp);
        if (len >= n)
            break;
    }
    if (len > 1 && len < n && out[len - 1] == '/')
        out[len - 1] = 0;   /* (no trailing separator) */
    return out;
}

/* ---------------------------------------------------------------- files */

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                   DWORD flags, HANDLE tmpl)
{
    char p[MAX_PATH * 2];
    int oflags, fd;
    HANDLE hv;
    (void)share; (void)sa; (void)flags; (void)tmpl;
    oflags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
    default: break;   /* OPEN_EXISTING */
    }
    fd = open(compat_path(path, p, sizeof p), oflags | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_errno_error();
        return INVALID_HANDLE_VALUE;
    }
    hv = h_new(H_FILE);
    if (!hv) {
        close(fd);
        return INVALID_HANDLE_VALUE;
    }
    h_get(hv, H_FILE)->fd = fd;
    g_last_error = 0;
    return hv;
}

BOOL ReadFile(HANDLE hv, LPVOID buf, DWORD n, LPDWORD done, void *overlapped)
{
    handle *h = h_get(hv, H_FILE);
    ssize_t r;
    (void)overlapped;
    if (done)
        *done = 0;
    if (!h) {
        g_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    r = read(h->fd, buf, n);
    if (r < 0) {
        g_last_error = ERROR_READ_FAULT;
        return FALSE;
    }
    if (done)
        *done = (DWORD)r;
    return TRUE;
}

BOOL WriteFile(HANDLE hv, LPCVOID buf, DWORD n, LPDWORD done, void *overlapped)
{
    handle *h = h_get(hv, H_FILE);
    ssize_t r;
    (void)overlapped;
    if (done)
        *done = 0;
    if (!h) {
        g_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    r = write(h->fd, buf, n);
    if (r < 0) {
        g_last_error = ERROR_WRITE_FAULT;
        return FALSE;
    }
    if (done)
        *done = (DWORD)r;
    return TRUE;
}

DWORD SetFilePointer(HANDLE hv, LONG dist, PLONG dist_hi, DWORD method)
{
    handle *h = h_get(hv, H_FILE);
    off_t off = dist_hi ? (off_t)(((int64_t)*dist_hi << 32) | (uint32_t)dist) : (off_t)dist, r;
    if (!h) {
        g_last_error = ERROR_INVALID_HANDLE;
        return INVALID_SET_FILE_POINTER;
    }
    r = lseek(h->fd, off, method == FILE_CURRENT ? SEEK_CUR : method == FILE_END ? SEEK_END : SEEK_SET);
    if (r < 0) {
        g_last_error = ERROR_NEGATIVE_SEEK;
        return INVALID_SET_FILE_POINTER;
    }
    if (dist_hi)
        *dist_hi = (LONG)((int64_t)r >> 32);
    g_last_error = 0;
    return (DWORD)r;
}

DWORD GetFileSize(HANDLE hv, LPDWORD hi)
{
    handle *h = h_get(hv, H_FILE);
    struct stat st;
    if (!h || fstat(h->fd, &st) != 0)
        return 0xFFFFFFFFu;
    if (hi)
        *hi = (DWORD)((uint64_t)st.st_size >> 32);
    return (DWORD)st.st_size;
}

DWORD GetFileType(HANDLE hv)
{
    return h_get(hv, H_FILE) ? FILE_TYPE_DISK : 0;
}

BOOL FlushFileBuffers(HANDLE hv)
{
    handle *h = h_get(hv, H_FILE);
    return h && fsync(h->fd) == 0;
}

DWORD GetFileAttributesA(LPCSTR path)
{
    char p[MAX_PATH * 2];
    struct stat st;
    if (stat(compat_path(path, p, sizeof p), &st) != 0) {
        set_errno_error();
        return INVALID_FILE_ATTRIBUTES;
    }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
}

BOOL DeleteFileA(LPCSTR path)
{
    char p[MAX_PATH * 2];
    if (unlink(compat_path(path, p, sizeof p)) != 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL MoveFileA(LPCSTR from, LPCSTR to)
{
    char a[MAX_PATH * 2], b[MAX_PATH * 2];
    struct stat st;
    compat_path(to, b, sizeof b);
    if (stat(b, &st) == 0) {   /* Windows' MoveFile doesn't replace */
        g_last_error = ERROR_ALREADY_EXISTS;
        return FALSE;
    }
    if (rename(compat_path(from, a, sizeof a), b) != 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa)
{
    char p[MAX_PATH * 2];
    (void)sa;
    if (mkdir(compat_path(path, p, sizeof p), 0755) != 0) {
        set_errno_error();
        if (errno == EEXIST)
            g_last_error = ERROR_ALREADY_EXISTS;
        return FALSE;
    }
    return TRUE;
}

static int find_next(handle *h, LPWIN32_FIND_DATAA fd)
{
    struct dirent *e;
    while ((e = readdir(h->dir))) {
        char full[MAX_PATH * 3];
        struct stat st;
        if (fnmatch(h->mask, e->d_name, FNM_CASEFOLD) != 0)
            continue;
        memset(fd, 0, sizeof *fd);
        snprintf(full, sizeof full, "%s/%s", h->dir_path, e->d_name);
        if (stat(full, &st) == 0) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
            fd->nFileSizeLow = (DWORD)st.st_size;
            fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        }
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        return 1;
    }
    return 0;
}

HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd)
{
    char dir[MAX_PATH * 2], full[MAX_PATH * 2], *slash, *q;
    const char *mask;
    HANDLE hv;
    handle *h;
    snprintf(full, sizeof full, "%s", pattern);
    for (q = full; *q; q++)
        if (*q == '\\')
            *q = '/';
    edition_path(full, sizeof full);   /* (DASH_*.MCD: the version's own saves) */
    snprintf(dir, sizeof dir, "%s", full);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mask = full + (slash - dir) + 1;
    } else {
        strcpy(dir, ".");
        mask = full;
    }
    hv = h_new(H_FIND);
    if (!hv)
        return INVALID_HANDLE_VALUE;
    h = h_get(hv, H_FIND);
    compat_path(dir, h->dir_path, sizeof h->dir_path);
    snprintf(h->mask, sizeof h->mask, "%s", strcmp(mask, "*.*") == 0 ? "*" : mask);
    h->dir = opendir(h->dir_path);
    if (!h->dir || !find_next(h, fd)) {
        CloseHandle(hv);
        g_last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    return hv;
}

BOOL FindNextFileA(HANDLE hv, LPWIN32_FIND_DATAA fd)
{
    handle *h = h_get(hv, H_FIND);
    return h && find_next(h, fd);
}

BOOL FindClose(HANDLE hv)
{
    return CloseHandle(hv);
}

/* ---------------------------------------------------------------- settings files */

static char *read_text(const char *file)
{
    char p[MAX_PATH * 2];
    FILE *f = fopen(compat_path(file, p, sizeof p), "rb");
    char *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (buf)
        buf[n] = 0;
    fclose(f);
    return buf;
}

static char *trim(char *s, char *end)
{
    while (s < end && isspace((unsigned char)*s))
        s++;
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = 0;
    return s;
}

/* the value of key in section, into out (n bytes); 1 if found */
static int ini_find(const char *text, const char *sec, const char *key, char *out, size_t n)
{
    const char *line = text;
    int in_sec = 0;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[1024], *s, *eq;
        if (len >= sizeof buf)
            len = sizeof buf - 1;
        memcpy(buf, line, len);
        buf[len] = 0;
        s = trim(buf, buf + len);
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (close) {
                *close = 0;
                in_sec = strcasecmp(trim(s + 1, close), sec) == 0;
            }
        } else if (in_sec && *s != ';' && (eq = strchr(s, '='))) {
            char *k = trim(s, eq), *v = trim(eq + 1, eq + 1 + strlen(eq + 1));
            if (strcasecmp(k, key) == 0) {
                size_t vl = strlen(v);
                if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) {
                    v[vl - 1] = 0;
                    v++;
                }
                snprintf(out, n, "%s", v);
                return 1;
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return 0;
}

DWORD GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR dflt, LPSTR out, DWORD n, LPCSTR file)
{
    char *text;
    if (!out || !n)
        return 0;
    out[0] = 0;
    text = sec && key ? read_text(file) : NULL;
    if (!text || !ini_find(text, sec, key, out, n))
        snprintf(out, n, "%s", dflt ? dflt : "");
    free(text);
    return (DWORD)strlen(out);
}

UINT GetPrivateProfileIntA(LPCSTR sec, LPCSTR key, INT dflt, LPCSTR file)
{
    char v[64];
    char *text = read_text(file);
    UINT r = (UINT)dflt;
    if (text && ini_find(text, sec, key, v, sizeof v))
        r = (UINT)strtol(v, NULL, 10);   /* (Windows: decimal, leading digits) */
    free(text);
    return r;
}

BOOL WritePrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR value, LPCSTR file)
{
    char p[MAX_PATH * 2];
    char *text = read_text(file), *o;
    const char *line;
    size_t cap;
    int in_sec = 0, done = 0, have_sec = 0;
    FILE *f;
    if (!sec || !key)
        return FALSE;
    if (!text)
        text = strdup("");
    cap = strlen(text) + strlen(sec) + strlen(key) + (value ? strlen(value) : 0) + 16;
    o = (char *)malloc(cap);
    if (!o) {
        free(text);
        return FALSE;
    }
    o[0] = 0;
    for (line = text; *line;) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line + 1) : strlen(line);
        char buf[1024], *s, *eq;
        snprintf(buf, sizeof buf, "%.*s", (int)(len < sizeof buf ? len : sizeof buf - 1), line);
        s = trim(buf, buf + strlen(buf));
        if (*s == '[') {
            if (in_sec && !done && value) {   /* the section ends without the key: add it */
                strcat(o, key), strcat(o, "="), strcat(o, value), strcat(o, "\n");
                done = 1;
            }
            in_sec = 0;
            if (strchr(s, ']')) {
                *strchr(s, ']') = 0;
                in_sec = strcasecmp(trim(s + 1, s + strlen(s)), sec) == 0;
                have_sec |= in_sec;
            }
        } else if (in_sec && !done && *s != ';' && (eq = strchr(s, '=')) && strcasecmp(trim(s, eq), key) == 0) {
            if (value)
                strcat(o, key), strcat(o, "="), strcat(o, value), strcat(o, "\n");
            done = 1;
            line += len;
            continue;
        }
        strncat(o, line, len);
        line += len;
    }
    if (!done && value) {
        if (!have_sec)
            strcat(o, "["), strcat(o, sec), strcat(o, "]\n");
        strcat(o, key), strcat(o, "="), strcat(o, value), strcat(o, "\n");
    }
    f = fopen(compat_path(file, p, sizeof p), "wb");
    if (f) {
        fputs(o, f);
        fclose(f);
    }
    free(o);
    free(text);
    return f != NULL;
}

/* ---------------------------------------------------------------- time */

BOOL QueryPerformanceCounter(LARGE_INTEGER *c)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    c->QuadPart = (LONGLONG)t.tv_sec * 1000000000LL + t.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    f->QuadPart = 1000000000LL;
    return TRUE;
}

DWORD GetTickCount(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (DWORD)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

void Sleep(DWORD ms)
{
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
        ;
}

static void deadline_in(struct timespec *d, DWORD ms)
{
    clock_gettime(CLOCK_REALTIME, d);   /* (pthread_cond_timedwait's clock) */
    d->tv_sec += ms / 1000;
    d->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (d->tv_nsec >= 1000000000L)
        d->tv_sec++, d->tv_nsec -= 1000000000L;
}

/* ---------------------------------------------------------------- threads, events, timers */

/* A thread's start lives apart from its handle: as on Windows, the handle may be closed while the
   thread runs (CloseHandle(CreateThread(...))), and a closed slot is soon reused and cleared. At
   its end the thread signals its handle only if the slot is still its own. */
typedef struct {
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
    HANDLE hv;
} thread_start;

static void *thread_main(void *p)
{
    thread_start t = *(thread_start *)p;
    handle *h;
    free(p);
    t.fn(t.arg);
    pthread_mutex_lock(&g_h_lock);
    h = h_get(t.hv, H_THREAD);
    if (h && pthread_equal(h->thread, pthread_self()))
        event_set(&h->ev);
    pthread_mutex_unlock(&g_h_lock);
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags,
                    LPDWORD id)
{
    HANDLE hv = h_new(H_THREAD);
    handle *h = h_get(hv, H_THREAD);
    pthread_attr_t at;
    thread_start *start;
    (void)sa; (void)flags;
    if (!h)
        return NULL;
    event_init(&h->ev, 1, 0);
    h->fn = fn;
    h->arg = arg;
    start = (thread_start *)malloc(sizeof *start);
    if (!start) {
        CloseHandle(hv);
        return NULL;
    }
    start->fn = fn;
    start->arg = arg;
    start->hv = hv;
    pthread_attr_init(&at);
    if (stack)
        pthread_attr_setstacksize(&at, stack < 65536 ? 65536 : stack);
    /* (the slot's lock held: the thread can't look at its handle before h->thread is set) */
    pthread_mutex_lock(&g_h_lock);
    if (pthread_create(&h->thread, &at, thread_main, start) != 0) {
        pthread_mutex_unlock(&g_h_lock);
        free(start);
        pthread_attr_destroy(&at);
        CloseHandle(hv);
        return NULL;
    }
    pthread_mutex_unlock(&g_h_lock);
    pthread_attr_destroy(&at);
    if (id)
        *id = (DWORD)(uintptr_t)h->thread;
    return hv;
}

BOOL SetThreadPriority(HANDLE h, int prio)
{
    (void)h; (void)prio;
    return TRUE;
}

DWORD GetCurrentThreadId(void)
{
    return (DWORD)gettid();
}

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name)
{
    HANDLE hv = h_new(H_EVENT);
    handle *h = h_get(hv, H_EVENT);
    (void)sa; (void)name;
    if (h)
        event_init(&h->ev, manual, initial);
    return hv;
}

BOOL SetEvent(HANDLE hv)
{
    handle *h = h_get(hv, H_EVENT);
    if (!h)
        return FALSE;
    event_set(&h->ev);
    return TRUE;
}

BOOL ResetEvent(HANDLE hv)
{
    handle *h = h_get(hv, H_EVENT);
    if (!h)
        return FALSE;
    pthread_mutex_lock(&h->ev.m);
    h->ev.signaled = 0;
    pthread_mutex_unlock(&h->ev.m);
    return TRUE;
}

HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{
    HANDLE hv = h_new(H_TIMER);
    handle *h = h_get(hv, H_TIMER);
    (void)sa; (void)name; (void)access;
    if (h)
        event_init(&h->ev, flags & 1, 0);   /* (CREATE_WAITABLE_TIMER_MANUAL_RESET) */
    return hv;
}

BOOL SetWaitableTimer(HANDLE hv, const LARGE_INTEGER *due, LONG period, void *fn, LPVOID arg, BOOL resume)
{
    handle *h = h_get(hv, H_TIMER);
    (void)period; (void)fn; (void)arg; (void)resume;
    if (!h || !due)
        return FALSE;
    if (due->QuadPart < 0) {   /* relative, in 100 ns units */
        int64_t ns = -due->QuadPart * 100;
        clock_gettime(CLOCK_MONOTONIC, &h->due);
        h->due.tv_sec += (time_t)(ns / 1000000000LL);
        h->due.tv_nsec += (long)(ns % 1000000000LL);
        if (h->due.tv_nsec >= 1000000000L)
            h->due.tv_sec++, h->due.tv_nsec -= 1000000000L;
    } else {
        clock_gettime(CLOCK_MONOTONIC, &h->due);   /* (absolute times: not used by us) */
    }
    h->armed = 1;
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE hv, DWORD ms)
{
    handle *h = h_get(hv, 0);
    struct timespec d;
    if (!h) {
        g_last_error = ERROR_INVALID_HANDLE;
        return WAIT_FAILED;
    }
    if (h->kind == H_TIMER) {
        if (!h->armed)
            return ms == INFINITE ? WAIT_FAILED : WAIT_TIMEOUT;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &h->due, NULL) == EINTR)
            ;
        h->armed = 0;
        return WAIT_OBJECT_0;
    }
    if (h->kind != H_EVENT && h->kind != H_THREAD)
        return WAIT_FAILED;
    if (ms != INFINITE)
        deadline_in(&d, ms);
    return event_wait(&h->ev, ms == INFINITE ? NULL : &d) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

void InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    cs->impl = m;
}

void DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    if (cs->impl) {
        pthread_mutex_destroy((pthread_mutex_t *)cs->impl);
        free(cs->impl);
        cs->impl = NULL;
    }
}

void EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    pthread_mutex_lock((pthread_mutex_t *)cs->impl);
}

void LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    pthread_mutex_unlock((pthread_mutex_t *)cs->impl);
}

/* SRWLOCK_INIT is all zero: the mutex is made on first use */
static pthread_mutex_t *srw(PSRWLOCK l)
{
    pthread_mutex_t *m = (pthread_mutex_t *)__atomic_load_n(&l->impl, __ATOMIC_ACQUIRE);
    if (!m) {
        pthread_mutex_t *n = (pthread_mutex_t *)malloc(sizeof *n);
        void *expect = NULL;
        pthread_mutex_init(n, NULL);
        if (__atomic_compare_exchange_n(&l->impl, &expect, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            m = n;
        } else {
            pthread_mutex_destroy(n);
            free(n);
            m = (pthread_mutex_t *)expect;
        }
    }
    return m;
}

void AcquireSRWLockExclusive(PSRWLOCK l)
{
    pthread_mutex_lock(srw(l));
}

void ReleaseSRWLockExclusive(PSRWLOCK l)
{
    pthread_mutex_unlock(srw(l));
}

/* ---------------------------------------------------------------- memory */

static int prot_of(DWORD protect)
{
    switch (protect) {
    case PAGE_NOACCESS: return PROT_NONE;
    default: return PROT_READ | PROT_WRITE;   /* (nothing here runs code from its own memory) */
    }
}

#define MAX_MAPS 256
static struct { uintptr_t at; size_t n; } g_maps[MAX_MAPS];
static pthread_mutex_t g_maps_lock = PTHREAD_MUTEX_INITIALIZER;

LPVOID VirtualAlloc(LPVOID at, SIZE_T n, DWORD type, DWORD protect)
{
    void *p;
    int k;
    if (at && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) {   /* commit inside a reservation */
        uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE), lo = (uintptr_t)at & ~(page - 1);
        uintptr_t hi = ((uintptr_t)at + n + page - 1) & ~(page - 1);
        return mprotect((void *)lo, hi - lo, prot_of(protect)) == 0 ? at : NULL;
    }
    p = mmap(at, n, (type & MEM_COMMIT) ? prot_of(protect) : PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        g_last_error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (at && p != at) {   /* Windows gives the address asked for or nothing */
        munmap(p, n);
        g_last_error = ERROR_INVALID_PARAMETER;
        return NULL;
    }
    pthread_mutex_lock(&g_maps_lock);
    for (k = 0; k < MAX_MAPS && g_maps[k].at; k++)
        ;
    if (k < MAX_MAPS)
        g_maps[k].at = (uintptr_t)p, g_maps[k].n = n;
    pthread_mutex_unlock(&g_maps_lock);
    return p;
}

BOOL VirtualFree(LPVOID p, SIZE_T n, DWORD type)
{
    int k;
    if (type & MEM_DECOMMIT) {
        madvise(p, n, MADV_DONTNEED);
        return mprotect(p, n, PROT_NONE) == 0;
    }
    pthread_mutex_lock(&g_maps_lock);
    for (k = 0; k < MAX_MAPS && g_maps[k].at != (uintptr_t)p; k++)
        ;
    if (k < MAX_MAPS) {
        munmap(p, g_maps[k].n);
        g_maps[k].at = 0;
    }
    pthread_mutex_unlock(&g_maps_lock);
    return k < MAX_MAPS;
}

BOOL VirtualProtect(LPVOID p, SIZE_T n, DWORD protect, LPDWORD old)
{
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE), lo = (uintptr_t)p & ~(page - 1);
    uintptr_t hi = ((uintptr_t)p + n + page - 1) & ~(page - 1);
    if (old)
        *old = PAGE_READWRITE;
    return mprotect((void *)lo, hi - lo, prot_of(protect)) == 0;
}

SIZE_T VirtualQuery(LPCVOID p, MEMORY_BASIC_INFORMATION *mi, SIZE_T n)
{
    (void)p; (void)mi; (void)n;
    return 0;   /* (the game's memory has its own: guest_mem.c) */
}

/* ---------------------------------------------------------------- modules, the process */

HMODULE LoadLibraryA(LPCSTR name)
{
    void *m = dlopen(name, RTLD_NOW);
    if (!m)
        g_last_error = ERROR_MOD_NOT_FOUND;
    return (HMODULE)m;
}

FARPROC GetProcAddress(HMODULE m, LPCSTR name)
{
    void *f = m ? dlsym((void *)m, name) : NULL;
    if (!f)
        g_last_error = ERROR_PROC_NOT_FOUND;
    return (FARPROC)f;
}

HMODULE GetModuleHandleA(LPCSTR name)
{
    return (HMODULE)(name ? dlopen(name, RTLD_NOW | RTLD_NOLOAD) : dlopen(NULL, RTLD_NOW));
}

DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n)
{
    const char *v = getenv(name);
    if (!v)
        return 0;
    if (buf && n)
        snprintf(buf, n, "%s", v);
    return (DWORD)strlen(v);
}

void ExitProcess(UINT code)
{
    log_printf("compat: the program ends (%u)", code);
    exit((int)code);
}

int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)w; (void)type;
    log_printf("message box \"%s\": %s", caption ? caption : "", text ? text : "");
    return IDOK;
}

static HWND g_window;
static int g_window_w = 1920, g_window_h = 1080;

void compat_set_window(HWND w, int width, int height)
{
    g_window = w;
    g_window_w = width;
    g_window_h = height;
}

HWND GetForegroundWindow(void)
{
    return g_window;   /* (the game is the only thing on the screen) */
}

BOOL GetClientRect(HWND w, LPRECT r)
{
    (void)w;
    r->left = r->top = 0;
    r->right = g_window_w;
    r->bottom = g_window_h;
    return TRUE;
}

int GetSystemMetrics(int index)
{
    return index == SM_CXSCREEN ? g_window_w : index == SM_CYSCREEN ? g_window_h : 0;
}
#else
typedef int compat_posix_unused;   /* Windows: the real thing */
#endif
