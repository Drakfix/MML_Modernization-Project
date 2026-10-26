/* Dev tool: hardware write watchpoints ([debug] watch=ADDR,SIZE[;ADDR,SIZE...]).
 *
 * Up to four addresses get a CPU data breakpoint (DR0-DR3) on the game's main
 * thread; every write traps into a vectored exception handler that records
 * which instruction wrote (the instruction before the reported address) and
 * lets the game continue. Unique writers are logged with hit counts every
 * few seconds. Used to find who updates a field when static search can't
 * (writes through pointers). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "watch.h"

#define MAX_WATCH   4
#define MAX_WRITERS 64

static struct { uintptr_t addr; int size; } g_w[MAX_WATCH];
static int g_nw;
static DWORD g_main_tid;
static int g_armed;
static struct { uintptr_t eip; int slot; unsigned hits; } g_writers[MAX_WRITERS];
static volatile LONG g_lock;

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    DWORD dr6;
    int i, k;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    dr6 = ep->ContextRecord->Dr6;
    if (!(dr6 & 0xF))
        return EXCEPTION_CONTINUE_SEARCH;
    while (InterlockedExchange(&g_lock, 1))
        ;
    for (i = 0; i < g_nw; i++) {
        if (!(dr6 & (1u << i)))
            continue;
        /* data breakpoints trap after the write: Eip is the next instruction */
        for (k = 0; k < MAX_WRITERS && g_writers[k].eip; k++)
            if (g_writers[k].eip == ep->ContextRecord->Eip && g_writers[k].slot == i)
                break;
        if (k < MAX_WRITERS) {
            g_writers[k].eip = ep->ContextRecord->Eip;
            g_writers[k].slot = i;
            g_writers[k].hits++;
        }
    }
    InterlockedExchange(&g_lock, 0);
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI arm_thread(LPVOID unused)
{
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_main_tid);
    CONTEXT c;
    int i;
    (void)unused;
    if (!t)
        return 0;
    SuspendThread(t);
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(t, &c)) {
        DWORD dr7 = 0;
        for (i = 0; i < g_nw; i++) {
            DWORD len = g_w[i].size == 4 ? 3 : g_w[i].size == 2 ? 1 : 0;
            (&c.Dr0)[i] = (DWORD)g_w[i].addr;
            dr7 |= 1u << (i * 2);                        /* local enable */
            dr7 |= (1u | (len << 2)) << (16 + i * 4);    /* RW = write, LEN */
        }
        c.Dr7 = dr7;
        SetThreadContext(t, &c);
    }
    ResumeThread(t);
    CloseHandle(t);
    return 0;
}

void watch_configure(const char *spec)
{
    const char *p = spec;
    while (p && *p && g_nw < MAX_WATCH) {
        char *end;
        uintptr_t a = (uintptr_t)strtoul(p, &end, 16);
        int sz = 1;
        if (end == p)
            break;
        if (*end == ',')
            sz = (int)strtol(end + 1, &end, 10);
        if (sz != 1 && sz != 2 && sz != 4)
            sz = 1;
        g_w[g_nw].addr = a & ~(uintptr_t)(sz - 1);
        g_w[g_nw].size = sz;
        g_nw++;
        p = strchr(end, ';');
        if (p)
            p++;
    }
    if (g_nw) {
        AddVectoredExceptionHandler(1, on_exception);
        log_printf("watch: %d write watchpoint(s) configured", g_nw);
    }
}

void watch_frame(void)
{
    static unsigned frames;
    int k;
    if (!g_nw)
        return;
    if (!g_armed) {
        HANDLE h;
        g_main_tid = GetCurrentThreadId();
        h = CreateThread(NULL, 0, arm_thread, NULL, 0, NULL);
        if (h) {
            WaitForSingleObject(h, 2000);
            CloseHandle(h);
        }
        g_armed = 1;
        log_printf("watch: armed on thread %lu", g_main_tid);
    }
    if (++frames % 90)
        return;
    while (InterlockedExchange(&g_lock, 1))
        ;
    for (k = 0; k < MAX_WRITERS && g_writers[k].eip; k++) {
        if (g_writers[k].hits)
            log_printf("watch: %08X (%d bytes) written by code before %08X, %u times",
                       (unsigned)g_w[g_writers[k].slot].addr, g_w[g_writers[k].slot].size,
                       (unsigned)g_writers[k].eip, g_writers[k].hits);
        g_writers[k].hits = 0;
    }
    InterlockedExchange(&g_lock, 0);
}
