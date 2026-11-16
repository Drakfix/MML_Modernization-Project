/* The Japanese names in the US version ([general] names=japanese): the names the original Rockman
 * DASH uses, in the US English script. Mega Man is Rock (Rockman in the titles of Trigger and Juno),
 * the Mega Buster the Rock Buster, the Servbots the Kobun, Diggers Digouters, Reaverbots Reaverbirds
 * and refractors deflectors. The Mother Lode (the Great Legacy) is only in the cutscene voices, so
 * only in the subtitles.
 *
 * The dialogue is in the stage files (DAT\*.BIN, DAJ\*.BIN): u32 offsets of the entries (the first
 * is the table's size, the last the file's), each entry an 8-byte header ("BD", its load region, the
 * region its pointers are relative to, the size of its pointer table) and data. Dialogue entries load
 * into regions 0x1F (a stage's, 0x8000 bytes up to the next region), 0x20 (the support car's, the
 * same) and 0x21 (the pause screens'), and have no pointers: u16 offsets of the blocks, then the
 * blocks. Text is in the game's font codes (A-Z 0x15.., a-z 0x2F.., space 0x0A, newline 0x82);
 * control codes refer to blocks by number, never by position. So when the game opens a stage file,
 * shim_kernel32.c hands it this copy: each name swapped as a whole word, each dialogue entry's
 * blocks laid out again with new offsets, the file's offsets after it. Every swapped line was
 * measured against the game's glyph widths: none is wider than the widest US line of its window.
 *
 * The program's own labels (the Buster parts screen, two refractor messages) are patched in place
 * when the swap is installed: "Rock" is three letters shorter than "MegaMan", and the three bytes
 * left become a pad of 0 pixels (code 0x9C, its 2-byte argument), as the labels already use. */
#include <windows.h>   /* (_stricmp; compat/windows.h elsewhere) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "addr.h"
#include "guest.h"
#include "log.h"
#include "names.h"

#define SP 0x0A        /* a space, in the font's codes */
#define NL 0x82        /* a new line */
#define PAD 0x9C       /* a pad: 2 bytes of pixels */
#define STAGE_ROOM 0x8000u   /* regions 0x1F and 0x20: the bytes up to the next region */

typedef struct {
    const char *us, *jp;
} rule;

/* the longer names first (the ones with a word after them) */
static const rule RULES[] = {
    { "MegaMan Trigger", "Rockman Trigger" },
    { "MegaMan Juno", "Rockman Juno" },
    { "MegaMan Buster", "Rock Buster" },
    { "Mega Man Trigger", "Rockman Trigger" },
    { "Mega Man Juno", "Rockman Juno" },
    { "Mega Man", "Rock" },
    { "MegaMan", "Rock" },
    { "Mother Lode", "Great Legacy" },
    { "Servbots", "Kobun" },
    { "Servbot", "Kobun" },
    { "Refractor", "Deflector" },
    { "refractor", "deflector" },
    { "Digger", "Digouter" },
    { "digger", "digouter" },
    { "Reaverbot", "Reaverbird" },
    { "reaverbot", "reaverbird" },
};
#define NRULES (int)(sizeof RULES / sizeof RULES[0])

static int g_on;

int names_active(void)
{
    return g_on;
}

/* ---- matching, in the font's codes or in ASCII ---- */

typedef struct {
    int font;   /* 1: the game's font codes; 0: ASCII */
} alphabet;

static int code_of(char c, int font)
{
    if (!font)
        return (unsigned char)c;
    if (c >= 'A' && c <= 'Z')
        return 0x15 + (c - 'A');
    if (c >= 'a' && c <= 'z')
        return 0x2F + (c - 'a');
    return SP;   /* (the rules hold letters and spaces only) */
}

static int is_letter(int b, int font)
{
    if (font)
        return b >= 0x15 && b <= 0x48;
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z');
}

static int is_sep(int b, int font)
{
    return font ? b == SP || b == NL : b == ' ';
}

/* the length of rule r's US words at b[i] (a whole word, or with a plural / possessive s after
   it), else 0 */
static size_t match(const uint8_t *b, size_t n, size_t i, const rule *r, int font)
{
    size_t k, len = strlen(r->us);
    int s = code_of('s', font);
    if (i + len > n || (i && is_letter(b[i - 1], font)))
        return 0;
    for (k = 0; k < len; k++) {
        if (r->us[k] == ' ') {
            if (!is_sep(b[i + k], font))
                return 0;
        } else if (b[i + k] != code_of(r->us[k], font))
            return 0;
    }
    if (i + len < n && is_letter(b[i + len], font)) {   /* "Diggers", "Reaverbot's" */
        if (b[i + len] != s || r->us[len - 1] == 's' || (i + len + 1 < n && is_letter(b[i + len + 1], font)))
            return 0;
    }
    return len;
}

/* rule r's Japanese words at out (with the separator the US words had); returns the length */
static size_t put(uint8_t *out, const uint8_t *us_at, const rule *r, int font)
{
    size_t k, len = strlen(r->jp);
    const char *usp = strchr(r->us, ' ');
    int sep = usp ? us_at[usp - r->us] : code_of(' ', font);
    for (k = 0; k < len; k++)
        out[k] = (uint8_t)(r->jp[k] == ' ' ? sep : code_of(r->jp[k], font));
    return len;
}

/* b[0..n) with the names swapped into out (room for n * 2); returns the new length and adds the
   swaps made to *count */
static size_t swap(const uint8_t *b, size_t n, uint8_t *out, int font, int *count)
{
    size_t i = 0, o = 0;
    while (i < n) {
        int r;
        size_t len = 0;
        for (r = 0; r < NRULES; r++)
            if ((len = match(b, n, i, &RULES[r], font)) != 0)
                break;
        if (len) {
            o += put(out + o, b + i, &RULES[r], font);
            i += len;
            (*count)++;
        } else
            out[o++] = b[i++];
    }
    return o;
}

/* ---- the stage files ---- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static unsigned rd16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* a dialogue entry e[0..n) laid out again into out (room for n * 2 + 8); returns its new length,
   or 0 to keep it as it is */
static size_t entry(const uint8_t *e, size_t n, uint8_t *out, int *swaps, const char *name)
{
    const uint8_t *body = e + 8;
    size_t len = n - 8, nb, k, o, room;
    uint8_t *ob = out + 8;
    int count = 0;
    if (n < 10 || e[0] != 'B' || e[1] != 'D' || e[2] < 0x1F || e[2] > 0x21 || rd32(e + 4) != 0)
        return 0;
    nb = rd16(body) / 2;
    if (!nb || rd16(body) & 1 || nb * 2 > len)
        return 0;
    for (k = 0; k < nb; k++)   /* (in order, inside the entry) */
        if (rd16(body + 2 * k) < nb * 2 || rd16(body + 2 * k) > len ||
            (k && rd16(body + 2 * k) < rd16(body + 2 * k - 2)))
            return 0;
    o = nb * 2;
    for (k = 0; k < nb; k++) {
        size_t from = rd16(body + 2 * k), to = k + 1 < nb ? rd16(body + 2 * k + 2) : len;
        ob[2 * k] = (uint8_t)o;
        ob[2 * k + 1] = (uint8_t)(o >> 8);
        o += swap(body + from, to - from, ob + o, 1, &count);
    }
    if (!count)
        return 0;
    room = e[2] == 0x21 ? len : STAGE_ROOM;   /* (the pause screens' room isn't known: no bigger) */
    if (o > room || o > 0xFFFF) {
        log_printf("names: %s: an entry would be %u bytes, more than its %u: left as it is", name,
                   (unsigned)o, (unsigned)room);
        return 0;
    }
    memcpy(out, e, 8);
    *swaps += count;
    return o + 8;
}

static int stage_file(const char *path)
{
    const char *base = path + strlen(path);
    size_t n = strlen(path);
    while (base > path && base[-1] != '\\' && base[-1] != '/')
        base--;
    if (base - path < 4 || n < 4 || _stricmp(path + n - 4, ".BIN") != 0)
        return 0;
    return _strnicmp(base - 4, "DAT", 3) == 0 || _strnicmp(base - 4, "DAJ", 3) == 0;
}

uint8_t *names_file(const char *path, const uint8_t *d, size_t size, size_t *out_size)
{
    uint32_t count, k;
    uint8_t *out, *tmp;
    size_t o, cap;
    int swaps = 0, changed = 0;
    if (!g_on || !stage_file(path) || size < 8)
        return NULL;
    count = rd32(d) / 4;
    if (count < 2 || (size_t)count * 4 > size || rd32(d + 4 * (count - 1)) != size)
        return NULL;
    for (k = 0; k + 1 < count; k++)
        if (rd32(d + 4 * k) > rd32(d + 4 * k + 4))
            return NULL;
    cap = size * 2 + 64;
    out = (uint8_t *)malloc(cap);
    tmp = (uint8_t *)malloc(cap);
    if (!out || !tmp) {
        free(out);
        free(tmp);
        return NULL;
    }
    o = (size_t)count * 4;
    for (k = 0; k + 1 < count; k++) {
        uint32_t from = rd32(d + 4 * k), to = rd32(d + 4 * k + 4);
        size_t len = entry(d + from, to - from, tmp, &swaps, path);
        wr32(out + 4 * k, (uint32_t)o);
        if (len) {
            memcpy(out + o, tmp, len);
            o += len;
            changed = 1;
        } else {
            memcpy(out + o, d + from, to - from);
            o += to - from;
        }
    }
    wr32(out + 4 * (count - 1), (uint32_t)o);
    free(tmp);
    if (!changed) {
        free(out);
        return NULL;
    }
    log_printf("names: %s: %d names (%u -> %u bytes)", path, swaps, (unsigned)size, (unsigned)o);
    *out_size = o;
    return out;
}

void names_ascii(const char *in, char *out, size_t n)
{
    size_t len = strlen(in), got;
    int count = 0;
    uint8_t *buf = (uint8_t *)malloc(len * 2 + 1);
    if (!buf) {
        snprintf(out, n, "%s", in);
        return;
    }
    got = swap((const uint8_t *)in, len, buf, 0, &count);
    buf[got] = 0;
    snprintf(out, n, "%s", (const char *)buf);
    free(buf);
}

/* ---- the program's labels (US program) ---- */

typedef struct {
    uint32_t va;
    const char *us, *jp;   /* "Rock" + a 0-pixel pad where it is three letters shorter */
} label;

static const label LABELS[] = {
    { 0x52F4A7u, "MegaMan", "Rock" },     /* the Buster parts screen: "MegaMan Buster" */
    { 0x52FA1Au, "MegaMan", "Rock" },     /* "Equip MegaMan Buster parts" */
    { 0x52EFE9u, "Refractor", "Deflector" },
    { 0x52EFF3u, "Refractor", "Deflector" },
    { 0x52EFFDu, "Refractor", "Deflector" },
    { 0x52F65Cu, "refractor", "deflector" },   /* "There's a refractor shard inside" */
    { 0x52F773u, "refractor", "deflector" },
};

static int patch_labels(void)
{
    size_t i, k;
    int n = 0;
    for (i = 0; i < sizeof LABELS / sizeof LABELS[0]; i++) {
        const label *l = &LABELS[i];
        size_t ul = strlen(l->us), jl = strlen(l->jp);
        int ok = (ul - jl) % 3 == 0;
        for (k = 0; k < ul && ok; k++)
            ok = GVAR(uint8_t, l->va + k) == code_of(l->us[k], 1);
        if (!ok)
            continue;
        for (k = 0; k < jl; k++)
            GVAR(uint8_t, l->va + k) = (uint8_t)code_of(l->jp[k], 1);
        for (; k < ul; k += 3) {   /* a pad of 0 pixels */
            GVAR(uint8_t, l->va + k) = PAD;
            GVAR(uint8_t, l->va + k + 1) = 0;
            GVAR(uint8_t, l->va + k + 2) = 0;
        }
        n++;
    }
    return n;
}

void names_install(int on)
{
    g_on = on && addr_game == GAME_US;
    if (g_on)
        log_printf("names: the Japanese names (Rock, Kobun, Digouter, Reaverbird, Deflector); %d of %d program "
                   "labels", patch_labels(), (int)(sizeof LABELS / sizeof LABELS[0]));
}
