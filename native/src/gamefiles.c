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

void gamefiles_sha256(const void *data, size_t n, char hex[65])
{
    hash_of((const uint8_t *)data, n, hex);
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
    return (exists(dir, "MEGAMAN_NATIVE.EXE") || exists(dir, "ROCKMAN_NATIVE.EXE")) && exists(dir, "DAT") &&
           exists(dir, "BGM") && exists(dir, "SPH");
}

int gamefiles_version_ready(const char *root, int jp)
{
    char dir[MAX_PATH];
    int exe;
    gamefiles_dir(root, jp, dir, sizeof dir);
    exe = exists(dir, jp ? "ROCKMAN_NATIVE.EXE" : "MEGAMAN_NATIVE.EXE");
#ifdef _WIN32
    /* (a folder Setup patched in place before the start menu imported games: its own exe) */
    exe = exe || exists(dir, jp ? "ROCKMAN.EXE" : "MEGAMAN.EXE");
#endif
    return exe && exists(dir, "DAT") && exists(dir, "BGM") && exists(dir, "SPH");
}

void gamefiles_dir(const char *root, int jp, char *out, size_t n)
{
    /* the Japanese version in a folder of its own, unless the app's folder is a Rockman DASH that
       Setup patched in place (Windows, before the start menu imported games) */
    if (jp && gamefiles_can_import() && !exists(root, "ROCKMAN.EXE") && !exists(root, "ROCKMAN_NATIVE.EXE"))
        _snprintf_s(out, n, _TRUNCATE, "%s\\%s", root, GAMEFILES_JP_DIR);
    else
        _snprintf_s(out, n, _TRUNCATE, "%s", root);
}

int gamefiles_edition(const char *root)
{
    char ini[MAX_PATH], v[32];
    int us_ok = gamefiles_version_ready(root, 0), jp_ok;
    if (!gamefiles_can_import())
        return 0;
    jp_ok = gamefiles_version_ready(root, 1);
    _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s\\mml.ini", root);
    GetPrivateProfileStringA("general", "edition", "", v, sizeof v, ini);
    if (_stricmp(v, "japanese") == 0 && (jp_ok || !us_ok))
        return 1;
    if (_stricmp(v, "us") == 0 && (us_ok || !jp_ok))
        return 0;
    return !us_ok && jp_ok;   /* (not chosen yet: the one there is) */
}

int gamefiles_finish(const char *dir, char *msg, size_t n)
{
#ifdef MML_PATCH_DATA
    char path[MAX_PATH], hex[65];
    size_t len = 0, i;
    uint8_t *exe = NULL;
    const pd_edition *ed = NULL;
    int known, k;
    msg[0] = 0;
    if (!exists(dir, "DAT") || !exists(dir, "BGM") || !exists(dir, "SPH")) {
        _snprintf_s(msg, n, _TRUNCATE, "The DAT, BGM or SPH folder is missing: pick the game's own folder.");
        return 0;
    }
    for (k = 0; k < PD_NEDITIONS && !exe; k++) {   /* the US game, or the Japanese one (Rockman DASH) */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, PD_EDITIONS[k].exe);
        if ((exe = read_all(path, &len)) != NULL)
            ed = &PD_EDITIONS[k];
    }
    if (!exe) {
        _snprintf_s(msg, n, _TRUNCATE, PD_NEDITIONS > 1 ? "MEGAMAN.EXE (or ROCKMAN.EXE) wasn't found in that folder."
                                                         : "MEGAMAN.EXE wasn't found in that folder.");
        return 0;
    }
    hash_of(exe, len, hex);
    if (strcmp(hex, ed->patched) == 0) {
        known = 2;   /* patched already (our installer) */
    } else {
        known = strcmp(hex, ed->vanilla) == 0;
        for (i = 0; !known && i < (size_t)ed->nprevious; i++)
            known = strcmp(hex, ed->previous[i]) == 0;
    }
    if (!known || len < ed->exe_size) {
        _snprintf_s(msg, n, _TRUNCATE, "This %s is a version the patch doesn't know (the original Windows PC "
                    "release is the one supported).", ed->exe);
        log_printf("gamefiles: unknown %s (%u bytes, sha256 %s)", ed->exe, (unsigned)len, hex);
        free(exe);
        return 0;
    }
    if (known == 1) {   /* the runs over the original bytes, then the tail */
        uint8_t *out = (uint8_t *)malloc(ed->patched_size);
        if (!out) {
            free(exe);
            _snprintf_s(msg, n, _TRUNCATE, "Out of memory.");
            return 0;
        }
        memcpy(out, exe, ed->exe_size);
        for (i = 0; i < (size_t)ed->nruns; i++)
            memcpy(out + ed->runs[i].off, ed->bytes + ed->runs[i].at, ed->runs[i].len);
        memcpy(out + ed->exe_size, ed->tail, ed->patched_size - ed->exe_size);
        free(exe);
        exe = out;
        len = ed->patched_size;
        hash_of(exe, len, hex);
        if (strcmp(hex, ed->patched) != 0) {
            _snprintf_s(msg, n, _TRUNCATE, "The patch didn't give the expected result (see mml.log).");
            log_printf("gamefiles: patched exe hashes to %s", hex);
            free(exe);
            return 0;
        }
    }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, ed->native);
    if (!write_all(path, exe, len)) {
        _snprintf_s(msg, n, _TRUNCATE, "%s couldn't be written (storage full?).", ed->native);
        free(exe);
        return 0;
    }
    free(exe);
    log_printf("gamefiles: %s %s, %s written", ed->exe, known == 2 ? "already patched" : "patched", ed->native);
    return 1;
#else
    (void)dir;
    _snprintf_s(msg, n, _TRUNCATE, "No patch data in this build.");
    return 0;
#endif
}

/* CAPCOM.AVI next to a version's program: without one the game asks for its CD. Our intro plays
   instead of it, so where a copy came without the file (a repack without videos) an empty one
   stands in. */
static void capcom_avi(const char *dir)
{
    char p[MAX_PATH];
    if (!gamefiles_ready(dir) || exists(dir, "CAPCOM.AVI"))
        return;
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\CAPCOM.AVI", dir);
    if (write_all(p, (const uint8_t *)"", 0))
        log_printf("gamefiles: no CAPCOM.AVI in %s: an empty one stands in (the game checks it's there)", dir);
}

void gamefiles_defaults(const char *dir)
{
    char jp[MAX_PATH];
    if (!exists(dir, "mml.ini") && gamefiles_bundled_file("mml.ini", dir))
        log_printf("gamefiles: the default mml.ini put in place");
    gamefiles_dir(dir, 1, jp, sizeof jp);
    if (!gamefiles_version_ready(dir, 0) && !(gamefiles_can_import() && gamefiles_version_ready(dir, 1)))
        return;
    gamefiles_bundled_intro(dir);   /* (shared by both versions) */
    capcom_avi(dir);
    if (gamefiles_can_import())
        capcom_avi(jp);
}

#if !defined(__linux__) && !(defined(_WIN32) && defined(MML_RUNNER))
/* (no importer here: mml.dll in the game Setup set up; Android: android_files.c, Linux:
   linux_files.c (Android defines __linux__ too), the Windows start menu: win_files.c) */
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

void gamefiles_import_begin(const char *dir, int jp)
{
    (void)dir;
    (void)jp;
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

void gamefiles_subtitles_begin(const char *dir)
{
    (void)dir;
}

void gamefiles_voices_begin(const char *us_dir, const char *jp_dir)
{
    (void)us_dir;
    (void)jp_dir;
}

int gamefiles_task_poll(char *status, size_t n, float *progress)
{
    if (n)
        status[0] = 0;
    *progress = 0;
    return TASK_IDLE;
}
#endif
