/* gamefiles.h's platform part on Linux, as android_files.c does it on Android: the game folder is the
 * app's own (in the user's data folder) unless one is given; the player picks their game folder with
 * the desktop's folder picker (SDL's: the XDG portal, else zenity) and what the game uses is copied
 * in on a thread of its own, by the same rules as MmlActivity.copyTree; the bundled files (our
 * mml.ini, the intro) come from the assets folder next to the program. */
#if defined(__linux__) && !defined(__ANDROID__)
#include <windows.h>   /* (native/compat) */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <SDL3/SDL.h>

#include "gamefiles.h"
#include "log.h"
#include "plat_sdl.h"
#include "subtitles_get.h"
#include "voice_build.h"

/* mkdir -p of a real path (its last component too) */
static void mkdirs(const char *path)
{
    char p[MAX_PATH], *s;
    snprintf(p, sizeof p, "%s", path);
    for (s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

const char *gamefiles_home(int argc, char **argv)
{
    static char dir[MAX_PATH];
    const char *v = getenv("MML_GAME_DIR"), *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    int i;
    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--game-dir") == 0)
            v = argv[i + 1];
    if (v && *v)
        snprintf(dir, sizeof dir, "%s", v);
    else if (data && *data)
        snprintf(dir, sizeof dir, "%s/mml-modernization", data);
    else
        snprintf(dir, sizeof dir, "%s/.local/share/mml-modernization", home ? home : ".");
    mkdirs(dir);
    return dir;
}

/* copy from to the real path to (through to.part); 0 if it failed. done counts the bytes. */
static int copy_file(const char *from, const char *to, volatile int64_t *done)
{
    static char buf[1 << 20];
    char part[MAX_PATH + 8];
    FILE *in = fopen(from, "rb"), *out;
    size_t n;
    int ok = 1;
    if (!in)
        return 0;
    snprintf(part, sizeof part, "%s.part", to);
    out = fopen(part, "wb");
    if (!out) {
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
    if (!ok || rename(part, to) != 0) {
        unlink(part);
        return 0;
    }
    return 1;
}

int gamefiles_bundled_file(const char *name, const char *dir)
{
    char from[MAX_PATH], real[MAX_PATH], to[MAX_PATH], *p;
    const char *base = SDL_GetBasePath(), *leaf = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    struct stat st;
    snprintf(from, sizeof from, "%sassets/%s", base ? base : "./", leaf);
    if (stat(from, &st) != 0)
        return 0;
    snprintf(to, sizeof to, "%s/%s", compat_path(dir, real, sizeof real), name);
    for (p = to; *p; p++)
        if (*p == '\\')
            *p = '/';
    p = strrchr(to, '/');
    *p = 0;
    mkdirs(to);
    *p = '/';
    return copy_file(from, to, NULL);
}

int gamefiles_bundled_intro(const char *dir)
{
    char path[MAX_PATH];
    int ok;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Modernization\\intro.mpg", dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return 1;
    ok = gamefiles_bundled_file("Modernization\\intro.mpg", dir);
    log_printf("gamefiles: the bundled intro %s", ok ? "put in place" : "isn't in this build");
    return ok;
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
static entry *g_files;
static int g_nfiles, g_cap;

static void add(const char *src, const char *rel, int64_t size)
{
    if (g_nfiles == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_files = (entry *)realloc(g_files, (size_t)g_cap * sizeof *g_files);
    }
    g_files[g_nfiles].src = strdup(src);
    g_files[g_nfiles].rel = strdup(rel);
    g_files[g_nfiles].size = size;
    g_nfiles++;
}

static void walk(const char *src, const char *rel)
{
    DIR *d = opendir(src);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char s[MAX_PATH], r[MAX_PATH];
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
        if (stat(s, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            walk(s, r);
        else if (S_ISREG(st.st_mode))
            add(s, r, (int64_t)st.st_size);
    }
    closedir(d);
}

static int ends_with(const char *s, const char *end)
{
    size_t a = strlen(s), b = strlen(end);
    return a >= b && strcasecmp(s + a - b, end) == 0;
}

static int fail(const char *msg)
{
    snprintf(g_msg, sizeof g_msg, "%s", msg);
    g_state = IMPORT_FAILED;
    log_printf("gamefiles: import: %s", msg);
    return 0;
}

static int SDLCALL copy_thread(void *unused)
{
    static const char *const DIRS[] = { "DAT", "BGM", "SPH", "MODERNIZATION" };
    const char *exe_name = g_jp ? "ROCKMAN.EXE" : "MEGAMAN.EXE", *cfg_name = g_jp ? "ROCKMAN.CFG" : "MEGAMAN.CFG";
    int dat = 0, us = 0, jp = 0, exe, i;
    int64_t total = 0;
    struct statvfs v;
    DIR *d;
    struct dirent *e;
    (void)unused;
    for (i = 0; i < g_nfiles; i++) {
        free(g_files[i].src);
        free(g_files[i].rel);
    }
    g_nfiles = 0;
    snprintf(g_msg, sizeof g_msg, "%s", "Looking through the folder");
    d = opendir(g_src);
    if (!d)
        return fail("That folder can't be read");
    while ((e = readdir(d)) != NULL) {
        char s[MAX_PATH];
        struct stat st;
        size_t k;
        snprintf(s, sizeof s, "%s/%s", g_src, e->d_name);
        if (e->d_name[0] == '.' || stat(s, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            for (k = 0; k < sizeof DIRS / sizeof DIRS[0]; k++)
                if (strcasecmp(e->d_name, DIRS[k]) == 0 && !(g_jp && k == 3)) {   /* (mods: shared) */
                    walk(s, e->d_name);
                    dat |= k == 0;
                }
        } else if (S_ISREG(st.st_mode)) {
            us |= strcasecmp(e->d_name, "MEGAMAN.EXE") == 0;
            jp |= strcasecmp(e->d_name, "ROCKMAN.EXE") == 0;
            if (strcasecmp(e->d_name, exe_name) == 0 || strcasecmp(e->d_name, cfg_name) == 0 ||
                strcasecmp(e->d_name, "CAPCOM.AVI") == 0 || ends_with(e->d_name, ".MCD") ||
                ends_with(e->d_name, ".MCD.QUICKSAVE"))   /* (CAPCOM.AVI: the game checks it's there) */
                add(s, e->d_name, (int64_t)st.st_size);
        }
    }
    closedir(d);
    exe = g_jp ? jp : us;
    if (!exe || !dat) {
        if (!exe && (g_jp ? us : jp))
            return fail(g_jp ? "That's the US version (MEGAMAN.EXE): import it as the US version"
                             : "That's the Japanese version (ROCKMAN.EXE): import it as the Japanese version");
        snprintf(g_msg, sizeof g_msg, "That folder has no %s: pick the game's own folder", !exe ? exe_name : "DAT folder");
        return fail(g_msg);
    }
    mkdirs(g_dest);
    for (i = 0; i < g_nfiles; i++)
        total += g_files[i].size;
    if (statvfs(g_dest, &v) == 0 && (double)v.f_bavail * v.f_frsize < (double)total + (64 << 20)) {
        snprintf(g_msg, sizeof g_msg, "Not enough free space: %.1f GB needed, %.1f GB free", total / 1e9,
                 (double)v.f_bavail * v.f_frsize / 1e9);
        return fail(g_msg);
    }
    g_total = total;
    g_state = IMPORT_COPYING;
    log_printf("gamefiles: import: %d files, %.1f MB from %s", g_nfiles, total / 1e6, g_src);
    for (i = 0; i < g_nfiles; i++) {
        char to[MAX_PATH], *slash;
        struct stat st;
        snprintf(to, sizeof to, "%s/%s", g_dest, g_files[i].rel);
        if (stat(to, &st) == 0 && S_ISREG(st.st_mode) && (int64_t)st.st_size == g_files[i].size) {   /* (there from before) */
            g_done += g_files[i].size;
            continue;
        }
        slash = strrchr(to, '/');
        *slash = 0;
        mkdirs(to);
        *slash = '/';
        snprintf(g_msg, sizeof g_msg, "Copying %s", g_files[i].rel);
        if (!copy_file(g_files[i].src, to, &g_done)) {
            snprintf(g_msg, sizeof g_msg, "The copy stopped: couldn't write %s (%s)", g_files[i].rel, strerror(errno));
            return fail(g_msg);
        }
    }
    snprintf(g_msg, sizeof g_msg, "%s", "Copied");
    g_state = IMPORT_DONE;
    log_printf("gamefiles: import: done");
    return 0;
}

/* SDL's folder picker answers here (on whichever thread it likes) */
static void SDLCALL picked(void *unused, const char *const *files, int filter)
{
    SDL_Thread *t;
    (void)unused;
    (void)filter;
    if (!files) {
        log_printf("gamefiles: no folder picker: %s", SDL_GetError());
        fail("This desktop has no folder picker (install zenity or xdg-desktop-portal), or start the game "
             "with --game-dir and your game folder");
        return;
    }
    if (!files[0]) {
        snprintf(g_msg, sizeof g_msg, "%s", "No folder picked");
        g_state = IMPORT_CANCELLED;
        return;
    }
    snprintf(g_src, sizeof g_src, "%s", files[0]);
    t = SDL_CreateThread(copy_thread, "mml-import", NULL);
    if (t)
        SDL_DetachThread(t);
    else
        fail("Couldn't start the copy");
}

int gamefiles_can_import(void)
{
    return 1;
}

void gamefiles_import_begin(const char *dir, int jp)
{
    char home[MAX_PATH];
    snprintf(home, sizeof home, "%s/", getenv("HOME") ? getenv("HOME") : "");   /* (the "/": open inside it) */
    compat_path(dir, g_dest, sizeof g_dest);
    g_jp = jp;
    g_done = g_total = 0;
    snprintf(g_msg, sizeof g_msg, "%s", jp ? "Pick your Rockman DASH folder (with ROCKMAN.EXE)"
                                           : "Pick your Mega Man Legends folder (with MEGAMAN.EXE)");
    g_state = IMPORT_PICKING;
    log_printf("gamefiles: the folder picker (the %s version, into %s)", jp ? "Japanese" : "US", g_dest);
    SDL_ShowOpenFolderDialog(picked, NULL, plat_sdl_window(), home, false);
}

int gamefiles_import_poll(char *status, size_t n, float *progress)
{
    int st = g_state;
    *progress = g_total > 0 ? (float)((double)g_done / (double)g_total) : 0;
    if (n)
        snprintf(status, n, "%s", g_msg);
    return st;
}

/* ---- the US version's extras: the Japanese voices from voice_build.c on a thread of its own ---- */
static int g_task;   /* 0, 1 subtitles, 2 voices */
static volatile int g_vstate;
static volatile float g_vprogress;
static char g_vmsg[200], g_us[MAX_PATH], g_jpdir[MAX_PATH];

static char g_sub_dir[MAX_PATH];

/* the subtitles' file from the internet (subtitles_get.h): curl, else wget, as every desktop has one */
int subtitles_fetch(const char *url, const char *out)
{
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "curl -fsSL --connect-timeout 30 --max-time 120 -A mml-modernization-setup -o '%s' '%s' 2>/dev/null || "
             "wget -q -T 30 -U mml-modernization-setup -O '%s' '%s' 2>/dev/null", out, url, out, url);
    return system(cmd) == 0;
}


static int SDLCALL subtitles_thread(void *unused)
{
    char real[MAX_PATH], msg[300];
    int ok;
    (void)unused;
    compat_path(g_sub_dir, real, sizeof real);
    mkdirs(real);
    ok = subtitles_download(real, msg, sizeof msg);
    snprintf(g_vmsg, sizeof g_vmsg, "%s", msg);
    g_vstate = ok ? TASK_DONE : TASK_FAILED;
    return 0;
}

void gamefiles_subtitles_begin(const char *dir)
{
    SDL_Thread *t;
    _snprintf_s(g_sub_dir, sizeof g_sub_dir, _TRUNCATE, "%s\\Modernization", dir);
    snprintf(g_vmsg, sizeof g_vmsg, "%s", "Downloading the subtitles from the Rockman DASH PSP English project");
    g_vprogress = 0;
    g_vstate = TASK_RUNNING;
    g_task = 1;
    log_printf("gamefiles: downloading the subtitles into %s", g_sub_dir);
    t = SDL_CreateThread(subtitles_thread, "mml-subtitles", NULL);
    if (t)
        SDL_DetachThread(t);
    else {
        snprintf(g_vmsg, sizeof g_vmsg, "%s", "Couldn't start the download");
        g_vstate = TASK_FAILED;
    }
}

static int SDLCALL voices_thread(void *unused)
{
    char msg[200];
    int ok;
    (void)unused;
    ok = voice_build_jp(g_us, g_jpdir, msg, sizeof msg, &g_vprogress);
    snprintf(g_vmsg, sizeof g_vmsg, "%s", ok ? "Japanese voices built" : msg);
    g_vstate = ok ? TASK_DONE : TASK_FAILED;
    return 0;
}

void gamefiles_voices_begin(const char *us_dir, const char *jp_dir)
{
    SDL_Thread *t;
    snprintf(g_us, sizeof g_us, "%s", us_dir);
    snprintf(g_jpdir, sizeof g_jpdir, "%s", jp_dir);
    snprintf(g_vmsg, sizeof g_vmsg, "%s", "Building the Japanese voices from your Japanese version");
    g_vprogress = 0;
    g_vstate = TASK_RUNNING;
    g_task = 2;
    t = SDL_CreateThread(voices_thread, "mml-voices", NULL);
    if (t)
        SDL_DetachThread(t);
    else {
        snprintf(g_vmsg, sizeof g_vmsg, "%s", "Couldn't start the build");
        g_vstate = TASK_FAILED;
    }
}

int gamefiles_task_poll(char *status, size_t n, float *progress)
{
    *progress = g_task == 2 ? g_vprogress : 0;
    if (n)
        snprintf(status, n, "%s", g_task ? g_vmsg : "");
    return g_task ? g_vstate : TASK_IDLE;
}
#else
typedef int linux_files_unused;   /* Android: android_files.c; Windows: gamefiles.c's own */
#endif
