/* mml_run.exe: the game without its exe running. The runner reserves the game's memory
 * (guest_mem.c), copies the game's exe file into it at the exe's own addresses, installs every
 * mod as the DLL would (mml_init: hooks become registrations the recompiled code calls, imports
 * are our shims), and starts the recompiled game at its entry point (recomp_run). Nothing of the
 * game runs as machine code and nothing reaches it at a host address: the step before the
 * portable runtime (M3 item 4). Built by build_runner.bat with MML_RUNNER defined; it goes in
 * the game folder and loads MEGAMAN_NATIVE.EXE (else MEGAMAN.EXE) from there. */
#ifdef MML_RUNNER
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "controls.h"
#include "costack.h"
#include "sampler.h"
#else
#include "controls.h"
#include "kbm.h"
#endif
#include "gamefiles.h"
#include "gfx.h"
#include "input.h"
#include "menu.h"
#include "plat.h"

#include "guest.h"
#include "log.h"
#include "mml.h"
#include "recomp.h"

/* the exe file into guest memory at its image base: headers, then each section */
static int load_image(const char *path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    uint8_t *file;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS32 *nt;
    const IMAGE_SECTION_HEADER *sec;
    uint32_t base;
    int k;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    size = GetFileSize(f, NULL);
    file = (uint8_t *)malloc(size);
    if (!file || !ReadFile(f, file, size, &got, NULL) || got != size) {
        CloseHandle(f);
        free(file);
        return 0;
    }
    CloseHandle(f);
    dos = (const IMAGE_DOS_HEADER *)file;
    nt = (const IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        log_printf("runner: %s isn't a 32-bit x86 exe", path);
        free(file);
        return 0;
    }
    base = nt->OptionalHeader.ImageBase;
    if (base != 0x400000 || guest_pages(base, nt->OptionalHeader.SizeOfImage, 1) != base) {
        log_printf("runner: can't place the image at %08X", base);
        free(file);
        return 0;
    }
    memcpy(GUEST(base), file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        DWORD n = sec->SizeOfRawData;
        /* no file data (.bss: raw pointer 0, though it states a raw size): stays zero, as
           Windows maps it; copying "file offset 0" filled the game's zeroed globals with the
           exe's own header and code */
        if (!sec->PointerToRawData || (sec->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA))
            continue;
        if (sec->PointerToRawData + n > size)
            n = size > sec->PointerToRawData ? size - sec->PointerToRawData : 0;
        if (n)
            memcpy(GUEST(base + sec->VirtualAddress), file + sec->PointerToRawData, n);
    }
    log_printf("runner: %s loaded at %08X (%u KB, %d sections)", path, base, nt->OptionalHeader.SizeOfImage >> 10,
               nt->FileHeader.NumberOfSections);
    free(file);
    return 1;
}

#ifndef _WIN32
/* The start menu, where there is no launcher ([general] start_menu, on by default): the window
 * and the renderer are made here, before the game; the game takes the same ones when it asks
 * for its window and device, so Play goes straight in. 0 = the app closed. */
static int start_menu(const char *dir)
{
    char ini[MAX_PATH];
    gfx_options o;
    uint32_t win;
    _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s\\mml.ini", dir);
    gamefiles_defaults(dir);   /* (a fresh install: our mml.ini; imported files: the intro...) */
    if (!GetPrivateProfileIntA("general", "start_menu", 1, ini))
        return 1;
    if (GetPrivateProfileIntA("debug", "remote_pad", 0, ini))
        controls_debug_pad();
    input_sdl_start_early();
    win = plat_window_create("Megaman", "Mega Man Legends", 0, 0, 0, 0, 854, 480, NULL, NULL);
    mml_gfx_options(ini, dir, &o);
    if (!win || !gfx_init((void *)(uintptr_t)win, 854, 480, &o)) {
        log_printf("runner: no start menu (no window or renderer)");
        return 1;
    }
    return menu_start(ini, dir);
}
#endif

static int runner_main(void)
{
    char dir[MAX_PATH], exe[MAX_PATH], *slash;
    GetModuleFileNameA(NULL, dir, sizeof dir);
    slash = strrchr(dir, '\\');
    if (slash)
        *slash = 0;
    SetCurrentDirectoryA(dir);
    log_open(dir);
    log_printf("mml_run %s in %s", MML_NATIVE_BUILD, dir);
    if (!guest_init())
        return 1;
#ifndef _WIN32
    if (!start_menu(dir))
        return 0;
#endif
#ifndef _WIN32
    if (gamefiles_edition(dir) == 1) {   /* the imported Japanese version, in its own folder (gamefiles.h) */
        _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s\\%s\\ROCKMAN_NATIVE.EXE", dir, GAMEFILES_JP_DIR);
        compat_set_edition_dir(GAMEFILES_JP_DIR);
        log_printf("runner: the Japanese version (the %s folder)", GAMEFILES_JP_DIR);
    } else
#endif
    {   /* the imported copy's (gamefiles.c), else the installed one: the US, then the Japanese. On
           Windows the chosen version's folder (the Japanese one may be a folder of its own) becomes the
           current one: its files are opened from there, and mml.c finds the shared settings above it */
        static const char *const names[] = { "MEGAMAN_NATIVE.EXE", "MEGAMAN.EXE", "ROCKMAN_NATIVE.EXE",
                                             "ROCKMAN.EXE" };
        char game[MAX_PATH];
        size_t i, first = 0;
#ifdef _WIN32
        int jp = gamefiles_edition(dir) == 1;
        gamefiles_dir(dir, jp, game, sizeof game);
        if (jp) {
            first = 2;   /* (its program first) */
            SetCurrentDirectoryA(game);
            log_printf("runner: the Japanese version (%s)", game);
        }
#else
        strcpy_s(game, sizeof game, dir);
#endif
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s\\%s", game, names[(first + i) % 4]);
            if (GetFileAttributesA(exe) != INVALID_FILE_ATTRIBUTES)
                break;
        }
    }
    if (!load_image(exe)) {
        log_printf("runner: no game exe in %s", dir);
        MessageBoxA(NULL, "MEGAMAN.EXE (or ROCKMAN.EXE) wasn't found next to mml_run.exe.", "mml_run",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    mml_init();
    recomp_run();
    return 0;
}

#ifdef _WIN32
/* ---- the start menu on Windows (mml_run64.exe --menu), in place of the Python launcher ----
   The same start menu as on Android and Linux, in a window of its own. Play starts the chosen game
   engine as a program of its own, so the renderer, display mode and engine just picked apply from
   its first frame: the Classic engine is the game's own exe (with mml.dll), the Standalone one
   this program without --menu. The menu's log is Modernization\menu.log (the game rewrites
   mml.log when it starts). */

/* the native widescreen width: 480 lines at the monitor's aspect, even (mml_gui.py native_width) */
static int native_width(void)
{
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    return sw > 0 && sh > 0 ? ((int)(480.0 * sw / sh + 0.5) + 1) & ~1 : 854;
}

/* DDrawCompat.ini for the Compatibility renderer, from DDrawCompat.ini.16to9 with the settings
   mml.ini holds (mml_gui.py write_ini) */
static void write_ddrawcompat_ini(const char *dir, const char *ini)
{
    char base[MAX_PATH], out[MAX_PATH], line[512], aspect[16], v[6][96];
    static const char *const KEYS[6] = { "ResolutionScale", "DisplayAspectRatio", "ResolutionScaleFilter",
                                         "TextureFilter", "Antialiasing", "SupportedResolutions" };
    int seen[6] = { 0 }, scale = GetPrivateProfileIntA("video", "render_scale", 0, ini), msaa, k;
    FILE *in, *o;
    GetPrivateProfileStringA("video", "aspect", "16:9", aspect, sizeof aspect, ini);
    msaa = GetPrivateProfileIntA("video", "msaa", 1, ini);
    if (scale > 0)
        _snprintf_s(v[0], sizeof v[0], _TRUNCATE, "app(%d)", scale);
    else
        strcpy_s(v[0], sizeof v[0], "display");
    strcpy_s(v[1], sizeof v[1], strcmp(aspect, "4:3") == 0 ? "4:3" : "display");
    strcpy_s(v[2], sizeof v[2], GetPrivateProfileIntA("video", "nearest_upscale", 0, ini) ? "point" : "bilinear");
    strcpy_s(v[3], sizeof v[3], GetPrivateProfileIntA("video", "anisotropy", 16, ini) > 1 ? "af16x" : "app");
    strcpy_s(v[4], sizeof v[4], msaa == 8 ? "msaa8x" : msaa == 4 ? "msaa4x" : "off");
    _snprintf_s(v[5], sizeof v[5], _TRUNCATE, "native, 640x480, 800x600, 1024x768, %dx480", native_width());
    _snprintf_s(base, sizeof base, _TRUNCATE, "%s\\DDrawCompat.ini.16to9", dir);
    _snprintf_s(out, sizeof out, _TRUNCATE, "%s\\DDrawCompat.ini", dir);
    if (fopen_s(&in, base, "r") != 0)
        in = NULL;
    if (fopen_s(&o, out, "w") != 0) {
        if (in)
            fclose(in);
        log_printf("menu: can't write %s", out);
        return;
    }
    while (in && fgets(line, sizeof line, in)) {
        char key[64], *eq = strchr(line, '=');
        int done = 0;
        if (eq) {
            size_t n = (size_t)(eq - line);
            while (n && (line[n - 1] == ' ' || line[n - 1] == '\t'))
                n--;
            if (n < sizeof key) {
                memcpy(key, line, n);
                key[n] = 0;
                for (k = 0; k < 6 && !done; k++)
                    if (_stricmp(key, KEYS[k]) == 0) {
                        fprintf(o, "%-23s = %s\n", KEYS[k], v[k]);
                        seen[k] = done = 1;
                    }
            }
        }
        if (!done)
            fputs(line, o);
    }
    for (k = 0; k < 6; k++)
        if (!seen[k])
            fprintf(o, "%-23s = %s\n", KEYS[k], v[k]);
    if (in)
        fclose(in);
    fclose(o);
}

/* the game's own window: a visible top-level window of its process */
typedef struct {
    DWORD pid;
    HWND found;
} find_window;

static BOOL CALLBACK find_game_window(HWND w, LPARAM p)
{
    find_window *f = (find_window *)p;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == f->pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        f->found = w;
        return FALSE;
    }
    return TRUE;
}

static void menu_window_event(uint32_t win, uint32_t msg, uint32_t wparam, uint32_t lparam, int sent)
{
    (void)win; (void)wparam; (void)lparam; (void)sent;
    if (msg == WM_DESTROY)   /* (Alt+F4, or the window closed) */
        plat_quit();
}

/* Play: the chosen engine as a program of its own; 0 if it started */
static int launch_game(const char *dir, const char *ini, uint32_t win)
{
    char engine[32], renderer[32], exe[MAX_PATH], cmd[MAX_PATH + 4], vdir[MAX_PATH];
    const char *cwd = dir;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int jp = gamefiles_edition(dir) == 1;
    GetPrivateProfileStringA("general", "engine", "classic", engine, sizeof engine, ini);
    GetPrivateProfileStringA("video", "renderer", "mml", renderer, sizeof renderer, ini);
    gamefiles_dir(dir, jp, vdir, sizeof vdir);   /* the version's folder (the Japanese one: its own) */
    if (_stricmp(renderer, "ddrawcompat") == 0)
        strcpy_s(engine, sizeof engine, "classic");   /* (Compatibility is the game's own exe with DDrawCompat) */
    if (_stricmp(engine, "standalone") == 0) {
        GetModuleFileNameA(NULL, exe, sizeof exe);   /* (it picks the version itself, runner_main) */
    } else {
        /* the version's patched program: the start menu's import writes *_NATIVE.EXE beside the
           original; a folder Setup patched in place has only the original name, patched */
        const char *base = jp ? "ROCKMAN" : "MEGAMAN";
        _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s\\%s_NATIVE.EXE", vdir, base);
        if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES)
            _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s\\%s.EXE", vdir, base);
        gamefiles_sync_runtime(dir, vdir);   /* (mml.dll and the wrappers beside it) */
        if (_stricmp(renderer, "ddrawcompat") == 0)
            write_ddrawcompat_ini(vdir, ini);
        cwd = vdir;
    }
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE, "\"%s\"", exe);
    log_printf("menu: play, the %s engine: %s", engine, exe);
    /* Windows lets only the program in front put another in front, and the game waits, paused,
       until its window is active: the menu stays in front until the game's window is there, hands
       the foreground to it, and only then goes */
    AllowSetForegroundWindow(ASFW_ANY);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) {
        log_printf("menu: the game didn't start (error %lu)", GetLastError());
        plat_message_box("The game couldn't be started. See Modernization\\menu.log.", "Mega Man Legends");
        return 1;
    }
    {
        find_window f;
        int k;
        f.pid = pi.dwProcessId;
        f.found = NULL;
        for (k = 0; k < 400 && !f.found; k++) {   /* (20 s at most) */
            plat_pump();
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                log_printf("menu: the game closed before its window appeared");
                break;
            }
            EnumWindows(find_game_window, (LPARAM)&f);
        }
        if (f.found) {
            SetForegroundWindow(f.found);
            log_printf("menu: the game's window is up (after %d ms), in front: %s", k * 50,
                       GetForegroundWindow() == f.found ? "yes" : "no");
        } else if (k >= 400)
            log_printf("menu: no game window after 20 s");
    }
    plat_window_show(win, SW_HIDE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

static int start_menu_windows(void)
{
    char dir[MAX_PATH], ini[MAX_PATH], log[MAX_PATH], *slash;
    gfx_options o;
    uint32_t win;
    int w, h;
    GetModuleFileNameA(NULL, dir, sizeof dir);
    slash = strrchr(dir, '\\');
    if (slash)
        *slash = 0;
    SetCurrentDirectoryA(dir);
    _snprintf_s(log, sizeof log, _TRUNCATE, "%s\\Modernization", dir);
    CreateDirectoryA(log, NULL);
    _snprintf_s(log, sizeof log, _TRUNCATE, "%s\\Modernization\\menu.log", dir);
    log_open_file(log);
    log_printf("start menu %s in %s", MML_NATIVE_BUILD, dir);
    _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s\\mml.ini", dir);
    if (GetPrivateProfileIntA("debug", "remote_pad", 0, ini))   /* (test tools drive the menu too) */
        controls_debug_pad();
    input_sdl_start_early();
    plat_desktop_size(&w, &h);
    win = plat_window_create("MMLStartMenu", "Mega Man Legends", WS_POPUP, 0, 0, 0, w, h, "ROCKMAN_ICON",
                             menu_window_event);
    mml_gfx_options(ini, dir, &o);
    o.exclusive = 0;   /* (the menu is a window; the game takes the display if it's set to) */
    o.shader[0] = 0;
    if (!win || !gfx_init((void *)(intptr_t)(int32_t)win, 854, 480, &o)) {
        log_printf("menu: no window or renderer");
        plat_message_box("The start menu couldn't open its window. See Modernization\\menu.log.", "Mega Man Legends");
        return 1;
    }
    plat_window_show(win, SW_SHOW);
    SetForegroundWindow((HWND)(intptr_t)(int32_t)win);
    kbm_attach((HWND)(intptr_t)(int32_t)win);   /* (the menu's keys count while it is in front) */
    if (!menu_start(ini, dir))
        return 0;
    return launch_game(dir, ini, win);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)inst; (void)prev; (void)show;
    if (cmd && strstr(cmd, "--menu"))
        return start_menu_windows();
    return runner_main();
}
#else
/* SDL's startup calls this. The game folder: on Android the app's own folder on the device's
   storage (Android/data/<app>/files), where the player puts the game's files; on Linux the app's
   own in the user's data folder, or the one given (gamefiles_home); elsewhere the current folder. */
static int g_exit_code = 1;
static int g_argc;
static char **g_argv;

static void game_thread(void *arg)
{
    const char *dir = NULL;
    (void)arg;
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");   /* Back is ours (menu.c): not a sudden exit */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {   /* (this thread is SDL's main one) */
        SDL_Log("mml: SDL_Init failed: %s", SDL_GetError());
        return;
    }
#ifdef __ANDROID__
    dir = SDL_GetAndroidExternalStoragePath();
#elif defined(__linux__)
    dir = gamefiles_home(g_argc, g_argv);
#endif
    compat_set_game_dir(dir ? dir : ".");
    {   /* [debug] sampler=1: from the start (mml.c starts it only at the first frame) */
        char ini[1024];
        snprintf(ini, sizeof ini, "%s\\mml.ini", dir ? dir : ".");
        if (GetPrivateProfileIntA("debug", "sampler", 0, ini))
            sampler_start();
    }
    g_exit_code = runner_main();
}

/* The game runs on a thread of ours whose stack holds the game's own stacks too (costack.h:
   the 32 MB one recomp_run starts on, the thread slots' 4 MB ones): 48 MB for those, 8 MB for
   the runner's own frames. */
int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
    if (!costack_thread(48u << 20, 8u << 20, game_thread, NULL))
        return 1;
    return g_exit_code;
}
#endif
#else
typedef int runner_unused;   /* the DLL build: nothing here (an empty file would warn) */
#endif
