/* gamefiles.h's platform part for the Windows start menu (mml_run64.exe --menu), as android_files.c
 * and linux_files.c do it: the player picks their game folder with Windows' folder picker and what
 * the game uses is copied into the app's folder (the US version) or a folder of its own in it (the
 * Japanese one), by the same rules as on Android and Linux; the picked folder isn't changed. The
 * project's CAPCOM.AVI (Setup's) is kept. The Classic engine runs a version's program with mml.dll
 * and the wrapper DLLs beside it: the Japanese folder gets copies of the app folder's
 * (gamefiles_sync_runtime). Also the subtitles download (subtitles_get.c, with WinHTTP) and the
 * Japanese voices build (voice_build.c), on threads of their own. */
#if defined(_WIN32) && defined(MML_RUNNER)
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamefiles.h"
#include "log.h"
#include "subtitles_get.h"
#include "voice_build.h"
#include "win_http.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

static int exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* mkdir -p (its last component too) */
static void mkdirs(const char *path)
{
    char p[MAX_PATH], *s;
    strcpy_s(p, sizeof p, path);
    for (s = p + 3; *s; s++)   /* (past "C:\") */
        if (*s == '\\' || *s == '/') {
            char c = *s;
            *s = 0;
            CreateDirectoryA(p, NULL);
            *s = c;
        }
    CreateDirectoryA(p, NULL);
}

/* from to the file to (through to.part); 0 if it failed. done counts the bytes. */
static int copy_file(const char *from, const char *to, volatile int64_t *done)
{
    static char buf[1 << 20];
    char part[MAX_PATH + 8];
    FILE *in, *out;
    size_t n;
    int ok = 1;
    if (fopen_s(&in, from, "rb") != 0)
        return 0;
    _snprintf_s(part, sizeof part, _TRUNCATE, "%s.part", to);
    if (fopen_s(&out, part, "wb") != 0) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
        if (done)
            *done += (int64_t)n;
    }
    ok &= !ferror(in);
    fclose(in);
    ok &= fclose(out) == 0;
    if (!ok || !MoveFileExA(part, to, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(part);
        return 0;
    }
    return 1;
}

int gamefiles_bundled_file(const char *name, const char *dir)
{
    (void)name;
    (void)dir;
    return 0;   /* (Setup puts mml.ini and the intro in the app's folder) */
}

int gamefiles_bundled_intro(const char *dir)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\Modernization\\intro.mpg", dir);
    return exists(p);
}

/* ---- the runtime beside a version's program (the Classic engine) ---- */
void gamefiles_sync_runtime(const char *root, const char *dir)
{
    static const char *const RUNTIME[] = { "mml.dll", "ddraw.dll", "dinput.dll", "dsound.dll", "dsoal-aldrv.dll",
                                           "SDL3.dll", "librashader.dll", "DDrawCompat.ini.16to9",
                                           "DDrawCompat.ini.4to3" };
    size_t i;
    if (_stricmp(root, dir) == 0)
        return;
    for (i = 0; i < sizeof RUNTIME / sizeof RUNTIME[0]; i++) {
        char from[MAX_PATH], to[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA a, b;
        _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\%s", root, RUNTIME[i]);
        _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\%s", dir, RUNTIME[i]);
        if (!GetFileAttributesExA(from, GetFileExInfoStandard, &a))
            continue;
        if (GetFileAttributesExA(to, GetFileExInfoStandard, &b) && a.nFileSizeLow == b.nFileSizeLow &&
            a.nFileSizeHigh == b.nFileSizeHigh && CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0)
            continue;   /* (the same file) */
        if (CopyFileA(from, to, FALSE))
            log_printf("gamefiles: %s copied beside %s's program", RUNTIME[i], dir);
        else
            log_printf("gamefiles: %s couldn't be copied to %s (error %lu)", RUNTIME[i], dir, GetLastError());
    }
}

/* ---- the importer ---- */
typedef struct {
    char *src, *rel;
    int64_t size;
} entry;

static volatile int g_state = IMPORT_IDLE;
static volatile int64_t g_done, g_total;
static char g_msg[300], g_dest[MAX_PATH], g_src[MAX_PATH];
static int g_jp;
static HWND g_owner;
static entry *g_files;
static int g_nfiles, g_cap;

static void add(const char *src, const char *rel, int64_t size)
{
    if (g_nfiles == g_cap) {
        entry *n;
        g_cap = g_cap ? g_cap * 2 : 256;
        n = (entry *)realloc(g_files, (size_t)g_cap * sizeof *g_files);
        if (!n)
            return;
        g_files = n;
    }
    g_files[g_nfiles].src = _strdup(src);
    g_files[g_nfiles].rel = _strdup(rel);
    g_files[g_nfiles].size = size;
    g_nfiles++;
}

/* in a Modernization folder, what isn't the player's mods: the logs, the old launcher's settings
   and the files Setup set aside (an install Setup patched in place has them) */
static int not_a_mod(const char *name, int dir)
{
    size_t n = strlen(name);
    if (dir)
        return _stricmp(name, "quarantined-wrappers") == 0 || _stricmp(name, "legacy-0.1") == 0;
    return (n > 4 && _stricmp(name + n - 4, ".log") == 0) || _stricmp(name, "settings.json") == 0 ||
           _stricmp(name, "recomp_patches.txt") == 0;
}

static void walk(const char *src, const char *rel, int mods)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA f;
    HANDLE h;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", src);
    h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char s[MAX_PATH], r[MAX_PATH];
        if (f.cFileName[0] == '.')
            continue;
        _snprintf_s(s, sizeof s, _TRUNCATE, "%s\\%s", src, f.cFileName);
        _snprintf_s(r, sizeof r, _TRUNCATE, "%s\\%s", rel, f.cFileName);
        if (mods && not_a_mod(f.cFileName, (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
            continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            walk(s, r, mods);
        else
            add(s, r, (int64_t)f.nFileSizeHigh << 32 | f.nFileSizeLow);
    } while (FindNextFileA(h, &f));
    FindClose(h);
}

static int ends_with(const char *s, const char *end)
{
    size_t a = strlen(s), b = strlen(end);
    return a >= b && _stricmp(s + a - b, end) == 0;
}

static void fail(const char *msg)
{
    strcpy_s(g_msg, sizeof g_msg, msg);
    g_state = IMPORT_FAILED;
    log_printf("gamefiles: import: %s", msg);
}

/* the picked folder's files into g_dest, by the rules of MmlActivity.copyTree (Android) */
static void copy_tree(void)
{
    static const char *const DIRS[] = { "DAT", "BGM", "SPH", "MODERNIZATION" };
    const char *exe_name = g_jp ? "ROCKMAN.EXE" : "MEGAMAN.EXE", *cfg_name = g_jp ? "ROCKMAN.CFG" : "MEGAMAN.CFG";
    char pat[MAX_PATH], full_src[MAX_PATH], full_dest[MAX_PATH];
    int dat = 0, us = 0, jp = 0, exe, i;
    int64_t total = 0;
    ULARGE_INTEGER avail;
    WIN32_FIND_DATAA f;
    HANDLE h;
    for (i = 0; i < g_nfiles; i++) {
        free(g_files[i].src);
        free(g_files[i].rel);
    }
    g_nfiles = 0;
    GetFullPathNameA(g_src, sizeof full_src, full_src, NULL);
    GetFullPathNameA(g_dest, sizeof full_dest, full_dest, NULL);
    if (_stricmp(full_src, full_dest) == 0) {
        fail("That's this app's own folder: pick the folder your game is in");
        return;
    }
    strcpy_s(g_msg, sizeof g_msg, "Looking through the folder");
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", g_src);
    h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) {
        fail("That folder can't be read");
        return;
    }
    do {
        char s[MAX_PATH];
        size_t k;
        if (f.cFileName[0] == '.')
            continue;
        _snprintf_s(s, sizeof s, _TRUNCATE, "%s\\%s", g_src, f.cFileName);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            for (k = 0; k < sizeof DIRS / sizeof DIRS[0]; k++)
                if (_stricmp(f.cFileName, DIRS[k]) == 0 && !(g_jp && k == 3)) {   /* (mods: shared) */
                    walk(s, f.cFileName, k == 3);
                    dat |= k == 0;
                }
        } else {
            us |= _stricmp(f.cFileName, "MEGAMAN.EXE") == 0;
            jp |= _stricmp(f.cFileName, "ROCKMAN.EXE") == 0;
            if (_stricmp(f.cFileName, exe_name) == 0 || _stricmp(f.cFileName, cfg_name) == 0 ||
                _stricmp(f.cFileName, "CAPCOM.AVI") == 0 || ends_with(f.cFileName, ".MCD") ||
                ends_with(f.cFileName, ".MCD.QUICKSAVE"))   /* (CAPCOM.AVI: the game checks it's there) */
                add(s, f.cFileName, (int64_t)f.nFileSizeHigh << 32 | f.nFileSizeLow);
        }
    } while (FindNextFileA(h, &f));
    FindClose(h);
    exe = g_jp ? jp : us;
    if (!exe || !dat) {
        if (!exe && (g_jp ? us : jp))
            fail(g_jp ? "That's the US version (MEGAMAN.EXE): import it as the US version"
                      : "That's the Japanese version (ROCKMAN.EXE): import it as the Japanese version");
        else {
            char m[160];
            _snprintf_s(m, sizeof m, _TRUNCATE, "That folder has no %s: pick the game's own folder",
                        !exe ? exe_name : "DAT folder");
            fail(m);
        }
        return;
    }
    mkdirs(g_dest);
    for (i = 0; i < g_nfiles; i++)
        total += g_files[i].size;
    if (GetDiskFreeSpaceExA(g_dest, &avail, NULL, NULL) && (double)avail.QuadPart < (double)total + (64 << 20)) {
        char m[160];
        _snprintf_s(m, sizeof m, _TRUNCATE, "Not enough free space: %.1f GB needed, %.1f GB free", total / 1e9,
                    (double)avail.QuadPart / 1e9);
        fail(m);
        return;
    }
    g_total = total;
    g_state = IMPORT_COPYING;
    log_printf("gamefiles: import: %d files, %.1f MB from %s into %s", g_nfiles, total / 1e6, g_src, g_dest);
    for (i = 0; i < g_nfiles; i++) {
        char to[MAX_PATH], *slash;
        WIN32_FILE_ATTRIBUTE_DATA a;
        _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\%s", g_dest, g_files[i].rel);
        if (GetFileAttributesExA(to, GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            int64_t have = (int64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
            if (have == g_files[i].size || _stricmp(g_files[i].rel, "CAPCOM.AVI") == 0) {
                g_done += g_files[i].size;   /* (there from before; CAPCOM.AVI: Setup's, our intro) */
                continue;
            }
        }
        slash = strrchr(to, '\\');
        *slash = 0;
        mkdirs(to);
        *slash = '\\';
        _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Copying %s", g_files[i].rel);
        if (!copy_file(g_files[i].src, to, &g_done)) {
            char m[300];
            _snprintf_s(m, sizeof m, _TRUNCATE, "The copy stopped: couldn't write %s (error %lu)", g_files[i].rel,
                        GetLastError());
            fail(m);
            return;
        }
    }
    strcpy_s(g_msg, sizeof g_msg, "Copied");
    g_state = IMPORT_DONE;
    log_printf("gamefiles: import: done");
}

/* Windows' folder picker, then the copy (a thread of its own: the menu keeps drawing) */
static DWORD WINAPI import_thread(LPVOID unused)
{
    IFileOpenDialog *d = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    HRESULT hr;
    (void)unused;
    {   /* (test tools: MML_IMPORT_US / MML_IMPORT_JP name the folder, no picker) */
        char given[MAX_PATH];
        if (GetEnvironmentVariableA(g_jp ? "MML_IMPORT_JP" : "MML_IMPORT_US", given, sizeof given) - 1 <
            sizeof given - 1) {
            strcpy_s(g_src, sizeof g_src, given);
            log_printf("gamefiles: the folder from the environment: %s", g_src);
            copy_tree();
            return 0;
        }
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d);
    if (FAILED(hr)) {
        fail("Windows' folder picker couldn't be opened");
        CoUninitialize();
        return 0;
    }
    IFileOpenDialog_SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(d, g_jp ? L"Pick your Rockman DASH folder (with ROCKMAN.EXE)"
                                     : L"Pick your Mega Man Legends folder (with MEGAMAN.EXE)");
    hr = IFileOpenDialog_Show(d, g_owner);
    if (SUCCEEDED(hr))
        hr = IFileOpenDialog_GetResult(d, &item);
    if (SUCCEEDED(hr))
        hr = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path);
    if (SUCCEEDED(hr) && WideCharToMultiByte(CP_ACP, 0, path, -1, g_src, sizeof g_src, NULL, NULL) > 0) {
        log_printf("gamefiles: picked %s", g_src);
        copy_tree();
    } else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        strcpy_s(g_msg, sizeof g_msg, "No folder picked");
        g_state = IMPORT_CANCELLED;
    } else
        fail("That folder can't be used (it isn't on a drive?)");
    if (path)
        CoTaskMemFree(path);
    if (item)
        IShellItem_Release(item);
    IFileOpenDialog_Release(d);
    CoUninitialize();
    return 0;
}

int gamefiles_can_import(void)
{
    return 1;
}

void gamefiles_import_begin(const char *dir, int jp)
{
    HANDLE t;
    strcpy_s(g_dest, sizeof g_dest, dir);
    g_jp = jp;
    g_done = g_total = 0;
    g_owner = GetForegroundWindow();   /* (the menu's window: the picker opens in front of it) */
    strcpy_s(g_msg, sizeof g_msg, jp ? "Pick your Rockman DASH folder (with ROCKMAN.EXE)"
                                     : "Pick your Mega Man Legends folder (with MEGAMAN.EXE)");
    g_state = IMPORT_PICKING;
    log_printf("gamefiles: the folder picker (the %s version, into %s)", jp ? "Japanese" : "US", g_dest);
    t = CreateThread(NULL, 0, import_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        fail("Couldn't start the import");
}

int gamefiles_import_poll(char *status, size_t n, float *progress)
{
    int st = g_state;
    *progress = g_total > 0 ? (float)((double)g_done / (double)g_total) : 0;
    if (n)
        strcpy_s(status, n, g_msg);
    return st;
}

/* ---- the US version's extras: the subtitles (subtitles_get.c) and the Japanese voices ---- */
static int g_task;   /* 0, 1 subtitles, 2 voices */
static volatile int g_tstate = TASK_IDLE;
static volatile float g_tprogress;
static char g_tmsg[300], g_tdir[MAX_PATH], g_us[MAX_PATH], g_jpdir[MAX_PATH];

int subtitles_fetch(const char *url, const char *out)
{
    return win_http_get(url, out, NULL, NULL, NULL, NULL);
}

static DWORD WINAPI subtitles_thread(LPVOID unused)
{
    (void)unused;
    g_tstate = subtitles_download(g_tdir, g_tmsg, sizeof g_tmsg) ? TASK_DONE : TASK_FAILED;
    return 0;
}

static DWORD WINAPI voices_thread(LPVOID unused)
{
    char msg[200];
    int ok;
    (void)unused;
    ok = voice_build_jp(g_us, g_jpdir, msg, sizeof msg, &g_tprogress);
    strcpy_s(g_tmsg, sizeof g_tmsg, ok ? "Japanese voices built" : msg);
    g_tstate = ok ? TASK_DONE : TASK_FAILED;
    return 0;
}

static void run(LPTHREAD_START_ROUTINE fn)
{
    HANDLE t = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else {
        strcpy_s(g_tmsg, sizeof g_tmsg, "Couldn't start");
        g_tstate = TASK_FAILED;
    }
}

void gamefiles_subtitles_begin(const char *dir)
{
    _snprintf_s(g_tdir, sizeof g_tdir, _TRUNCATE, "%s\\Modernization", dir);
    CreateDirectoryA(g_tdir, NULL);
    strcpy_s(g_tmsg, sizeof g_tmsg, "Downloading the subtitles from the Rockman DASH PSP English project");
    g_tprogress = 0;
    g_tstate = TASK_RUNNING;
    g_task = 1;
    log_printf("gamefiles: downloading the subtitles into %s", g_tdir);
    run(subtitles_thread);
}

void gamefiles_voices_begin(const char *us_dir, const char *jp_dir)
{
    strcpy_s(g_us, sizeof g_us, us_dir);
    strcpy_s(g_jpdir, sizeof g_jpdir, jp_dir);
    strcpy_s(g_tmsg, sizeof g_tmsg, "Building the Japanese voices from your Japanese version");
    g_tprogress = 0;
    g_tstate = TASK_RUNNING;
    g_task = 2;
    run(voices_thread);
}

int gamefiles_task_poll(char *status, size_t n, float *progress)
{
    *progress = g_task == 2 ? g_tprogress : 0;
    if (n)
        strcpy_s(status, n, g_task ? g_tmsg : "");
    return g_task ? g_tstate : TASK_IDLE;
}
#else
typedef int win_files_unused;   /* (the Windows start menu's; mml.dll has gamefiles.c's stubs) */
#endif
