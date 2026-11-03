/* The game's address space (guest.h).
 *
 * In the DLL the game's addresses are the process's own: guest_alloc is calloc and pages are
 * Windows' VirtualAlloc. In the standalone runtime (MML_RUNNER) the game lives in one block we
 * reserve, at G_MEMBASE = g_membase: page regions (the exe image, the game's own VirtualAlloc
 * heap, the game's stack) are handed out from it with Windows' reserve / commit / query
 * semantics, and guest_alloc runs a small first-fit heap in regions of its own. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "log.h"

#ifndef MML_RUNNER

uint32_t guest_alloc(size_t n)
{
    return (uint32_t)(uintptr_t)calloc(1, n);
}

void guest_free(uint32_t g)
{
    free((void *)(uintptr_t)g);
}

uint32_t guest_pages(uint32_t at, size_t n, int commit)
{
    return (uint32_t)(uintptr_t)VirtualAlloc((void *)(uintptr_t)at, n, MEM_RESERVE | (commit ? MEM_COMMIT : 0),
                                             PAGE_READWRITE);
}

#else

uintptr_t g_membase;

#define GUEST_SIZE   0x20000000u   /* 512 MB of game address space */
#define GRANULE      0x10000u      /* Windows' allocation granularity */
#define PAGE         0x1000u
#define FIRST_FREE   0x01000000u   /* below: the exe image's neighbourhood (0x400000 + its size) */
#define MAX_REGIONS  4096

static struct {
    uint32_t base, size;   /* game addresses; size a multiple of GRANULE */
} g_regions[MAX_REGIONS];
static int g_nregions;

int guest_init(void)
{
    g_membase = (uintptr_t)VirtualAlloc(NULL, GUEST_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_membase) {
        log_printf("guest: can't reserve %u MB for the game", GUEST_SIZE >> 20);
        return 0;
    }
    log_printf("guest: the game's memory is %u MB at host %p", GUEST_SIZE >> 20, (void *)g_membase);
    return 1;
}

static int region_of(uint32_t a)
{
    int k;
    for (k = 0; k < g_nregions; k++)
        if (a >= g_regions[k].base && a - g_regions[k].base < g_regions[k].size)
            return k;
    return -1;
}

static int overlaps(uint32_t base, uint32_t size)
{
    int k;
    for (k = 0; k < g_nregions; k++)
        if (base < g_regions[k].base + g_regions[k].size && g_regions[k].base < base + size)
            return 1;
    return 0;
}

static int commit(uint32_t a, size_t n)
{
    uint32_t lo = a & ~(PAGE - 1), hi = (uint32_t)((a + n + PAGE - 1) & ~(size_t)(PAGE - 1));
    return VirtualAlloc((void *)(g_membase + lo), hi - lo, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

/* a region of n bytes at game address `at` (0: anywhere), reserved, committed if asked */
uint32_t guest_pages(uint32_t at, size_t n, int commit_it)
{
    uint32_t size = (uint32_t)((n + GRANULE - 1) & ~(size_t)(GRANULE - 1)), base;
    if (!n || g_nregions >= MAX_REGIONS)
        return 0;
    if (at) {
        base = at & ~(GRANULE - 1);
        size = (uint32_t)((at + n - base + GRANULE - 1) & ~(size_t)(GRANULE - 1));
        if (base + size > GUEST_SIZE || overlaps(base, size))
            return 0;
    } else {
        for (base = FIRST_FREE; base + size <= GUEST_SIZE; base += GRANULE) {
            int k = region_of(base);
            if (k >= 0) {   /* skip past it */
                base = g_regions[k].base + g_regions[k].size - GRANULE;
                continue;
            }
            if (!overlaps(base, size))
                break;
        }
        if (base + size > GUEST_SIZE) {
            int k;
            uint32_t used = 0;
            for (k = 0; k < g_nregions; k++)
                used += g_regions[k].size;
            log_printf("guest: out of game memory for %u KB (%d regions, %u MB given out)", (unsigned)(n >> 10),
                       g_nregions, used >> 20);
            for (k = 0; k < g_nregions && k < 40; k++)
                log_printf("guest:   %08X + %X", g_regions[k].base, g_regions[k].size);
            return 0;
        }
    }
    g_regions[g_nregions].base = base;
    g_regions[g_nregions].size = size;
    g_nregions++;
    if (commit_it && !commit(at ? at : base, at ? n : size))
        return 0;
    return at ? at : base;
}

/* VirtualAlloc(addr, size, type, protect) for the game: a new region, or commit inside one */
uint32_t guest_virtual_alloc(uint32_t at, uint32_t n, uint32_t type)
{
    if (at && region_of(at) >= 0) {   /* commit (or re-reserve) inside a region we gave out */
        if ((type & MEM_COMMIT) && !commit(at, n))
            return 0;
        return at & ~(PAGE - 1);
    }
    return guest_pages(at, n, (type & MEM_COMMIT) != 0);
}

int guest_virtual_free(uint32_t at, uint32_t n, uint32_t type)
{
    int k = region_of(at);
    if (k < 0)
        return 0;
    if (type & MEM_RELEASE) {
        VirtualFree((void *)(g_membase + g_regions[k].base), g_regions[k].size, MEM_DECOMMIT);
        g_regions[k] = g_regions[--g_nregions];
        return 1;
    }
    return VirtualFree((void *)(g_membase + at), n, MEM_DECOMMIT) != 0;
}

/* VirtualQuery for a game address: MEMORY_BASIC_INFORMATION as the 32-bit game sees it */
uint32_t guest_virtual_query(uint32_t at, uint32_t out, uint32_t len)
{
    MEMORY_BASIC_INFORMATION mi;
    uint32_t m[7];
    int k = region_of(at);
    if (len < sizeof m)
        return 0;
    memset(m, 0, sizeof m);
    m[0] = at & ~(PAGE - 1);   /* BaseAddress */
    if (k >= 0 && VirtualQuery((void *)(g_membase + m[0]), &mi, sizeof mi)) {
        uint32_t end = g_regions[k].base + g_regions[k].size, run = (uint32_t)mi.RegionSize;
        m[1] = g_regions[k].base;                    /* AllocationBase */
        m[2] = PAGE_READWRITE;                       /* AllocationProtect */
        m[3] = run > end - m[0] ? end - m[0] : run;  /* RegionSize: same state, inside the region */
        m[4] = mi.State;
        m[5] = mi.State == MEM_COMMIT ? PAGE_READWRITE : 0;
        m[6] = MEM_PRIVATE;
    } else {
        m[3] = PAGE;
        m[4] = MEM_FREE;
        m[5] = PAGE_NOACCESS;
    }
    memcpy(GUEST(out), m, sizeof m);
    return sizeof m;
}

/* the regions and arenas (a crash report) */
void guest_dump(void)
{
    int k;
    for (k = 0; k < g_nregions; k++)
        log_printf("guest: region %08X + %X", g_regions[k].base, g_regions[k].size);
}

/* ---- guest_alloc: first-fit blocks with a 16-byte header, in 16 MB arenas ---- */
#define ARENA (16u << 20)
typedef struct {
    uint32_t size;   /* of the block, header included */
    uint32_t used;
    uint32_t pad[2];
} blk;
static uint32_t g_arenas[64], g_arena_size[64];
static int g_narenas;

static blk *blk_at(uint32_t g)
{
    return (blk *)GUEST(g);
}

static int new_arena(size_t need)
{
    uint32_t size = need + 16 > ARENA ? (uint32_t)((need + 16 + GRANULE - 1) & ~(size_t)(GRANULE - 1)) : ARENA;
    uint32_t a;
    if (g_narenas >= 64 || !(a = guest_pages(0, size, 1)))
        return 0;
    blk_at(a)->size = size;
    blk_at(a)->used = 0;
    g_arenas[g_narenas] = a;
    g_arena_size[g_narenas++] = size;
    return 1;
}

uint32_t guest_alloc(size_t n)
{
    uint32_t need = (uint32_t)((n + 16 + 15) & ~(size_t)15);
    int pass, k;
    for (pass = 0; pass < 2; pass++) {
        for (k = 0; k < g_narenas; k++) {
            uint32_t a = g_arenas[k], end = a + g_arena_size[k];
            while (a < end) {
                blk *b = blk_at(a);
                if (!b->used) {   /* merge the free blocks after it first */
                    while (a + b->size < end && !blk_at(a + b->size)->used)
                        b->size += blk_at(a + b->size)->size;
                    if (b->size >= need) {
                        if (b->size - need >= 64) {   /* split */
                            blk *rest = blk_at(a + need);
                            rest->size = b->size - need;
                            rest->used = 0;
                            b->size = need;
                        }
                        b->used = 1;
                        memset(b + 1, 0, b->size - 16);
                        return a + 16;
                    }
                }
                a += b->size;
            }
        }
        if (!new_arena(n))
            break;
    }
    log_printf("guest: out of memory for %u bytes", (unsigned)n);
    return 0;
}

void guest_free(uint32_t g)
{
    if (g)
        blk_at(g - 16)->used = 0;
}

#endif
