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
#include "gamefiles.h"
#include "gfx.h"
#include "input.h"
#include "menu.h"
#include "plat.h"
#include "sampler.h"
#endif

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
    _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s\\MEGAMAN_NATIVE.EXE", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES)
        _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s\\MEGAMAN.EXE", dir);
    if (!load_image(exe)) {
        log_printf("runner: no game exe in %s", dir);
        MessageBoxA(NULL, "MEGAMAN_NATIVE.EXE or MEGAMAN.EXE wasn't found next to mml_run.exe.", "mml_run",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    mml_init();
    recomp_run();
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)inst; (void)prev; (void)cmd; (void)show;
    return runner_main();
}
#else
/* SDL's startup calls this. The game folder: on Android the app's own folder on the device's
   storage (Android/data/<app>/files), where the player puts the game's files; elsewhere the
   current folder. */
static int g_exit_code = 1;

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
    (void)argc; (void)argv;
    if (!costack_thread(48u << 20, 8u << 20, game_thread, NULL))
        return 1;
    return g_exit_code;
}
#endif
#else
typedef int runner_unused;   /* the DLL build: nothing here (an empty file would warn) */
#endif
