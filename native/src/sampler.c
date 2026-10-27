/* Dev tool: sampling profiler for the game thread ([debug] sampler=1).
 * Every millisecond it stops the game thread, notes where it is, and every
 * 5000 samples logs the hottest 256-byte code regions with their module
 * (look the offsets up in build\mml.map for mml.dll). It starts 46 s in, so
 * the samples come from play rather than loading. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#include "log.h"
#include "sampler.h"

#define BUCKETS 65536
static HANDLE g_target;
static struct { uint32_t addr; unsigned n; } g_hist[BUCKETS];
static unsigned g_total;

static void count(uint32_t eip)
{
    uint32_t key = eip >> 8, i = (key * 2654435761u) & (BUCKETS - 1);
    while (g_hist[i].n && g_hist[i].addr != key)
        i = (i + 1) & (BUCKETS - 1);
    g_hist[i].addr = key;
    g_hist[i].n++;
}

static void dump(void)
{
    int top, i;
    log_printf("sampler: %u samples", g_total);
    for (top = 0; top < 25; top++) {
        int best = -1;
        char name[MAX_PATH] = "?";
        HMODULE m = NULL;
        for (i = 0; i < BUCKETS; i++)
            if (g_hist[i].n && (best < 0 || g_hist[i].n > g_hist[best].n))
                best = i;
        if (best < 0)
            break;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)(g_hist[best].addr << 8), &m) && m) {
            GetModuleFileNameA(m, name, sizeof name);
            log_printf("sampler: %5.1f%%  %08X  %s+%X", 100.0 * g_hist[best].n / g_total,
                       g_hist[best].addr << 8, strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name,
                       (g_hist[best].addr << 8) - (uint32_t)(uintptr_t)m);
        } else {
            log_printf("sampler: %5.1f%%  %08X", 100.0 * g_hist[best].n / g_total, g_hist[best].addr << 8);
        }
        g_hist[best].n = 0;
    }
    memset(g_hist, 0, sizeof g_hist);
    g_total = 0;
}

static DWORD WINAPI sampler_thread(void *p)
{
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    (void)p;
    Sleep(46000);   /* let the game get somewhere first */
    for (;;) {
        CONTEXT c;
        LARGE_INTEGER due;
        due.QuadPart = -10000;   /* 1 ms */
        if (t && SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(t, INFINITE);
        else
            Sleep(1);
        if (SuspendThread(g_target) == (DWORD)-1)
            continue;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_target, &c))
            count(c.Eip);
        ResumeThread(g_target);
        if (++g_total == 5000)
            dump();
    }
}

void sampler_start(void)
{
    if (g_target)
        return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    log_printf("sampler: profiling the game thread");
}
