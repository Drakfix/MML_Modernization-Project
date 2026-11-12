/* win_http.h: HTTPS downloads for the Windows start menu (the updater, the subtitles), with
 * Windows' own WinHTTP: the system's proxy settings and certificates, nothing shipped. Redirects
 * (GitHub's downloads go through one) are followed. */
#ifdef _WIN32   /* (the start menu, and the game for its update check: hints.c) */
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "win_http.h"

#pragma comment(lib, "winhttp.lib")

int win_http_get(const char *url, const char *path, char **mem, size_t *len, volatile int64_t *done,
                 volatile int64_t *total)
{
    wchar_t wurl[2048], host[256], urlpath[1800];
    URL_COMPONENTSW uc;
    HINTERNET s = NULL, c = NULL, r = NULL;
    DWORD status = 0, sz = sizeof status, got;
    wchar_t clen[32];
    DWORD clen_sz = sizeof clen;
    FILE *f = NULL;
    char *buf = NULL;
    size_t have = 0, cap = 0;
    int ok = 0;
    if (mem)
        *mem = NULL;
    if (len)
        *len = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048))
        return 0;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = urlpath;
    uc.dwUrlPathLength = 1800;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
        return 0;
    s = WinHttpOpen(L"MML-Modernization-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)   /* (before Windows 8.1: the default proxy setting) */
        s = WinHttpOpen(L"MML-Modernization-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (s)
        WinHttpSetTimeouts(s, 15000, 15000, 30000, 30000);
    c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    r = c ? WinHttpOpenRequest(c, L"GET", urlpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!r || !WinHttpSendRequest(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n",
                                  (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, NULL) ||
        !WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &sz, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        log_printf("http: %s failed (status %lu, error %lu)", url, status, GetLastError());
        goto out;
    }
    if (total && WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &clen_sz,
                                     WINHTTP_NO_HEADER_INDEX))
        *total = _wtoi64(clen);
    if (path && fopen_s(&f, path, "wb") != 0) {
        log_printf("http: can't write %s", path);
        goto out;
    }
    for (;;) {
        char chunk[1 << 16];
        if (!WinHttpReadData(r, chunk, sizeof chunk, &got)) {
            log_printf("http: %s broke off (error %lu)", url, GetLastError());
            goto out;
        }
        if (!got)
            break;
        if (f && fwrite(chunk, 1, got, f) != got)
            goto out;
        if (mem) {
            if (have + got + 1 > cap) {
                char *nb;
                cap = (have + got + 1) * 2;
                nb = (char *)realloc(buf, cap);
                if (!nb)
                    goto out;
                buf = nb;
            }
            memcpy(buf + have, chunk, got);
            buf[have + got] = 0;
        }
        have += got;
        if (done)
            *done = (int64_t)have;
    }
    ok = 1;
out:
    if (f && fclose(f) != 0)
        ok = 0;
    if (ok && mem) {
        *mem = buf;
        if (len)
            *len = have;
    } else
        free(buf);
    if (r)
        WinHttpCloseHandle(r);
    if (c)
        WinHttpCloseHandle(c);
    if (s)
        WinHttpCloseHandle(s);
    return ok;
}
#else
typedef int win_http_unused;   /* (the Windows start menu's only) */
#endif
