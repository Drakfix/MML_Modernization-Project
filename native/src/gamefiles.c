/* gamefiles.h: the portable part. The exe patch is the installer's (src/patch_manifest.py, made
 * by tools/make_patcher.py): the build turns it into patch_data.h (android/build_android.py). A
 * vanilla exe, or one patched by an earlier release, gets the byte runs written over its first
 * EXE_SIZE bytes and the tail appended; the result must hash to PATCHED_SHA256. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamefiles.h"
#include "log.h"

#ifdef MML_PATCH_DATA
#include "patch_data.h"   /* (generated) PD_* */
#endif

/* ---- SHA-256 (FIPS 180-4) ---- */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t nbuf;
} sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void sha_init(sha256 *s)
{
    static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, H0, sizeof H0);
    s->len = 0;
    s->nbuf = 0;
}

static void sha_update(sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    while (n) {
        size_t k = 64 - s->nbuf < n ? 64 - s->nbuf : n;
        memcpy(s->buf + s->nbuf, p, k);
        s->nbuf += k, p += k, n -= k;
        if (s->nbuf == 64) {
            sha_block(s, s->buf);
            s->nbuf = 0;
        }
    }
}

static void sha_hex(sha256 *s, char out[65])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, lenb[8];
    int i;
    sha_update(s, &pad, 1);
    while (s->nbuf != 56)
        sha_update(s, &z, 1);
    for (i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lenb, 8);
    for (i = 0; i < 8; i++)
        snprintf(out + 8 * i, 9, "%08x", (unsigned)s->h[i]);
}

static void hash_of(const uint8_t *p, size_t n, char out[65])
{
    sha256 s;
    sha_init(&s);
    sha_update(&s, p, n);
    sha_hex(&s, out);
}

/* ---- files ---- */

static uint8_t *read_all(const char *path, size_t *n)
{
    FILE *f = NULL;
    uint8_t *p = NULL;
    long len;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && len < 64 << 20 && (p = (uint8_t *)malloc((size_t)len + 4096)) != NULL &&
        fread(p, 1, (size_t)len, f) != (size_t)len) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *n = p ? (size_t)len : 0;
    return p;
}

static int write_all(const char *path, const uint8_t *p, size_t n)
{
    FILE *f = NULL;
    int ok;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return 0;
    ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static int exists(const char *dir, const char *name)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", dir, name);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

int gamefiles_ready(const char *dir)
{
    return exists(dir, "MEGAMAN_NATIVE.EXE") && exists(dir, "DAT") && exists(dir, "BGM") && exists(dir, "SPH");
}

int gamefiles_finish(const char *dir, char *msg, size_t n)
{
#ifdef MML_PATCH_DATA
    char path[MAX_PATH], hex[65];
    size_t len, i;
    uint8_t *exe;
    int known;
    msg[0] = 0;
    if (!exists(dir, "DAT") || !exists(dir, "BGM") || !exists(dir, "SPH")) {
        _snprintf_s(msg, n, _TRUNCATE, "The DAT, BGM or SPH folder is missing: pick the folder with MEGAMAN.EXE.");
        return 0;
    }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\MEGAMAN.EXE", dir);
    if (!(exe = read_all(path, &len))) {
        _snprintf_s(msg, n, _TRUNCATE, "MEGAMAN.EXE wasn't found in that folder.");
        return 0;
    }
    hash_of(exe, len, hex);
    if (strcmp(hex, PD_PATCHED_SHA256) == 0) {
        known = 2;   /* patched already (our installer) */
    } else {
        known = strcmp(hex, PD_VANILLA_SHA256) == 0;
        for (i = 0; !known && i < sizeof PD_PREVIOUS / sizeof PD_PREVIOUS[0]; i++)
            known = strcmp(hex, PD_PREVIOUS[i]) == 0;
    }
    if (!known || len < PD_EXE_SIZE) {
        _snprintf_s(msg, n, _TRUNCATE, "This MEGAMAN.EXE is a version the patch doesn't know (the original Windows PC "
                    "release is the one supported).");
        log_printf("gamefiles: unknown MEGAMAN.EXE (%u bytes, sha256 %s)", (unsigned)len, hex);
        free(exe);
        return 0;
    }
    if (known == 1) {   /* the runs over the original bytes, then the tail */
        uint8_t *out = (uint8_t *)malloc(PD_PATCHED_SIZE);
        if (!out) {
            free(exe);
            _snprintf_s(msg, n, _TRUNCATE, "Out of memory.");
            return 0;
        }
        memcpy(out, exe, PD_EXE_SIZE);
        for (i = 0; i < PD_NRUNS; i++)
            memcpy(out + PD_RUNS[i].off, PD_BYTES + PD_RUNS[i].at, PD_RUNS[i].len);
        memcpy(out + PD_EXE_SIZE, PD_TAIL, PD_PATCHED_SIZE - PD_EXE_SIZE);
        free(exe);
        exe = out;
        len = PD_PATCHED_SIZE;
        hash_of(exe, len, hex);
        if (strcmp(hex, PD_PATCHED_SHA256) != 0) {
            _snprintf_s(msg, n, _TRUNCATE, "The patch didn't give the expected result (see mml.log).");
            log_printf("gamefiles: patched exe hashes to %s", hex);
            free(exe);
            return 0;
        }
    }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\MEGAMAN_NATIVE.EXE", dir);
    if (!write_all(path, exe, len)) {
        _snprintf_s(msg, n, _TRUNCATE, "MEGAMAN_NATIVE.EXE couldn't be written (storage full?).");
        free(exe);
        return 0;
    }
    free(exe);
    log_printf("gamefiles: MEGAMAN.EXE %s, MEGAMAN_NATIVE.EXE written", known == 2 ? "already patched" : "patched");
    gamefiles_defaults(dir);
    return 1;
#else
    (void)dir;
    _snprintf_s(msg, n, _TRUNCATE, "No patch data in this build.");
    return 0;
#endif
}

void gamefiles_defaults(const char *dir)
{
    if (!exists(dir, "mml.ini") && gamefiles_bundled_file("mml.ini", dir))
        log_printf("gamefiles: the default mml.ini put in place");
    if (!gamefiles_ready(dir))
        return;
    gamefiles_bundled_intro(dir);
    /* The game won't start without CAPCOM.AVI next to it: without one it asks for its CD. Our intro
       plays instead of it, so where a copy came without the file (a repack without videos) an
       empty one stands in. */
    if (!exists(dir, "CAPCOM.AVI")) {
        char p[MAX_PATH];
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\CAPCOM.AVI", dir);
        if (write_all(p, (const uint8_t *)"", 0))
            log_printf("gamefiles: no CAPCOM.AVI: an empty one stands in (the game checks it's there)");
    }
}

#ifndef __ANDROID__
/* (no importer here yet: the installer sets the folder up) */
int gamefiles_bundled_file(const char *name, const char *dir)
{
    (void)name;
    (void)dir;
    return 0;
}

int gamefiles_can_import(void)
{
    return 0;
}

void gamefiles_import_begin(const char *dir)
{
    (void)dir;
}

int gamefiles_import_poll(char *status, size_t n, float *progress)
{
    if (n)
        status[0] = 0;
    *progress = 0;
    return IMPORT_IDLE;
}

int gamefiles_bundled_intro(const char *dir)
{
    return exists(dir, "Modernization\\intro.mpg");
}
#endif
