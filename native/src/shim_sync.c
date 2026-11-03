/* KERNEL32 events and waits. The game creates events for DirectSound's position notifications
 * (FUN_004F9AD6) and polls them once a frame with MsgWaitForMultipleObjects, timeout 0
 * (FUN_004FA8A3); our DirectSound (shim_dsound.c) signals them from the audio thread. The C
 * runtime makes one more it never waits on; SetEvent / WaitForSingleObject are only in the
 * thread code the game never runs. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "plat.h"
#include "recomp_rt.h"
#include "shim.h"

#define EVENT_BASE 0x7F010000u   /* + 4i; file handles are at 0x7F000000 */
#define MAX_EVENTS 64
static struct {
    int used, manual;
    volatile long signaled;
} g_ev[MAX_EVENTS];

int g_sync_log;

static int ev_index(uint32_t h)
{
    uint32_t i = (h - EVENT_BASE) / 4;
    if (h < EVENT_BASE || (h & 3) || i >= MAX_EVENTS || !g_ev[i].used)
        return -1;
    return (int)i;
}

uint32_t shim_event_create(int manual, int initial)
{
    int i;
    for (i = 0; i < MAX_EVENTS; i++)
        if (!g_ev[i].used) {
            g_ev[i].used = 1;
            g_ev[i].manual = manual;
            plat_atomic_xchg(&g_ev[i].signaled, initial != 0);
            return EVENT_BASE + 4u * (uint32_t)i;
        }
    log_printf("shims: out of events");
    return 0;
}

void shim_event_set(uint32_t h)
{
    int i = ev_index(h);
    if (i >= 0)
        plat_atomic_xchg(&g_ev[i].signaled, 1);
}

int shim_event_close(uint32_t h)
{
    int i = ev_index(h);
    if (i < 0)
        return 0;
    g_ev[i].used = 0;
    return 1;
}

/* signaled now: an auto-reset event is reset by the wait that sees it */
static int ev_take(int i)
{
    if (g_ev[i].manual)
        return g_ev[i].signaled != 0;
    return plat_atomic_xchg(&g_ev[i].signaled, 0) != 0;
}

/* CreateEventA(security, manual, initial, name) */
static uint32_t sh_create_event_a(const uint32_t *a)
{
    if (a[3])
        log_printf("shims: named event \"%s\" made unnamed", (const char *)HPTR(a[3]));
    return shim_event_create(a[1] != 0, a[2] != 0);
}

static uint32_t sh_set_event(const uint32_t *a)
{
    if (ev_index(a[0]) < 0) {
        shim_set_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    shim_event_set(a[0]);
    return 1;
}

/* the first of n handles that is signaled, within ms (INFINITE: forever) */
static uint32_t wait_any(const uint32_t *handles, uint32_t n, uint32_t ms)
{
    uint32_t start = plat_ticks_ms(), k;
    for (;;) {
        for (k = 0; k < n; k++) {
            int i = ev_index(handles[k]);
            if (i >= 0 && ev_take(i)) {
                if (g_sync_log)
                    log_printf("shims: wait: event %08X (%u of %u)", handles[k], k, n);
                return k;   /* WAIT_OBJECT_0 + k */
            }
        }
        if (ms != INFINITE && plat_ticks_ms() - start >= ms)
            return WAIT_TIMEOUT;
        plat_sleep_ms(1);
    }
}

/* WaitForSingleObject(handle, ms) */
static uint32_t sh_wait_for_single_object(const uint32_t *a)
{
    if (ev_index(a[0]) < 0) {
        log_printf("shims: WaitForSingleObject on %08X, not an event", a[0]);
        shim_set_error(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    return wait_any(a, 1, a[1]);
}

/* MsgWaitForMultipleObjects(count, handles, wait_all, ms, wake_mask): the game asks with
   wait_all 0 and timeout 0; window messages are its own loop's business */
static uint32_t sh_msg_wait_for_multiple_objects(const uint32_t *a)
{
    uint32_t h[64];
    uint32_t n = a[0] < 64 ? a[0] : 64;
    static int warned;
    if ((a[2] || a[3]) && !warned++)
        log_printf("shims: MsgWaitForMultipleObjects with wait_all %u, timeout %u: treated as any, no messages",
                   a[2], a[3]);
    memcpy(h, HPTR(a[1]), 4 * (size_t)n);
    return wait_any(h, n, a[3]);
}

void shim_sync_install(void)
{
    shim_add("KERNEL32.dll", "CreateEventA", 4, sh_create_event_a);
    shim_add("KERNEL32.dll", "SetEvent", 1, sh_set_event);
    shim_add("KERNEL32.dll", "WaitForSingleObject", 2, sh_wait_for_single_object);
    shim_add("USER32.dll", "MsgWaitForMultipleObjects", 5, sh_msg_wait_for_multiple_objects);
}
