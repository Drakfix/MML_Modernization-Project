#include <windows.h>
#include <string.h>

#include "hook.h"
#include "log.h"

static uint8_t *g_stub_page;
static size_t g_stub_used;

static uint8_t *stub_alloc(size_t n)
{
    uint8_t *p;
    if (!g_stub_page) {
        g_stub_page = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_stub_page)
            return NULL;
    }
    if (g_stub_used + n > 0x10000)
        return NULL;
    p = g_stub_page + g_stub_used;
    g_stub_used += (n + 15) & ~(size_t)15;
    return p;
}

int mem_expect(uintptr_t addr, const void *bytes, size_t n)
{
    return memcmp((const void *)addr, bytes, n) == 0;
}

int mem_write(uintptr_t addr, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)addr, bytes, n);
    VirtualProtect((void *)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
    return 1;
}

static void put_rel32(uint8_t *at, uintptr_t from_next, uintptr_t target)
{
    int32_t rel = (int32_t)(target - from_next);
    memcpy(at, &rel, 4);
}

int hook_mid(uintptr_t site, size_t steal, const uint8_t *expect, hook_fn fn)
{
    uint8_t *s, *p;
    uint8_t jmp[16];
    size_t i;

    if (steal < 5 || steal > sizeof jmp) {
        log_printf("hook %08X: bad steal size %u", (unsigned)site, (unsigned)steal);
        return 0;
    }
    if (!mem_expect(site, expect, steal)) {
        log_printf("hook %08X: unexpected bytes, not hooking", (unsigned)site);
        return 0;
    }
    s = stub_alloc(32 + steal);
    if (!s)
        return 0;

    p = s;
    *p++ = 0x9C;                 /* pushfd */
    *p++ = 0x60;                 /* pushad */
    *p++ = 0x54;                 /* push esp   -> regs_t* */
    *p++ = 0xE8;                 /* call fn */
    put_rel32(p, (uintptr_t)p + 4, (uintptr_t)fn);
    p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;   /* add esp,4 */
    *p++ = 0x61;                 /* popad */
    *p++ = 0x9D;                 /* popfd */

    /* replay the stolen instruction(s) */
    if (expect[0] == 0xE8 || expect[0] == 0xE9) {
        int32_t rel;
        uintptr_t target;
        if (steal != 5) {
            log_printf("hook %08X: relative branch must be stolen alone", (unsigned)site);
            return 0;
        }
        memcpy(&rel, expect + 1, 4);
        target = site + 5 + rel;
        *p++ = expect[0];
        put_rel32(p, (uintptr_t)p + 4, target);
        p += 4;
    } else {
        memcpy(p, expect, steal);
        p += steal;
    }
    *p++ = 0xE9;                 /* jmp back */
    put_rel32(p, (uintptr_t)p + 4, site + steal);
    p += 4;
    FlushInstructionCache(GetCurrentProcess(), s, (size_t)(p - s));

    jmp[0] = 0xE9;
    put_rel32(jmp + 1, site + 5, (uintptr_t)s);
    for (i = 5; i < steal; i++)
        jmp[i] = 0x90;
    return mem_write(site, jmp, steal);
}
