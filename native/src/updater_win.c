/* updater.h for the Windows start menu: what mml_update.py does for the Python launcher. The newest
 * release on GitHub (releases/latest, so pre-releases never count), its MML-Setup.exe downloaded
 * into the temp folder and checked against the size and SHA-256 GitHub lists for it, then started
 * on this game folder (--dir); the start menu closes so Setup can replace its files. The work runs
 * on threads of its own; the menu polls it. */
#if defined(_WIN32) && defined(MML_RUNNER)
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamefiles.h"
#include "log.h"
#include "mml.h"
#include "updater.h"
#include "win_http.h"

#define API "https://api.github.com/repos/Drakfix/MML_Modernization-Project/releases/latest"
#define ASSET "MML-Setup.exe"

static volatile int g_state = UPD_IDLE;
static volatile int64_t g_done, g_total;
static char g_msg[200], g_ver[48], g_url[1024], g_digest[80];
static int64_t g_size;

int update_can(void)
{
    return 1;
}

const char *update_current(void)
{
    static char v[48];
    const char *sp;
    if (!v[0]) {   /* MML_NATIVE_BUILD "0.7.2-beta" (anything after a space is the build's note) */
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
            n[k++] = strtoll(s, (char **)&s, 10);
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
    if (!win_http_get(API, NULL, &json, &len, NULL, NULL)) {
        strcpy_s(g_msg, sizeof g_msg, "GitHub couldn't be reached");
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
    /* the asset named MML-Setup.exe: its size, digest and download address follow its name */
    g_url[0] = 0;
    for (p = json; (p = strstr(p, "\"name\"")) != NULL;) {
        p = json_str(p, "name", name, sizeof name);
        if (_stricmp(name, ASSET) == 0) {
            const char *s = strstr(p, "\"size\"");
            json_str(p, "digest", g_digest, sizeof g_digest);
            json_str(p, "browser_download_url", g_url, sizeof g_url);
            g_size = 0;
            if (s) {
                s += 6;
                while (*s == ' ' || *s == ':')
                    s++;
                _snprintf_s(num, sizeof num, _TRUNCATE, "%.20s", s);
                g_size = _atoi64(num);
            }
            break;
        }
    }
    free(json);
    if (!g_url[0]) {
        _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "%s has no %s yet", tag, ASSET);
        g_state = UPD_FAILED;
        return 0;
    }
    _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Version %s is out: Update downloads its Setup and runs it", g_ver);
    log_printf("update: %s available (%s, %lld bytes)", g_ver, g_url, (long long)g_size);
    g_state = UPD_AVAILABLE;
    return 0;
}

static DWORD WINAPI install_thread(LPVOID unused)
{
    char dir[MAX_PATH], tmp[MAX_PATH], part[MAX_PATH + 8], exe[MAX_PATH + 8], game[MAX_PATH], cmd[2 * MAX_PATH + 32];
    char hex[65] = "", *data = NULL, *slash;
    size_t len = 0;
    int have;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    FILE *f;
    (void)unused;
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sMML-update", tmp);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(part, sizeof part, _TRUNCATE, "%s\\%s.part", dir, ASSET);
    _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s\\%s", dir, ASSET);
    g_done = 0;
    g_total = g_size;
    if (!win_http_get(g_url, part, NULL, NULL, &g_done, &g_total)) {
        strcpy_s(g_msg, sizeof g_msg, "The download stopped: try again");
        g_state = UPD_FAILED;
        return 0;
    }
    if (fopen_s(&f, part, "rb") == 0) {   /* checked as a whole: the size and the SHA-256 GitHub lists */
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
        DeleteFileA(part);
        strcpy_s(g_msg, sizeof g_msg, "The download doesn't match its checksum: try again");
        log_printf("update: %s failed its check (%zu bytes, sha256 %s, listed %s)", part, len, hex, g_digest);
        g_state = UPD_FAILED;
        return 0;
    }
    DeleteFileA(exe);
    if (!MoveFileA(part, exe)) {
        strcpy_s(g_msg, sizeof g_msg, "The download couldn't be saved");
        g_state = UPD_FAILED;
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
    if (g_state != UPD_AVAILABLE)
        return;
    _snprintf_s(g_msg, sizeof g_msg, _TRUNCATE, "Downloading Setup %s", g_ver);
    g_state = UPD_DOWNLOADING;
    run(install_thread);
}

int update_poll(char *msg, size_t n, char *ver, size_t vn, float *progress)
{
    int st = g_state;
    if (n)
        strcpy_s(msg, n, g_msg);
    if (vn)
        strcpy_s(ver, vn, g_ver);
    *progress = st == UPD_DOWNLOADING && g_total > 0 ? (float)((double)g_done / (double)g_total) : 0;
    return st;
}
#else
typedef int updater_win_unused;   /* (the Windows start menu's only; Android: updater.c) */
#endif
