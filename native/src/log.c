#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "log.h"

static HANDLE g_log = INVALID_HANDLE_VALUE;

void log_open(const char *game_dir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mml.log", game_dir);
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
}

void log_printf(const char *fmt, ...)
{
    char buf[512];
    int n;
    DWORD wrote;
    va_list ap;

    if (g_log == INVALID_HANDLE_VALUE)
        return;
    va_start(ap, fmt);
    n = _vsnprintf_s(buf, sizeof buf - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    /* truncated: MSVC's returns -1, the standard vsnprintf the length it would have needed */
    if (n < 0 || n >= (int)sizeof buf - 2)
        n = (int)strlen(buf);
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "mml", buf);   /* (adb logcat -s mml) */
#endif
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(g_log, buf, (DWORD)n, &wrote, NULL);
}
