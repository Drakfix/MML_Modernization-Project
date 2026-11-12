/* updater.h on Windows and Linux. The newest release on GitHub (releases/latest, so pre-releases
 * never count) and its package for this platform, downloaded and checked against the size and
 * SHA-256 GitHub lists for it, then installed:
 *  - Windows (the start menu, mml_run64.exe --menu): MML-Setup.exe, started on this folder
 *    (--dir); the start menu closes so Setup can replace its files. The game (mml.dll, or the
 *    runner without --menu) only checks: the title screen says when one is out (hints.c).
 *  - Linux: mml-linux-<version>-x86_64.tar.gz, unpacked beside the app and copied over its
 *    files (anything else in the folder stays); the start menu then starts the new program.
 *    Downloads go through curl, else wget, as the subtitles' do.
 * The work runs on threads of its own; the caller polls it. */
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "win_http.h"
#else
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "gamefiles.h"
#include "log.h"
#include "mml.h"
#include "updater.h"

#define API "https://api.github.com/repos/Drakfix/MML_Modernization-Project/releases/latest"
#if defined(_WIN32) && defined(MML_RUNNER)
#define CAN_INSTALL 1
#elif defined(_WIN32)
#define CAN_INSTALL 0   /* (mml.dll: the game only checks) */
#else
#define CAN_INSTALL 1
#endif

static volatile int g_state = UPD_IDLE;
static volatile int64_t g_done, g_total;
static char g_msg[200], g_ver[48], g_url[1024], g_digest[80], g_asset[128];
static int64_t g_size;

/* this platform's package among the release's files */
static int asset_match(const char *name)
{
#ifdef _WIN32
    return _stricmp(name, "MML-Setup.exe") == 0;
#else
    size_t n = strlen(name), t = strlen("-x86_64.tar.gz");
    return strncmp(name, "mml-linux-", 10) == 0 && n > t && strcmp(name + n - t, "-x86_64.tar.gz") == 0;
#endif
}

#ifdef _WIN32
static int get_mem(const char *url, char **mem, size_t *len)
{
    return win_http_get(url, NULL, mem, len, NULL, NULL);
}
#else
static char g_part[PATH_MAX];   /* the download under way (its size so far: the progress) */

/* s in single quotes for the shell */
static void sh_quote(const char *s, char *out, size_t n)
{
    size_t o = 0;
    if (n < 3)
        return;
    out[o++] = '\'';
    for (; *s && o + 5 < n; s++) {
        if (*s == '\'') {
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else
            out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = 0;
}

/* url into the file path, with curl, else wget */
static int get_file(const char *url, const char *path)
{
    char cmd[3 * PATH_MAX], qu[1200], qp[PATH_MAX + 64];
    sh_quote(url, qu, sizeof qu);
    sh_quote(path, qp, sizeof qp);
    snprintf(cmd, sizeof cmd,
             "curl -fsSL --connect-timeout 30 -A mml-modernization -o %s %s 2>/dev/null || "
             "wget -q -T 30 -U mml-modernization -O %s %s 2>/dev/null", qp, qu, qp, qu);
    return system(cmd) == 0;
}

static int get_mem(const char *url, char **mem, size_t *len)
{
    char tmp[] = "/tmp/mml-update-XXXXXX";
    int fd = mkstemp(tmp), ok = 0;
    FILE *f;
    if (fd < 0)
        return 0;
    close(fd);
    if (get_file(url, tmp) && (f = fopen(tmp, "rb")) != NULL) {
        long n;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        *mem = (char *)malloc((size_t)n + 1);
        if (*mem && fread(*mem, 1, (size_t)n, f) == (size_t)n) {
            (*mem)[n] = 0;
            *len = (size_t)n;
            ok = 1;
        } else {
            free(*mem);
            *mem = NULL;
        }
        fclose(f);
    }
    unlink(tmp);
    return ok;
}

/* the folder this program is in (the app's) */
static int app_dir(char *out, size_t n)
{
    ssize_t len = readlink("/proc/self/exe", out, n - 1);
    char *slash;
    if (len <= 0)
        return 0;
    out[len] = 0;
    if (len > 10 && strcmp(out + len - 10, " (deleted)") == 0)   /* (replaced while it runs) */
        out[len - 10] = 0;
    slash = strrchr(out, '/');
    if (!slash)
        return 0;
    *slash = 0;
    return 1;
}
#endif

int update_can(void)
{
    return 1;
}

const char *update_current(void)
{
    static char v[48];
    const char *sp;
    if (!v[0]) {   /* MML_NATIVE_BUILD "0.8.0-beta" (anything after a space is the build's note) */
        strcpy_s(v, sizeof v, MML_NATIVE_BUILD);
        if ((sp = strchr(v, ' ')) != NULL)
            v[sp - v] = 0;
    }
    return v;
}

/* "v0.7.3-beta" -> 0.7.3 as one number to compare (mml_update.py numbers) */
static long long version_number(const char *s)
{
    long long n[3] = { 0, 0, 0 };
    int k = 0;
    while (*s && k < 3) {
        if (*s >= '0' && *s <= '9') {
            char *end;
            n[k++] = strtoll(s, &end, 10);
            s = end;
            continue;
        }
        s++;
    }
    return (n[0] * 100000 + n[1]) * 100000 + n[2];
}

/* the JSON string value of "key" at or after from, into out ("" for null or none); where it ended */
static const char *json_str(const char *from, const char *key, char *out, size_t n)
{
    char pat[64];
    const char *p, *q;
    size_t len;
    out[0] = 0;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "\"%s\"", key);
    p = strstr(from, pat);
    if (!p)
        return from;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != '"')
        return p;   /* (null, a number) */
    q = strchr(++p, '"');
    if (!q)
        return p;
    len = (size_t)(q - p) < n - 1 ? (size_t)(q - p) : n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return q + 1;
}

static DWORD WINAPI check_thread(LPVOID unused)
{
    char *json = NULL, tag[64], name[128], num[32];
    const char *p;
    size_t len;
    (void)unused;
    if (!get_mem(API, &json, &len)) {
#ifdef _WIN32
        strcpy_s(g_msg, sizeof g_msg, "GitHub couldn't be reached");
#else
        strcpy_s(g_msg, sizeof g_msg, "GitHub couldn't be reached (curl or wget is needed)");
#endif
        g_state = UPD_FAILED;
        return 0;
    }
    json_str(json, "tag_name", tag, sizeof tag);
    strcpy_s(g_ver, sizeof g_ver, tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
    if (!tag[0] || version_number(tag) <= version_number(update_current())) {
        _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "This is the newest version (%s)", update_current());
        g_state = UPD_UPTODATE;
        free(json);
        return 0;
    }
    /* this platform's package: its size, digest and download address follow its name */
    g_url[0] = 0;
    for (p = json; (p = strstr(p, "\"name\"")) != NULL;) {
        p = json_str(p, "name", name, sizeof name);
        if (asset_match(name)) {
            const char *s = strstr(p, "\"size\"");
            strcpy_s(g_asset, sizeof g_asset, name);
            json_str(p, "digest", g_digest, sizeof g_digest);
            json_str(p, "browser_download_url", g_url, sizeof g_url);
            g_size = 0;
            if (s) {
                s += 6;
                while (*s == ' ' || *s == ':')
                    s++;
                _snprintf_s(num, sizeof num, _TRUNCATE, "%.20s", s);
                g_size = strtoll(num, NULL, 10);
            }
            break;
        }
    }
    free(json);
    if (!g_url[0]) {
        _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "%s has no package for this system yet", tag);
        g_state = UPD_FAILED;
        return 0;
    }
#ifdef _WIN32
    _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Version %s is out: Update downloads its Setup and runs it", g_ver);
#else
    _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Version %s is out: Update downloads it and installs it here", g_ver);
#endif
    log_printf("update: %s available (%s, %lld bytes)", g_ver, g_url, (long long)g_size);
    g_state = UPD_AVAILABLE;
    return 0;
}

#if CAN_INSTALL
/* the downloaded file checked as a whole: the size and the SHA-256 GitHub lists */
static int check_download(const char *path)
{
    char hex[65] = "", *data = NULL;
    size_t len = 0;
    int have;
    FILE *f;
    if (fopen_s(&f, path, "rb") == 0) {
        fseek(f, 0, SEEK_END);
        len = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        data = (char *)malloc(len ? len : 1);
        if (data && fread(data, 1, len, f) != len) {
            free(data);
            data = NULL;
        }
        fclose(f);
    }
    have = data != NULL;
    if (have)
        gamefiles_sha256(data, len, hex);
    free(data);
    if (!have || (g_size && (int64_t)len != g_size) ||
        (strncmp(g_digest, "sha256:", 7) == 0 && _stricmp(g_digest + 7, hex) != 0)) {
        log_printf("update: %s failed its check (%zu bytes, sha256 %s, listed %s)", path, len, hex, g_digest);
        return 0;
    }
    return 1;
}

static void fail(const char *msg)
{
    strcpy_s(g_msg, sizeof g_msg, msg);
    log_printf("update: %s", msg);
    g_state = UPD_FAILED;
}

#ifdef _WIN32
static DWORD WINAPI install_thread(LPVOID unused)
{
    char dir[MAX_PATH], tmp[MAX_PATH], part[MAX_PATH + 8], exe[MAX_PATH + 8], game[MAX_PATH], cmd[2 * MAX_PATH + 32];
    char *slash;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    (void)unused;
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sMML-update", tmp);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(part, sizeof part, _TRUNCATE, "%s\\%s.part", dir, g_asset);
    _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s\\%s", dir, g_asset);
    g_done = 0;
    g_total = g_size;
    if (!win_http_get(g_url, part, NULL, NULL, &g_done, &g_total)) {
        fail("The download stopped: try again");
        return 0;
    }
    if (!check_download(part)) {
        DeleteFileA(part);
        fail("The download doesn't match its checksum: try again");
        return 0;
    }
    DeleteFileA(exe);
    if (!MoveFileA(part, exe)) {
        fail("The download couldn't be saved");
        return 0;
    }
    GetModuleFileNameA(NULL, game, sizeof game);
    if ((slash = strrchr(game, '\\')) != NULL)
        *slash = 0;
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE, "\"%s\" --dir \"%s\"", exe, game);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    AllowSetForegroundWindow(ASFW_ANY);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Setup didn't start (error %lu)", GetLastError());
        g_state = UPD_FAILED;
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log_printf("update: Setup %s started on %s", g_ver, game);
    strcpy_s(g_msg, sizeof g_msg, "Setup is starting: the menu closes so it can update the game");
    g_state = UPD_INSTALLING;
    return 0;
}
#else
static DWORD WINAPI install_thread(LPVOID unused)
{
    char app[PATH_MAX], stage[PATH_MAX + 16], unpack[PATH_MAX + 32], top[PATH_MAX + 64], probe[PATH_MAX + 80];
    char cmd[4 * PATH_MAX], qa[PATH_MAX + 64], qs[PATH_MAX + 64], qp[PATH_MAX + 200], qu[PATH_MAX + 64];
    (void)unused;
    if (!app_dir(app, sizeof app)) {
        fail("The app's folder couldn't be found");
        return 0;
    }
    if (access(app, W_OK) != 0) {
        fail("The app's folder can't be written: download the new archive and replace the folder by hand");
        return 0;
    }
    /* everything happens in a folder of our own inside the app's, then the new files go over the old */
    snprintf(stage, sizeof stage, "%s/.mml-update", app);
    sh_quote(stage, qs, sizeof qs);
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", qs, qs);
    if (system(cmd) != 0) {
        fail("The update's folder couldn't be made");
        return 0;
    }
    snprintf(g_part, sizeof g_part, "%s/%s", stage, g_asset);
    g_done = 0;
    g_total = g_size;
    if (!get_file(g_url, g_part)) {
        fail("The download stopped: try again");
        return 0;
    }
    if (!check_download(g_part)) {
        fail("The download doesn't match its checksum: try again");
        return 0;
    }
    snprintf(unpack, sizeof unpack, "%s/new", stage);
    sh_quote(g_part, qp, sizeof qp);
    sh_quote(unpack, qu, sizeof qu);
    snprintf(cmd, sizeof cmd, "mkdir -p %s && tar -xzf %s -C %s", qu, qp, qu);
    if (system(cmd) != 0) {
        fail("The archive couldn't be unpacked");
        return 0;
    }
    /* the archive holds one folder (mml-linux) with the program in it */
    snprintf(top, sizeof top, "%s/mml-linux", unpack);
    snprintf(probe, sizeof probe, "%s/mml", top);
    if (access(probe, X_OK) != 0) {
        fail("The archive isn't the app's (no mml-linux/mml in it)");
        return 0;
    }
    /* over the old files: a file in use (the running program) is replaced, not rewritten */
    sh_quote(top, qu, sizeof qu);
    sh_quote(app, qa, sizeof qa);
    snprintf(cmd, sizeof cmd, "cp -a --remove-destination %s/. %s/", qu, qa);
    if (system(cmd) != 0) {
        fail("The new files couldn't all be copied: download the new archive and replace the folder by hand");
        return 0;
    }
    snprintf(cmd, sizeof cmd, "rm -rf %s", qs);
    if (system(cmd) != 0)
        log_printf("update: %s couldn't be removed", stage);
    log_printf("update: %s installed in %s", g_ver, app);
    _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Version %s installed: the app starts again", g_ver);
    g_state = UPD_INSTALLING;
    return 0;
}
#endif
#endif

static void run(LPTHREAD_START_ROUTINE fn)
{
    HANDLE t = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else {
        strcpy_s(g_msg, sizeof g_msg, "Couldn't start");
        g_state = UPD_FAILED;
    }
}

void update_check_begin(void)
{
    if (g_state == UPD_CHECKING || g_state == UPD_DOWNLOADING || g_state == UPD_INSTALLING)
        return;
    strcpy_s(g_msg, sizeof g_msg, "Asking GitHub for the newest version");
    g_state = UPD_CHECKING;
    run(check_thread);
}

void update_install_begin(void)
{
#if CAN_INSTALL
    if (g_state != UPD_AVAILABLE)
        return;
#ifdef _WIN32
    _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Downloading Setup %s", g_ver);
#else
    _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Downloading version %s", g_ver);
#endif
    g_state = UPD_DOWNLOADING;
    run(install_thread);
#endif
}

int update_poll(char *msg, size_t n, char *ver, size_t vn, float *progress)
{
    int st = g_state;
    if (n)
        strcpy_s(msg, n, g_msg);
    if (vn)
        strcpy_s(ver, vn, g_ver);
#ifndef _WIN32
    if (st == UPD_DOWNLOADING && g_part[0]) {   /* (curl writes the file as it goes) */
        struct stat sb;
        if (stat(g_part, &sb) == 0)
            g_done = sb.st_size;
    }
#endif
    *progress = st == UPD_DOWNLOADING && g_total > 0 ? (float)((double)g_done / (double)g_total) : 0;
    return st;
}
#else
typedef int updater_net_unused;   /* (Android: updater.c) */
#endif
