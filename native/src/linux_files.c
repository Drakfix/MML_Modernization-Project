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

/* ---- the English subtitles: Subtitles.java (Android) and tools/voice/build_subtitles.py --download
   (the Windows installer) in C, with the same output byte for byte. The Rockman DASH PSP English
   project's script is downloaded at a fixed commit (curl, else wget: every desktop has one), checked
   against its SHA-256, and converted into Modernization/subtitles_en.txt. ---- */
#define SUB_REPO "https://github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher"
#define SUB_COMMIT "7d49ceb334aae2b2381bdde3d03448cbd90320da"
#define SUB_PATH "script/subtitles_en.csv"
#define SUB_URL "https://raw.githubusercontent.com/JamesAZ0/RockmanDASH-PSP-English-Patcher/" SUB_COMMIT "/" SUB_PATH
#define SUB_SHA256 "0822f5228b3248dd62a358611abe1a7a6182c6a15de9e210896f6b753579697c"
static const char *const SUB_CREDIT[] = {
    "English subtitles for the cutscenes: the work of the Rockman DASH PSP English project",
    "(JamesAZ0 and its contributors), " SUB_REPO ",",
    "from its " SUB_PATH " at commit 7d49ceb, downloaded from there during setup.",
    "That project transcribed the US voice acting and timed the lines to it, under the MIT licence:",
    "Copyright (c) 2026 The Rockman DASH PSP English project contributors.",
    "The lines belong to Capcom.",
};
static const int SUB_NEWS_CLIP[] = { 10, 4, 5, 7, 6 };   /* C scenes 0-4: the news reports (bank 1821E) */
static char g_sub_dir[MAX_PATH];

typedef struct {
    char *p;
    size_t len, cap;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_str(sbuf *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

/* url into the file out with curl, else wget; 0 if neither could */
static int download(const char *url, const char *out)
{
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "curl -fsSL --connect-timeout 30 --max-time 120 -A mml-modernization-setup -o '%s' '%s' 2>/dev/null || "
             "wget -q -T 30 -U mml-modernization-setup -O '%s' '%s' 2>/dev/null", out, url, out, url);
    return system(cmd) == 0;
}

/* one CSV record (the script has no line breaks inside fields): up to max fields into f, each a
   copy (free them); the count */
static int csv_fields(const char *line, char **f, int max)
{
    int n = 0, quoted = 0;
    sbuf cur = { 0 };
    const char *s;
    sb_put(&cur, "", 0);
    for (s = line;; s++) {
        if (!*s || (!quoted && *s == ',')) {
            if (n < max)
                f[n++] = strdup(cur.p);
            cur.len = 0;
            cur.p[0] = 0;
            if (!*s)
                break;
        } else if (quoted) {
            if (*s == '"' && s[1] == '"') {
                sb_put(&cur, "\"", 1);
                s++;
            } else if (*s == '"')
                quoted = 0;
            else
                sb_put(&cur, s, 1);
        } else if (*s == '"')
            quoted = 1;
        else
            sb_put(&cur, s, 1);
    }
    free(cur.p);
    return n;
}

/* the next UTF-8 character of s (advanced past it); invalid bytes count as one character each */
static unsigned utf8_next(const unsigned char **s)
{
    const unsigned char *p = *s;
    unsigned c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, i;
    if (c >= 0x80) {
        unsigned v = c & (0x3F >> more);
        for (i = 0; i < more && (*p & 0xC0) == 0x80; i++)
            v = v << 6 | (*p++ & 0x3F);
        c = i == more && more ? v : 0xFFFD;
    }
    *s = p;
    return c;
}

/* the text as the game's font can show it: typographic marks as ASCII, the rest '?', words one
   space apart (Subtitles.asciiText) */
static void ascii_text(const char *in, sbuf *out)
{
    sbuf a = { 0 };
    const unsigned char *s = (const unsigned char *)in;
    const char *p;
    int word = 0;
    sb_put(&a, "", 0);
    while (*s) {
        unsigned c = utf8_next(&s);
        char ch[2] = { 0, 0 };
        switch (c) {
        case 0x2018: case 0x2019: sb_str(&a, "'"); break;
        case 0x201C: case 0x201D: sb_str(&a, "\""); break;
        case 0x2026: sb_str(&a, "..."); break;
        case 0x2013: sb_str(&a, "-"); break;
        case 0x2014: sb_str(&a, "--"); break;
        case 0x00A0: sb_str(&a, " "); break;
        case 0x00E9: case 0x00E8: sb_str(&a, "e"); break;
        case 0x00E0: sb_str(&a, "a"); break;
        case 0x2122: break;
        default:
            ch[0] = c < 127 ? (char)c : '?';
            if (c >= 0x10000)
                sb_str(&a, "?");   /* (Java counts it as two characters) */
            sb_str(&a, ch);
        }
    }
    for (p = a.p; *p; p++) {   /* split on whitespace, joined by single spaces */
        int space = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f';
        if (space) {
            word = 0;
            continue;
        }
        if (!word && out->len)
            sb_str(out, " ");
        word = 1;
        sb_put(out, (*p >= 32 && *p < 127) ? p : "?", 1);
    }
    free(a.p);
}

static int column(char **head, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(head[i], name) == 0)
            return i;
    return -1;
}

/* the subtitle file from the script (Subtitles.convert); 0 if the script isn't the expected kind */
static int convert(char *csv, sbuf *out)
{
    enum { MAXF = 64 };
    char *head[MAXF], *r[MAXF], *line, *next;
    int nh, sc, st, en, tx, fi, stt, ov, i, ok = 1;
    size_t k;
    for (k = 0; k < sizeof SUB_CREDIT / sizeof SUB_CREDIT[0]; k++) {
        sb_str(out, "# ");
        sb_str(out, SUB_CREDIT[k]);
        sb_str(out, "\n");
    }
    sb_str(out, "# bank clip start end text: built by tools/voice/build_subtitles.py\n");
    next = strchr(csv, '\n');
    if (next)
        *next++ = 0;
    if (strlen(csv) && csv[strlen(csv) - 1] == '\r')
        csv[strlen(csv) - 1] = 0;
    nh = csv_fields(csv, head, MAXF);
    sc = column(head, nh, "scene");
    st = column(head, nh, "start_s");
    en = column(head, nh, "end_s");
    tx = column(head, nh, "text");
    fi = column(head, nh, "en_final");
    stt = column(head, nh, "status");
    ov = column(head, nh, "time_override");
    if (sc < 0 || st < 0 || en < 0 || tx < 0 || fi < 0 || stt < 0 || ov < 0)
        ok = 0;
    for (line = next; ok && line && *line; line = next) {
        char bank[8], text_ok;
        int nr, num, clip;
        double s, e;
        sbuf t = { 0 };
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (*line && line[strlen(line) - 1] == '\r')
            line[strlen(line) - 1] = 0;
        if (!*line)
            continue;
        nr = csv_fields(line, r, MAXF);
        for (i = nr; i < nh; i++)
            r[i] = strdup("");
        if (nr < nh)
            nr = nh;
        {
            char *a = r[stt], *b;
            while (*a == ' ' || *a == '\t')
                a++;
            b = a + strlen(a);
            while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r'))
                b--;
            if ((size_t)(b - a) == 4 && strncmp(a, "drop", 4) == 0)
                goto next_line;
        }
        {
            char *scene = r[sc], *endp;
            while (*scene == ' ')
                scene++;
            num = (int)strtol(scene + 1, &endp, 10);
            if (!scene[0] || endp == scene + 1) {
                ok = 0;   /* (Java: a NumberFormatException, the download fails) */
                goto next_line;
            }
            if (scene[0] == 'A') {
                strcpy(bank, "3700E");
                clip = num;
            } else if (scene[0] == 'B') {
                strcpy(bank, "3701E");
                clip = num + 2;
            } else if (scene[0] == 'C' && num >= 0 && num < 5) {
                strcpy(bank, "1821E");
                clip = SUB_NEWS_CLIP[num];
            } else
                goto next_line;
        }
        s = strtod(r[st], NULL);
        e = strtod(r[en], NULL);
        {
            char *o = r[ov];
            while (*o == ' ')
                o++;
            if (*o) {
                char *dash = strchr(o, '-');
                s = strtod(o, NULL);
                e = dash ? strtod(dash + 1, NULL) : e;
            }
        }
        ascii_text(r[fi][0] ? r[fi] : r[tx], &t);
        text_ok = t.p && t.p[0];
        if (text_ok) {
            char num_s[96];
            snprintf(num_s, sizeof num_s, "%s %d %.2f %.2f ", bank, clip, s, e);   /* (as Python's %.2f) */
            sb_str(out, num_s);
            sb_str(out, t.p);
            sb_str(out, "\n");
        }
        free(t.p);
    next_line:
        for (i = 0; i < nr; i++)
            free(r[i]);
    }
    for (i = 0; i < nh; i++)
        free(head[i]);
    return ok;
}

static int SDLCALL subtitles_thread(void *unused)
{
    char real[MAX_PATH], tmp[MAX_PATH + 16], dst[MAX_PATH + 32], hex[65];
    FILE *f;
    char *data = NULL;
    long n = 0;
    sbuf out = { 0 };
    (void)unused;
    compat_path(g_sub_dir, real, sizeof real);
    mkdirs(real);
    snprintf(tmp, sizeof tmp, "%s/subtitles_en.csv.part", real);
    if (!download(SUB_URL, tmp)) {
        unlink(tmp);
        snprintf(g_vmsg, sizeof g_vmsg, "%s", "The subtitles couldn't be downloaded: no connection, or no curl or "
                 "wget on this system");
        g_vstate = TASK_FAILED;
        return 0;
    }
    f = fopen(tmp, "rb");
    if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = (char *)malloc((size_t)n + 1);
        if (data && fread(data, 1, (size_t)n, f) == (size_t)n)
            data[n] = 0;
        else {
            free(data);
            data = NULL;
        }
    }
    if (f)
        fclose(f);
    unlink(tmp);
    if (data)
        gamefiles_sha256(data, (size_t)n, hex);
    if (!data || strcmp(hex, SUB_SHA256) != 0) {
        snprintf(g_vmsg, sizeof g_vmsg, "The subtitles couldn't be downloaded: the file from %s is not the expected "
                 "one, so nothing was written (the repository may have changed)", SUB_REPO);
        free(data);
        g_vstate = TASK_FAILED;
        return 0;
    }
    if (!convert(data, &out)) {
        snprintf(g_vmsg, sizeof g_vmsg, "%s", "The subtitles couldn't be downloaded: the script couldn't be read");
        free(data);
        free(out.p);
        g_vstate = TASK_FAILED;
        return 0;
    }
    free(data);
    snprintf(tmp, sizeof tmp, "%s/subtitles_en.txt.part", real);
    snprintf(dst, sizeof dst, "%s/subtitles_en.txt", real);
    f = fopen(tmp, "wb");
    if (!f || fwrite(out.p, 1, out.len, f) != out.len || fclose(f) != 0 || rename(tmp, dst) != 0) {
        if (f)
            unlink(tmp);
        snprintf(g_vmsg, sizeof g_vmsg, "The subtitles couldn't be downloaded: couldn't write %s", dst);
        free(out.p);
        g_vstate = TASK_FAILED;
        return 0;
    }
    free(out.p);
    log_printf("gamefiles: subtitles written to %s", dst);
    snprintf(g_vmsg, sizeof g_vmsg, "%s", "Subtitles downloaded");
    g_vstate = TASK_DONE;
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
