/* RetroArch shader presets (.slangp: CRT, scanlines, NTSC, bezels...) through
 * librashader ([video] shader=path\to\preset.slangp).
 *
 * librashader (MPL-2.0, built from source, loaded at run time) runs the whole RetroArch shader
 * pipeline on Direct3D 11 (Windows) or Vulkan (Windows, Linux, Android). Our renderer draws the
 * game into its scene target as usual; at compose time the preset turns that image into the
 * window's picture instead of our plain upscale. Shaders aren't shipped: the preset path points
 * at the user's own copy (most of them are GPL, ours is MIT).
 *
 * Where it lives: the 32-bit game folder's librashader.dll, x64\librashader.dll for the 64-bit
 * runner, librashader.so in the APK (Android) or in the package's lib/ (Linux). */
#include <windows.h>   /* (native/compat elsewhere) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "rshader.h"   /* (first: the Vulkan headers without prototypes) */
#ifdef _WIN32
#define LIBRA_RUNTIME_D3D11
#endif
#ifdef MML_VULKAN
#define LIBRA_RUNTIME_VULKAN
#endif
#include "../vendor/librashader/librashader.h"

#include "log.h"

#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0
#endif

enum { CH_NONE, CH_D3D11, CH_VK };

static struct {
    HMODULE lib;
    PFN_libra_instance_abi_version abi_version;
    PFN_libra_preset_create preset_create;
    PFN_libra_preset_free preset_free;
    PFN_libra_error_write error_write;
    PFN_libra_error_free_string error_free_string;
    PFN_libra_error_free error_free;
#ifdef _WIN32
    PFN_libra_d3d11_filter_chain_create d3d11_create;
    PFN_libra_d3d11_filter_chain_frame d3d11_frame;
    PFN_libra_d3d11_filter_chain_free d3d11_free;
#endif
#ifdef MML_VULKAN
    PFN_libra_vk_filter_chain_create vk_create;
    PFN_libra_vk_filter_chain_frame vk_frame;
    PFN_libra_vk_filter_chain_free vk_free;
#endif
} L;

static int g_kind;   /* CH_*: which chain is alive */
#ifdef _WIN32
static libra_d3d11_filter_chain_t g_d3d11;
#endif
#ifdef MML_VULKAN
static libra_vk_filter_chain_t g_vk;
#endif
static size_t g_frame_count;
static int g_failed_frames;
static int g_disabled;   /* given up on (Vulkan: kept until the GPU is idle) */

/* log a librashader error and free it; returns 1 if there was one */
static int failed(libra_error_t err, const char *what)
{
    char *msg = NULL;
    if (!err)
        return 0;
    if (L.error_write && L.error_write(err, &msg) == 0 && msg) {
        log_printf("shader: %s failed: %s", what, msg);
        L.error_free_string(&msg);
    } else {
        log_printf("shader: %s failed", what);
    }
    L.error_free(&err);
    return 1;
}

static HMODULE open_library(const char *game_dir, char *path, size_t n)
{
    HMODULE m;
#if defined(_WIN64)
    _snprintf_s(path, n, _TRUNCATE, "%s\\x64\\librashader.dll", game_dir);   /* the 64-bit build's */
#elif defined(_WIN32)
    _snprintf_s(path, n, _TRUNCATE, "%s\\librashader.dll", game_dir);
#else
    /* by name: Android, the APK's own; Linux, the package's lib/ (the program's rpath, searched
       before the system's libraries) */
    _snprintf_s(path, n, _TRUNCATE, "librashader.so");
#endif
    m = LoadLibraryA(path);
#if !defined(_WIN32) && !defined(__ANDROID__)
    if (!m) {   /* (or put next to the game) */
        _snprintf_s(path, n, _TRUNCATE, "%s/librashader.so", game_dir);
        m = LoadLibraryA(path);
    }
#else
    (void)game_dir;
#endif
    return m;
}

/* once: 1 when the library and its common functions are there */
static int load_library(const char *game_dir)
{
    static int tried;
    char path[MAX_PATH];
    HMODULE m;
    if (L.lib)
        return 1;
    if (tried)
        return 0;
    tried = 1;
    m = open_library(game_dir, path, sizeof path);
    if (!m) {
#ifdef _WIN32
        log_printf("shader: %s not found, shaders off", path);
#else
        {   /* (the linker's reason: a missing symbol looks like a missing file otherwise) */
            const char *why = dlerror();
            log_printf("shader: %s didn't load (%s), shaders off", path, why ? why : "not found");
        }
#endif
        return 0;
    }
#define GET(field, name) *(FARPROC *)&L.field = GetProcAddress(m, name)
    GET(abi_version, "libra_instance_abi_version");
    GET(preset_create, "libra_preset_create");
    GET(preset_free, "libra_preset_free");
    GET(error_write, "libra_error_write");
    GET(error_free_string, "libra_error_free_string");
    GET(error_free, "libra_error_free");
#ifdef _WIN32
    GET(d3d11_create, "libra_d3d11_filter_chain_create");
    GET(d3d11_frame, "libra_d3d11_filter_chain_frame");
    GET(d3d11_free, "libra_d3d11_filter_chain_free");
#endif
#ifdef MML_VULKAN
    GET(vk_create, "libra_vk_filter_chain_create");
    GET(vk_frame, "libra_vk_filter_chain_frame");
    GET(vk_free, "libra_vk_filter_chain_free");
#endif
#undef GET
    if (!L.abi_version || !L.preset_create || !L.preset_free || !L.error_write || !L.error_free_string ||
        !L.error_free) {
        log_printf("shader: %s is missing functions, shaders off", path);
        return 0;
    }
    if (L.abi_version() != LIBRASHADER_CURRENT_ABI) {
        log_printf("shader: %s ABI %d, expected %d, shaders off", path, (int)L.abi_version(),
                   LIBRASHADER_CURRENT_ABI);
        return 0;
    }
    L.lib = m;
    return 1;
}

/* the chain there is, freed (its GPU objects at once: callers have waited for the GPU) */
static void free_chain(void)
{
#ifdef _WIN32
    if (g_kind == CH_D3D11 && g_d3d11) {
        L.d3d11_free(&g_d3d11);
        g_d3d11 = NULL;
    }
#endif
#ifdef MML_VULKAN
    if (g_kind == CH_VK && g_vk) {
        L.vk_free(&g_vk);
        g_vk = NULL;
    }
#endif
    g_kind = CH_NONE;
    g_disabled = 0;
    g_failed_frames = 0;
    g_frame_count = 0;
}

int rshader_active(void)
{
    return g_kind != CH_NONE && !g_disabled;
}

/* a frame that failed: after many in a row, the plain picture from then on */
static void frame_failed(void)
{
    if (++g_failed_frames < 30)
        return;
    log_printf("shader: turned off after repeated errors");
#ifdef MML_VULKAN
    if (g_kind == CH_VK) {   /* frames in flight may still use it: freed with the next load */
        g_disabled = 1;
        return;
    }
#endif
    free_chain();
}

/* ---------------------------------------------------------------- building, on a thread of its own */

/* The game's main thread has a 128 KB stack (the exe asks for no more), far too little for the
 * shader compilers inside librashader, so chains are built on a thread with room to spare. */
static struct {
    int kind;
    const char *path;
    int ok;
#ifdef _WIN32
    ID3D11Device *d3d11;
#endif
#ifdef MML_VULKAN
    const rshader_vk_device *vk;
#endif
} g_job;

static int build_chain(void)
{
    libra_shader_preset_t preset = NULL;
    libra_error_t err = NULL;
    char path[MAX_PATH];
    strcpy_s(path, sizeof path, g_job.path);
#ifndef _WIN32
    {   /* the ini and the menu write Windows paths */
        char *p;
        for (p = path; *p; p++)
            if (*p == '\\')
                *p = '/';
    }
#endif
    if (failed(L.preset_create(path, &preset), "loading the preset"))
        return 0;
#ifdef _WIN32
    if (g_job.kind == CH_D3D11) {
        filter_chain_d3d11_opt_t opt;
        memset(&opt, 0, sizeof opt);
        opt.version = LIBRASHADER_CURRENT_VERSION;
        err = L.d3d11_create(&preset, g_job.d3d11, &opt, &g_d3d11);   /* (consumes the preset) */
        if (err)
            g_d3d11 = NULL;
    }
#endif
#ifdef MML_VULKAN
    if (g_job.kind == CH_VK) {
        filter_chain_vk_opt_t opt;
        libra_device_vk_t dev;
        memset(&opt, 0, sizeof opt);
        opt.version = LIBRASHADER_CURRENT_VERSION;
        opt.frames_in_flight = g_job.vk->frames_in_flight;
        opt.use_dynamic_rendering = false;   /* render passes: Vulkan 1.1, no extension needed */
        memset(&dev, 0, sizeof dev);
        dev.physical_device = g_job.vk->phys;
        dev.instance = g_job.vk->inst;
        dev.device = g_job.vk->dev;
        dev.queue = g_job.vk->queue;
        dev.entry = g_job.vk->gipa;
        err = L.vk_create(&preset, dev, &opt, &g_vk);
        if (err)
            g_vk = NULL;
    }
#endif
    if (failed(err, "building the shader chain")) {
        if (preset)
            L.preset_free(&preset);
        return 0;
    }
    g_kind = g_job.kind;
    log_printf("shader: %s", path);
    return 1;
}

static DWORD WINAPI build_thread(void *p)
{
    (void)p;
    g_job.ok = build_chain();
    return 0;
}

/* the chain of g_job: built and made the one in use (the old one freed first) */
static int build(const char *game_dir)
{
    HANDLE t;
    free_chain();
    if (!g_job.path || !g_job.path[0])
        return 1;   /* off */
    if (!load_library(game_dir))
        return 0;
    g_job.ok = 0;
    t = CreateThread(NULL, 16u << 20, build_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t)
        return 0;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return g_job.ok;
}

/* ---------------------------------------------------------------- Direct3D 11 */

#ifdef _WIN32
int rshader_load(ID3D11Device *dev, const char *game_dir, const char *preset_path)
{
    if (!preset_path || !preset_path[0])
        return 0;
    return rshader_reload(dev, game_dir, preset_path);
}

int rshader_reload(ID3D11Device *dev, const char *game_dir, const char *preset_path)
{
    if (preset_path && preset_path[0] && load_library(game_dir) && !L.d3d11_create) {
        free_chain();
        log_printf("shader: this librashader has no Direct3D 11 runtime, shaders off");
        return 0;
    }
    memset(&g_job, 0, sizeof g_job);
    g_job.kind = CH_D3D11;
    g_job.path = preset_path;
    g_job.d3d11 = dev;
    return build(game_dir);
}

int rshader_frame(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                  float x, float y, unsigned w, unsigned h)
{
    libra_viewport_t vp;
    if (g_kind != CH_D3D11)
        return 0;
    vp.x = x;
    vp.y = y;
    vp.width = w;
    vp.height = h;
    if (failed(L.d3d11_frame(&g_d3d11, ctx, g_frame_count++, src, dst, &vp, NULL, NULL), "drawing a frame")) {
        frame_failed();
        return 0;
    }
    return 1;
}
#endif

/* ---------------------------------------------------------------- Vulkan */

#ifdef MML_VULKAN
int rshader_vk_load(const rshader_vk_device *d, const char *game_dir, const char *preset_path)
{
    if (preset_path && preset_path[0] && load_library(game_dir) && (!L.vk_create || !L.vk_frame || !L.vk_free)) {
        free_chain();
        log_printf("shader: this librashader has no Vulkan runtime, shaders off");
        return 0;
    }
    memset(&g_job, 0, sizeof g_job);
    g_job.kind = CH_VK;
    g_job.path = preset_path;
    g_job.vk = d;
    return build(game_dir);
}

int rshader_vk_frame(VkCommandBuffer c, const rshader_vk_image *src, const rshader_vk_image *dst, float x, float y,
                     unsigned w, unsigned h)
{
    libra_image_vk_t in, out;
    libra_viewport_t vp;
    if (g_kind != CH_VK || g_disabled)
        return 0;
    in.handle = src->img;
    in.format = src->fmt;
    in.width = src->w;
    in.height = src->h;
    out.handle = dst->img;
    out.format = dst->fmt;
    out.width = dst->w;
    out.height = dst->h;
    vp.x = x;
    vp.y = y;
    vp.width = w;
    vp.height = h;
    if (failed(L.vk_frame(&g_vk, c, g_frame_count++, in, out, &vp, NULL, NULL), "drawing a frame")) {
        frame_failed();
        return 0;
    }
    return 1;
}
#endif
