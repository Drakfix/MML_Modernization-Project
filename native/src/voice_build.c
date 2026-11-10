/* The Japanese voices for the US version, built on the device (see voice_build.h): the same
 * output as tools/voice/build_jp_voices.py, byte for byte.
 *
 * Voice banks (SPH\*.WBK) are a table of uint32 offsets (the first one is the table's size, the
 * last one the file size) followed by RIFF WAV clips. For each in-game US bank, SPJ gets a bank
 * with the same name and clip count: the US clip where no script plays it (VM_KEEP), an empty
 * entry (0 clips), or the Japanese clip(s) of the line as one 8-bit WAV (joined with a short
 * silence when the US dub split the line). The two cutscene tracks are the Japanese ones as they
 * are. DAJ gets the few DAT files whose sound banks ("WB" chunks) differ: the US file with the
 * Japanese file's sound banks. */
#include <windows.h>   /* (native/compat elsewhere) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif

#include "log.h"
#include "voice_build.h"

#define GAP_BYTES 2646   /* 0.12 s of silence at 22050 Hz, 8-bit (build_jp_voices.py GAP) */
#define MAX_BANK_CLIPS 1024
#define MAX_JOIN 32          /* Japanese clips joined into one (10 at most) */

typedef struct {
    FILE *f;
    uint32_t n;                          /* clips */
    uint32_t offs[MAX_BANK_CLIPS + 1];
} bank;

typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf;

static int put(buf *b, const void *d, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1u << 16;
        uint8_t *q;
        while (cap < b->len + n)
            cap *= 2;
        if (!(q = (uint8_t *)realloc(b->p, cap)))
            return 0;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 1;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static long file_size(FILE *f)
{
    long n;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    return n;
}

static int bank_open(bank *b, const char *dir, const char *name)
{
    char path[MAX_PATH];
    uint32_t first;
    long size;
    snprintf(path, sizeof path, "%s\\SPH\\%s", dir, name);
    b->n = 0;
    if (fopen_s(&b->f, path, "rb") != 0 || !b->f)
        return 0;
    size = file_size(b->f);
    if (fread(&first, 4, 1, b->f) == 1 && first >= 8 && first % 4 == 0 && first / 4 <= MAX_BANK_CLIPS + 1) {
        b->offs[0] = first;
        if (fread(b->offs + 1, 4, first / 4 - 1, b->f) == first / 4 - 1 && b->offs[first / 4 - 1] == (uint32_t)size)
            b->n = first / 4 - 1;
    }
    if (!b->n) {
        fclose(b->f);
        b->f = NULL;
    }
    return b->n > 0;
}

static void bank_close(bank *b)
{
    if (b->f)
        fclose(b->f);
    b->f = NULL;
}

/* clip i of the bank into a new allocation (*len its size); NULL on a read error */
static uint8_t *clip_read(bank *b, uint32_t i, size_t *len)
{
    uint8_t *p;
    *len = b->offs[i + 1] - b->offs[i];
    if (b->offs[i + 1] < b->offs[i] || !(p = (uint8_t *)malloc(*len ? *len : 1)))
        return NULL;
    if (fseek(b->f, (long)b->offs[i], SEEK_SET) != 0 || fread(p, 1, *len, b->f) != *len) {
        free(p);
        return NULL;
    }
    return p;
}

typedef struct {
    unsigned ch, rate, bits;
    const uint8_t *pcm;
    size_t pcm_len;
} wav;

static int wav_parse(const uint8_t *w, size_t len, wav *out)
{
    uint64_t p = 12;
    int fmt = 0;
    if (len < 12 || memcmp(w, "RIFF", 4) != 0 || memcmp(w + 8, "WAVE", 4) != 0)
        return 0;
    while (p + 8 <= len) {
        uint32_t size = rd32(w + p + 4);
        if (memcmp(w + p, "fmt ", 4) == 0 && p + 8 + 16 <= len) {
            out->ch = w[p + 10] | w[p + 11] << 8;
            out->rate = rd32(w + p + 12);
            out->bits = w[p + 22] | w[p + 23] << 8;
            fmt = 1;
        } else if (memcmp(w + p, "data", 4) == 0) {
            out->pcm = w + p + 8;
            out->pcm_len = size <= len - (p + 8) ? size : (size_t)(len - (p + 8));
            return fmt;
        }
        p += 8 + (uint64_t)size + (size & 1);
    }
    return 0;
}

static int wav_header(buf *b, unsigned ch, unsigned rate, unsigned bits, size_t pcm_len)
{
    uint8_t h[44];
    unsigned align = ch * bits / 8;
    memcpy(h, "RIFF", 4);
    wr32(h + 4, (uint32_t)(4 + 8 + 16 + 8 + pcm_len));
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16);
    h[20] = 1, h[21] = 0;
    h[22] = (uint8_t)ch, h[23] = 0;
    wr32(h + 24, rate);
    wr32(h + 28, rate * align);
    h[32] = (uint8_t)align, h[33] = 0;
    h[34] = (uint8_t)bits, h[35] = 0;
    memcpy(h + 36, "data", 4);
    wr32(h + 40, (uint32_t)pcm_len);
    return put(b, h, sizeof h);
}

/* a bank file from its clips (all in one buffer, ends[i] where clip i ends) */
static int bank_write(const char *path, const buf *clips, const size_t *ends, uint32_t n)
{
    FILE *f;
    uint32_t i, table = 4 * (n + 1);
    int ok;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return 0;
    ok = 1;
    for (i = 0; i <= n && ok; i++) {
        uint8_t o[4];
        wr32(o, table + (uint32_t)(i ? ends[i - 1] : 0));
        ok = fwrite(o, 4, 1, f) == 1;
    }
    ok = ok && (!clips->len || fwrite(clips->p, 1, clips->len, f) == clips->len);
    return fclose(f) == 0 && ok;
}

static int copy_file(const char *from, const char *to, volatile float *progress, double *done, double total)
{
    FILE *a, *b;
    static uint8_t chunk[1 << 16];
    size_t k;
    int ok = 1;
    if (fopen_s(&a, from, "rb") != 0 || !a)
        return 0;
    if (fopen_s(&b, to, "wb") != 0 || !b) {
        fclose(a);
        return 0;
    }
    while (ok && (k = fread(chunk, 1, sizeof chunk, a)) > 0) {
        ok = fwrite(chunk, 1, k, b) == k;
        *done += (double)k;
        if (progress)
            *progress = (float)(*done / total);
    }
    ok = ok && !ferror(a);
    fclose(a);
    return fclose(b) == 0 && ok;
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f;
    uint8_t *p;
    long n;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return NULL;
    n = file_size(f);
    p = n >= 0 ? (uint8_t *)malloc((size_t)n + 1) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* a DAT file's chunks: offsets into the file, n+1 of them; 0 if it isn't one */
static uint32_t dat_chunks(const uint8_t *d, size_t len, const uint8_t **start, uint32_t *size, uint32_t max)
{
    uint32_t n, i;
    if (len < 8 || rd32(d) % 4 || rd32(d) / 4 < 2 || rd32(d) / 4 - 1 > max || rd32(d) > len)
        return 0;
    n = rd32(d) / 4 - 1;
    for (i = 0; i < n; i++) {
        uint32_t a = rd32(d + 4 * i), b = rd32(d + 4 * i + 4);
        if (b < a || b > len)
            return 0;
        start[i] = d + a;
        size[i] = b - a;
    }
    return n;
}

#define MAX_CHUNKS 4096
/* us_dir\DAJ\name: the US file with the Japanese file's sound banks, where they differ;
   -1 on an error, else 1 written / 0 not needed */
static int dat_with_jp_sounds(const char *us_dir, const char *jp_dir, const char *name)
{
    static const uint8_t *us_c[MAX_CHUNKS], *jp_c[MAX_CHUNKS], *jwb[MAX_CHUNKS];
    static uint32_t us_s[MAX_CHUNKS], jp_s[MAX_CHUNKS], jwb_s[MAX_CHUNKS];
    char path[MAX_PATH];
    uint8_t *u, *j;
    size_t ul, jl;
    uint32_t un, jn, i, nwb = 0, uwb = 0, k = 0;
    int same = 1, r = 0;
    snprintf(path, sizeof path, "%s\\DAT\\%s", us_dir, name);
    u = read_all(path, &ul);
    snprintf(path, sizeof path, "%s\\DAT\\%s", jp_dir, name);
    j = read_all(path, &jl);
    snprintf(path, sizeof path, "%s\\DAJ\\%s", us_dir, name);
    if (!u || !j) {
        free(u);
        free(j);
        return u ? 0 : -1;   /* (no Japanese file: nothing to do) */
    }
    un = dat_chunks(u, ul, us_c, us_s, MAX_CHUNKS);
    jn = dat_chunks(j, jl, jp_c, jp_s, MAX_CHUNKS);
    for (i = 0; i < jn; i++)
        if (jp_s[i] >= 2 && jp_c[i][0] == 'W' && jp_c[i][1] == 'B')
            jwb[nwb] = jp_c[i], jwb_s[nwb++] = jp_s[i];
    for (i = 0; i < un; i++)
        if (us_s[i] >= 2 && us_c[i][0] == 'W' && us_c[i][1] == 'B') {
            if (uwb < nwb && (us_s[i] != jwb_s[uwb] || memcmp(us_c[i], jwb[uwb], us_s[i]) != 0))
                same = 0;
            uwb++;
        }
    if (un && uwb == nwb && !same) {   /* the n-th sound bank of one is the n-th of the other */
        FILE *f;
        uint32_t pos = 4 * (un + 1);
        int ok;
        r = -1;
        if (fopen_s(&f, path, "wb") == 0 && f) {
            ok = 1;
            for (i = 0; i <= un && ok; i++) {   /* the offset table */
                uint8_t o[4];
                wr32(o, pos);
                ok = fwrite(o, 4, 1, f) == 1;
                if (i < un)
                    pos += us_s[i] >= 2 && us_c[i][0] == 'W' && us_c[i][1] == 'B' ? jwb_s[k++] : us_s[i];
            }
            for (i = 0, k = 0; i < un && ok; i++) {
                int wb = us_s[i] >= 2 && us_c[i][0] == 'W' && us_c[i][1] == 'B';
                const uint8_t *c = wb ? jwb[k] : us_c[i];
                uint32_t s = wb ? jwb_s[k++] : us_s[i];
                ok = fwrite(c, 1, s, f) == s;
            }
            if (fclose(f) == 0 && ok)
                r = 1;
        }
    } else
        DeleteFileA(path);   /* (from an earlier build) */
    free(u);
    free(j);
    return r;
}

static int ends_with_bin(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && _stricmp(s + n - 4, ".BIN") == 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int voice_built(const char *us_dir)
{
    char path[MAX_PATH];
    int i;
    for (i = 0; i < VM_US_BANKS; i++) {
        snprintf(path, sizeof path, "%s\\SPJ\\%s", us_dir, VM_US_BANK[i]);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            return 0;
    }
    return 1;
}

static void remove_spj(const char *us_dir)
{
    char path[MAX_PATH];
    int i;
    for (i = 0; i < VM_US_BANKS; i++) {
        snprintf(path, sizeof path, "%s\\SPJ\\%s", us_dir, VM_US_BANK[i]);
        DeleteFileA(path);
    }
}

static double free_bytes(const char *dir)
{
#ifdef _WIN32
    ULARGE_INTEGER a;
    return GetDiskFreeSpaceExA(dir, &a, NULL, NULL) ? (double)a.QuadPart : 1e18;
#else
    char real[MAX_PATH];
    struct statvfs v;
    return statvfs(compat_path(dir, real, sizeof real), &v) == 0 ? (double)v.f_bavail * (double)v.f_frsize : 1e18;
#endif
}

int voice_build_jp(const char *us_dir, const char *jp_dir, char *msg, size_t n, volatile float *progress)
{
    static bank us, jp[VM_JP_BANKS];
    char path[MAX_PATH], to[MAX_PATH];
    double total = 0, done = 0;
    int b, k, clip_at = 0, ok = 0;
    buf out = { 0 };
    size_t *ends = NULL;
    if (progress)
        *progress = 0;
    snprintf(msg, n, "%s", "");
    for (b = 0; b < VM_JP_BANKS; b++) {
        if (!bank_open(&jp[b], jp_dir, VM_JP_BANK[b]) || jp[b].n != VM_JP_CLIPS[b]) {
            snprintf(msg, n, "The Japanese version's SPH\\%s isn't the one these voices were made for",
                     VM_JP_BANK[b]);
            goto end;
        }
        total += (double)jp[b].offs[jp[b].n];
    }
    if (free_bytes(us_dir) < total + 16e6) {
        snprintf(msg, n, "Not enough free space: %.0f MB needed, %.0f MB free", (total + 16e6) / 1e6,
                 free_bytes(us_dir) / 1e6);
        goto end;
    }
    snprintf(path, sizeof path, "%s\\SPJ", us_dir);
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof path, "%s\\DAJ", us_dir);
    CreateDirectoryA(path, NULL);
    ends = (size_t *)malloc(sizeof *ends * MAX_BANK_CLIPS);
    if (!ends)
        goto end;

    /* the in-game banks */
    for (b = 0; b < VM_US_BANKS - 2; b++) {
        uint32_t c;
        if (!bank_open(&us, us_dir, VM_US_BANK[b]) || us.n != VM_US_CLIPS[b]) {
            snprintf(msg, n, "The US version's SPH\\%s isn't the one these voices were made for", VM_US_BANK[b]);
            goto end;
        }
        out.len = 0;
        for (c = 0; c < us.n; c++, clip_at++) {
            unsigned cnt = VM_COUNT[clip_at];
            if (cnt == VM_KEEP) {   /* no script plays it: the US clip */
                size_t len;
                uint8_t *w = clip_read(&us, c, &len);
                int r = w && put(&out, w, len);
                free(w);
                if (!r)
                    goto read_error;
            } else if (cnt) {
                uint8_t *w[MAX_JOIN] = { 0 };
                wav p[MAX_JOIN];
                size_t len, pcm = 0;
                unsigned i, mono;
                int r = cnt <= MAX_JOIN;
                for (i = 0; r && i < cnt; i++) {
                    uint32_t id = VM_SRC[VM_FIRST[clip_at] + i];
                    r = (id >> 16) < VM_JP_BANKS && (id & 0xFFFF) < jp[id >> 16].n &&
                        (w[i] = clip_read(&jp[id >> 16], id & 0xFFFF, &len)) != NULL && wav_parse(w[i], len, &p[i]) &&
                        p[i].ch == p[0].ch && p[i].rate == p[0].rate && p[i].bits == 8;
                }
                if (r) {
                    mono = p[0].ch == 1;
                    for (i = 0; i < (mono ? cnt : 1); i++)
                        pcm += p[i].pcm_len + (i ? GAP_BYTES : 0);
                    r = wav_header(&out, p[0].ch, p[0].rate, 8, pcm);
                    for (i = 0; r && i < (mono ? cnt : 1); i++) {
                        static uint8_t silence[GAP_BYTES];
                        if (!silence[0])
                            memset(silence, 0x80, sizeof silence);
                        r = (!i || put(&out, silence, GAP_BYTES)) && put(&out, p[i].pcm, p[i].pcm_len);
                    }
                }
                for (i = 0; i < MAX_JOIN; i++)
                    free(w[i]);
                if (!r) {
                    snprintf(msg, n, "A Japanese clip for SPH\\%s isn't as expected", VM_US_BANK[b]);
                    goto end;
                }
            }
            ends[c] = out.len;
        }
        snprintf(path, sizeof path, "%s\\SPJ\\%s", us_dir, VM_US_BANK[b]);
        if (!bank_write(path, &out, ends, us.n))
            goto write_error;
        bank_close(&us);
        done += (double)out.len;
        if (progress)
            *progress = (float)(done / total);
    }

    /* the DAT files with voice samples in their sound banks */
    {
        static char names[512][64];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        int count = 0, i;
        snprintf(path, sizeof path, "%s\\DAT\\*", us_dir);
        h = FindFirstFileA(path, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do
                if (ends_with_bin(fd.cFileName) && strlen(fd.cFileName) < sizeof names[0] && count < 512)
                    snprintf(names[count++], sizeof names[0], "%s", fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        qsort(names, (size_t)count, sizeof names[0], by_name);
        for (i = 0; i < count; i++) {
            int r = dat_with_jp_sounds(us_dir, jp_dir, names[i]);
            if (r < 0)
                goto write_error;
            if (r)
                log_printf("voice build: DAJ\\%s (Japanese sound banks)", names[i]);
        }
    }

    /* the cutscene tracks: the Japanese ones as they are */
    for (k = 0; k < 2; k++) {
        snprintf(path, sizeof path, "%s\\SPH\\%s", jp_dir, VM_JP_BANK[VM_JP_BANKS - 2 + k]);
        snprintf(to, sizeof to, "%s\\SPJ\\%s", us_dir, VM_US_BANK[VM_US_BANKS - 2 + k]);
        if (!copy_file(path, to, progress, &done, total))
            goto write_error;
    }
    ok = 1;
    if (progress)
        *progress = 1;
    log_printf("voice build: SPJ and DAJ written in %s", us_dir);
    goto end;

read_error:
    snprintf(msg, n, "Couldn't read SPH\\%s", VM_US_BANK[b]);
    goto end;
write_error:
    snprintf(msg, n, "Couldn't write the voices (is the storage full?)");
end:
    bank_close(&us);
    for (b = 0; b < VM_JP_BANKS; b++)
        bank_close(&jp[b]);
    free(out.p);
    free(ends);
    if (!ok) {
        remove_spj(us_dir);
        log_printf("voice build: %s", msg);
    }
    return ok;
}
