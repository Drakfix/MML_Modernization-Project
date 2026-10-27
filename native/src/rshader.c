/* RetroArch shader presets (.slangp: CRT, scanlines, NTSC, bezels...) through
 * librashader ([video] shader=path\to\preset.slangp).
 *
 * librashader.dll (MPL-2.0, built for 32-bit, loaded at runtime) runs the
 * whole RetroArch shader pipeline on Direct3D 11. Our renderer draws the game
 * into its scene target as usual; at compose time the preset turns that image
 * into the window's picture instead of our plain upscale. Shaders aren't
 * shipped: the preset path points at the user's own copy (most of them are
 * GPL, ours is MIT). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LIBRA_RUNTIME_D3D11
#include "../vendor/librashader/librashader.h"

#include "log.h"
#include "rshader.h"

static struct {
    PFN_libra_instance_abi_version abi_version;
    PFN_libra_preset_create preset_create;
    PFN_libra_preset_free preset_free;
    PFN_libra_d3d11_filter_chain_create chain_create;
    PFN_libra_d3d11_filter_chain_frame chain_frame;
    PFN_libra_d3d11_filter_chain_free chain_free;
    PFN_libra_error_write error_write;
    PFN_libra_error_free_string error_free_string;
    PFN_libra_error_free error_free;
} L;
static libra_d3d11_filter_chain_t g_chain;
static size_t g_frame_count;
static int g_failed_frames;

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

static int load_library(const char *game_dir)
{
    char path[MAX_PATH];
    HMODULE m;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\librashader.dll", game_dir);
    m = LoadLibraryA(path);
    if (!m) {
        log_printf("shader: %s not found, shaders off", path);
        return 0;
    }
#define GET(field, name) *(FARPROC *)&L.field = GetProcAddress(m, name)
    GET(abi_version, "libra_instance_abi_version");
    GET(preset_create, "libra_preset_create");
    GET(preset_free, "libra_preset_free");
    GET(chain_create, "libra_d3d11_filter_chain_create");
    GET(chain_frame, "libra_d3d11_filter_chain_frame");
    GET(chain_free, "libra_d3d11_filter_chain_free");
    GET(error_write, "libra_error_write");
    GET(error_free_string, "libra_error_free_string");
    GET(error_free, "libra_error_free");
#undef GET
    if (!L.abi_version || !L.preset_create || !L.chain_create || !L.chain_frame || !L.chain_free ||
        !L.error_write || !L.error_free_string || !L.error_free) {
        log_printf("shader: librashader.dll is missing functions (not a Direct3D 11 build?), shaders off");
        return 0;
    }
    if (L.abi_version() != LIBRASHADER_CURRENT_ABI) {
        log_printf("shader: librashader.dll ABI %d, expected %d, shaders off", (int)L.abi_version(),
                   LIBRASHADER_CURRENT_ABI);
        return 0;
    }
    return 1;
}

static struct { ID3D11Device *dev; const char *path; int ok; } g_job;

static int build_chain(ID3D11Device *dev, const char *preset_path);

/* The game's main thread has a 128 KB stack (the exe asks for no more), far
 * too little for the shader compilers inside librashader, so the chain is
 * built on a thread of our own with room to spare. */
static DWORD WINAPI build_thread(void *p)
{
    (void)p;
    g_job.ok = build_chain(g_job.dev, g_job.path);
    return 0;
}

int rshader_load(ID3D11Device *dev, const char *game_dir, const char *preset_path)
{
    HANDLE t;
    if (!preset_path || !preset_path[0] || !load_library(game_dir))
        return 0;
    g_job.dev = dev;
    g_job.path = preset_path;
    g_job.ok = 0;
    t = CreateThread(NULL, 16u << 20, build_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t)
        return 0;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return g_job.ok;
}

static int build_chain(ID3D11Device *dev, const char *preset_path)
{
    libra_shader_preset_t preset = NULL;
    filter_chain_d3d11_opt_t opt;
    if (failed(L.preset_create(preset_path, &preset), "loading the preset"))
        return 0;
    memset(&opt, 0, sizeof opt);
    opt.version = LIBRASHADER_CURRENT_VERSION;
    /* consumes the preset */
    if (failed(L.chain_create(&preset, dev, &opt, &g_chain), "building the shader chain")) {
        if (preset)
            L.preset_free(&preset);
        g_chain = NULL;
        return 0;
    }
    log_printf("shader: %s", preset_path);
    return 1;
}

int rshader_active(void)
{
    return g_chain != NULL;
}

int rshader_frame(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                  float x, float y, unsigned w, unsigned h)
{
    libra_viewport_t vp;
    if (!g_chain)
        return 0;
    vp.x = x;
    vp.y = y;
    vp.width = w;
    vp.height = h;
    if (failed(L.chain_frame(&g_chain, ctx, g_frame_count++, src, dst, &vp, NULL, NULL), "drawing a frame")) {
        if (++g_failed_frames >= 30) {   /* keeps failing: plain picture from now on */
            log_printf("shader: turned off after repeated errors");
            L.chain_free(&g_chain);
            g_chain = NULL;
        }
        return 0;
    }
    return 1;
}
