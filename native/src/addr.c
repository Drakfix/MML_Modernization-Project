/* Which game program runs, and US addresses translated to it (addr.h). */
#include <string.h>

#include "addr.h"
#include "guest.h"
#include "log.h"

extern const addr_seg addr_jp_segs[];
extern const int addr_jp_nsegs;

int addr_game = GAME_US;

/* the coroutine scheduler's first bytes: it saves ESP in a variable of the initialized data,
   which sits at a different address in each program */
static const struct { uint32_t at; uint8_t bytes[11]; int game; const char *name; } g_probe[] = {
    { 0x4EED08u, { 0x53, 0x51, 0x56, 0x57, 0x55, 0x89, 0x25, 0x10, 0x14, 0x55, 0x00 }, GAME_US,
      "US MEGAMAN.EXE" },
    { 0x4EEF28u, { 0x53, 0x51, 0x56, 0x57, 0x55, 0x89, 0x25, 0x74, 0x16, 0x55, 0x00 }, GAME_JP,
      "Japanese rockman.exe (Rockman DASH)" },
};

int addr_detect(void)
{
    size_t i;
    for (i = 0; i < sizeof g_probe / sizeof g_probe[0]; i++)
        if (memcmp(GUEST(g_probe[i].at), g_probe[i].bytes, sizeof g_probe[i].bytes) == 0) {
            addr_game = g_probe[i].game;
            log_printf("game: %s", g_probe[i].name);
            return addr_game;
        }
    /* (as before there was a Japanese build: every patch checks its bytes first) */
    log_printf("game: unknown build, treated as the US one");
    return -1;
}

static const addr_seg *find(uint32_t us)
{
    int lo = 0, hi = addr_jp_nsegs - 1;
    while (lo <= hi) {   /* the runs are sorted and don't overlap */
        int mid = (lo + hi) / 2;
        if (us < addr_jp_segs[mid].start)
            hi = mid - 1;
        else if (us >= addr_jp_segs[mid].end)
            lo = mid + 1;
        else
            return &addr_jp_segs[mid];
    }
    return NULL;
}

static uint32_t xlat(uint32_t us)
{
    const addr_seg *s;
    if (addr_game == GAME_US)
        return us;
    s = find(us);
    return s ? us + (uint32_t)s->delta : 0;
}

uint32_t addr_xlat(uint32_t us)
{
    static int logged;
    uint32_t r = xlat(us);
    if (!r && logged++ < 16)
        log_printf("game: US address %06X has no counterpart in this program", (unsigned)us);
    return r;
}

uint32_t addr_us(uint32_t running)
{
    int i;
    if (addr_game == GAME_US)
        return running;
    for (i = 0; i < addr_jp_nsegs; i++)
        if (running >= addr_jp_segs[i].start + (uint32_t)addr_jp_segs[i].delta &&
            running < addr_jp_segs[i].end + (uint32_t)addr_jp_segs[i].delta)
            return running - (uint32_t)addr_jp_segs[i].delta;
    return 0;
}

/* a 4-byte field at k of the expected bytes made to fit what is at the running address:
   a rel32 call/jump distance, or an absolute address of code or initialized data */
static int fix_field(uint8_t *b, size_t k, uint32_t us_at, uint32_t at, const uint8_t *m)
{
    uint32_t v, t;
    memcpy(&v, b + k, 4);
    if ((k >= 1 && (b[k - 1] == 0xE8 || b[k - 1] == 0xE9)) ||
        (k >= 2 && b[k - 2] == 0x0F && (b[k - 1] & 0xF0) == 0x80)) {
        t = xlat(us_at + (uint32_t)k + 4 + v);
        if (t) {
            uint32_t rel = t - (at + (uint32_t)k + 4);
            if (memcmp(&rel, m + k, 4) == 0) {
                memcpy(b + k, &rel, 4);
                return 1;
            }
        }
    }
    if (v >= 0x401000u && v < 0x559000u && (t = xlat(v)) != 0 && memcmp(&t, m + k, 4) == 0) {
        memcpy(b + k, &t, 4);
        return 1;
    }
    return 0;
}

int addr_expect(uint32_t at, const uint8_t *e, size_t n)
{
    const uint8_t *m = (const uint8_t *)GUEST(at);
    uint8_t b[64];
    uint32_t us_at;
    size_t i;
    if (memcmp(m, e, n) == 0)
        return 1;
    if (addr_game == GAME_US || n > sizeof b || !(us_at = addr_us(at)))
        return 0;
    memcpy(b, e, n);
    for (i = 0; i < n; i++) {
        size_t k;
        int fixed = 0;
        if (b[i] == m[i])
            continue;
        for (k = i >= 3 ? i - 3 : 0; k <= i && k + 4 <= n && !fixed; k++)
            fixed = fix_field(b, k, us_at, at, m);
        if (!fixed)
            return 0;
    }
    return memcmp(m, b, n) == 0;
}
