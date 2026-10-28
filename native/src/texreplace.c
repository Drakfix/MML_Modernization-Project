/* HD texture replacement and dumping ([textures] replace=1 / dump=1).
 *
 * Every texture the game builds (a 256x256 page converted from its PS1-style
 * VRAM through a palette) is hashed when it reaches our renderer: zlib CRC32
 * of its size and RGBA pixels. With dump=1 each new one is written to
 * Modernization\textures\dump\<hash>.png; a PNG named <hash>.png anywhere
 * under Modernization\textures (outside dump) replaces it, at any size (2x,
 * 4x... texture coordinates are normalised, so nothing else changes).
 *
 * PNG reading and writing go through WIC, built into Windows. A portable
 * decoder replaces it for the Linux / Android ports; the rest is plain C.
 *
 * Note: Beetle PSX HW packs don't apply directly. Their names hash the PS1's
 * 4bpp texture uploads, while the PC port re-encoded most world textures as
 * 8bpp with a different layout (RE-notes 7.19). */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "texreplace.h"

typedef struct {
    uint32_t hash;
    char *path;
    uint32_t *pixels;   /* loaded on first use */
    int w, h, failed;
} entry;

static entry *g_entries;
static int g_count, g_cap;
static int g_replace, g_dump;
static char g_dump_dir[MAX_PATH];
static uint32_t *g_dumped;   /* hashes dumped this session (sorted insert) */
static int g_ndumped, g_cap_dumped;
static IWICImagingFactory *g_wic;
static uint32_t g_crc_table[256];
static unsigned g_replaced, g_written;

static void crc_init(void)
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c = ~c;
    while (n--)
        c = g_crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static int by_hash(const void *a, const void *b)
{
    uint32_t x = ((const entry *)a)->hash, y = ((const entry *)b)->hash;
    return x < y ? -1 : x > y;
}

static void add_file(const char *dir, const char *name)
{
    char *end;
    unsigned long h;
    size_t len = strlen(name);
    if (len < 12 || _stricmp(name + len - 4, ".png") != 0)
        return;
    h = strtoul(name, &end, 16);
    if (end != name + 8)
        return;   /* not <8 hex digits>... */
    if (g_count == g_cap) {
        int ncap = g_cap ? g_cap * 2 : 256;
        entry *n = (entry *)realloc(g_entries, sizeof *n * (size_t)ncap);
        if (!n)
            return;
        g_entries = n;
        g_cap = ncap;
    }
    memset(&g_entries[g_count], 0, sizeof g_entries[0]);
    g_entries[g_count].hash = (uint32_t)h;
    {
        size_t n = strlen(dir) + len + 2;
        g_entries[g_count].path = (char *)malloc(n);
        if (!g_entries[g_count].path)
            return;
        _snprintf_s(g_entries[g_count].path, n, _TRUNCATE, "%s\\%s", dir, name);
    }
    g_count++;
}

static void scan(const char *dir, int depth)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", dir);
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.cFileName[0] == '.')
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[MAX_PATH];
            size_t n = strlen(fd.cFileName);
            if (depth == 0 && _stricmp(fd.cFileName, "dump") == 0)
                continue;   /* dumps aren't replacements */
            if (fd.cFileName[0] == '_' || (n > 4 && _stricmp(fd.cFileName + n - 4, ".off") == 0))
                continue;   /* "_name" or "name.off": a pack switched off */
            if (depth < 8) {
                _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
                scan(sub, depth + 1);
            }
        } else {
            add_file(dir, fd.cFileName);
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static int wic(void)
{
    HRESULT hr;
    if (g_wic)
        return 1;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;   /* already initialised in another mode is fine for WIC */
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                          (void **)&g_wic);
    if (FAILED(hr)) {
        log_printf("textures: WIC unavailable (%08lX), PNGs off", (unsigned long)hr);
        g_wic = NULL;
        return 0;
    }
    return 1;
}

static void widen(const char *in, WCHAR *out, int n)
{
    if (!MultiByteToWideChar(CP_ACP, 0, in, -1, out, n))
        out[0] = 0;
}

static int load_png(entry *e)
{
    WCHAR path[MAX_PATH];
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *rgba = NULL;
    UINT w = 0, h = 0;
    int ok = 0;
    if (!wic())
        return 0;
    widen(e->path, path, MAX_PATH);
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(g_wic, path, NULL, GENERIC_READ,
                                                              WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppRGBA, (IWICBitmapSource *)frame, &rgba)) &&
        SUCCEEDED(IWICBitmapSource_GetSize(rgba, &w, &h)) && w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        e->pixels = (uint32_t *)malloc((size_t)w * h * 4);
        if (e->pixels && SUCCEEDED(IWICBitmapSource_CopyPixels(rgba, NULL, w * 4, w * h * 4, (BYTE *)e->pixels))) {
            e->w = (int)w;
            e->h = (int)h;
            ok = 1;
        } else {
            free(e->pixels);
            e->pixels = NULL;
        }
    }
    if (rgba)
        IWICBitmapSource_Release(rgba);
    if (frame)
        IWICBitmapFrameDecode_Release(frame);
    if (dec)
        IWICBitmapDecoder_Release(dec);
    if (!ok)
        log_printf("textures: couldn't read %s", e->path);
    return ok;
}

static int save_png(const char *file, const uint32_t *px, int w, int h)
{
    WCHAR path[MAX_PATH];
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    uint8_t *bgra;
    int i, ok = 0;
    if (!wic())
        return 0;
    bgra = (uint8_t *)malloc((size_t)w * h * 4);
    if (!bgra)
        return 0;
    for (i = 0; i < w * h; i++) {   /* our RGBA -> BGRA, which every WIC PNG encoder takes */
        uint32_t v = px[i];
        bgra[i * 4 + 0] = (uint8_t)(v >> 16);
        bgra[i * 4 + 1] = (uint8_t)(v >> 8);
        bgra[i * 4 + 2] = (uint8_t)v;
        bgra[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    widen(file, path, MAX_PATH);
    if (SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &st)) &&
        SUCCEEDED(IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(g_wic, &GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(frame, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(frame, (UINT)w, (UINT)h)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt)) &&
        IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, (UINT)h, (UINT)w * 4, (UINT)w * h * 4, bgra)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Commit(frame)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc)))
        ok = 1;
    if (frame)
        IWICBitmapFrameEncode_Release(frame);
    if (enc)
        IWICBitmapEncoder_Release(enc);
    if (st)
        IWICStream_Release(st);
    free(bgra);
    return ok;
}

static int dumped_before(uint32_t h)
{
    int lo = 0, hi = g_ndumped;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_dumped[mid] < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < g_ndumped && g_dumped[lo] == h)
        return 1;
    if (g_ndumped == g_cap_dumped) {
        int ncap = g_cap_dumped ? g_cap_dumped * 2 : 1024;
        uint32_t *n = (uint32_t *)realloc(g_dumped, sizeof *n * (size_t)ncap);
        if (!n)
            return 1;
        g_dumped = n;
        g_cap_dumped = ncap;
    }
    memmove(g_dumped + lo + 1, g_dumped + lo, sizeof *g_dumped * (size_t)(g_ndumped - lo));
    g_dumped[lo] = h;
    g_ndumped++;
    return 0;
}

void texreplace_init(const char *game_dir, int replace, int dump)
{
    char dir[MAX_PATH];
    crc_init();
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\Modernization\\textures", game_dir);
    g_replace = replace;
    g_dump = dump;
    if (replace) {
        scan(dir, 0);
        if (g_count)
            qsort(g_entries, (size_t)g_count, sizeof *g_entries, by_hash);
        log_printf("textures: %d replacement%s in %s", g_count, g_count == 1 ? "" : "s", dir);
    }
    if (dump) {
        CreateDirectoryA(dir, NULL);
        _snprintf_s(g_dump_dir, sizeof g_dump_dir, _TRUNCATE, "%s\\dump", dir);
        CreateDirectoryA(g_dump_dir, NULL);
        log_printf("textures: dumping new textures to %s", g_dump_dir);
    }
}

int texreplace_active(void)
{
    return (g_replace && g_count) || g_dump;
}

const uint32_t *texreplace_filter(const uint32_t *rgba, int w, int h, int *out_w, int *out_h)
{
    uint32_t hash;
    int dims[2];
    if (!texreplace_active() || !rgba || w <= 0 || h <= 0)
        return NULL;
    dims[0] = w;
    dims[1] = h;
    hash = crc_update(crc_update(0, dims, sizeof dims), rgba, (size_t)w * h * 4);

    if (g_dump && !dumped_before(hash)) {
        char file[MAX_PATH];
        _snprintf_s(file, sizeof file, _TRUNCATE, "%s\\%08x.png", g_dump_dir, hash);
        if (GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES && save_png(file, rgba, w, h)) {
            if (++g_written % 50 == 1)
                log_printf("textures: %u dumped so far", g_written);
        }
    }
    if (g_replace && g_count) {
        entry key, *e;
        key.hash = hash;
        e = (entry *)bsearch(&key, g_entries, (size_t)g_count, sizeof *g_entries, by_hash);
        if (e && !e->failed) {
            if (!e->pixels && !load_png(e)) {
                e->failed = 1;
                return NULL;
            }
            if (++g_replaced % 100 == 1)
                log_printf("textures: %u replacement uploads so far (%s %dx%d)", g_replaced, e->path, e->w, e->h);
            *out_w = e->w;
            *out_h = e->h;
            return e->pixels;
        }
    }
    return NULL;
}
