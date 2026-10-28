/* Direct3D 11 backend for gfx.h.
 *
 * The game's frame is drawn into an offscreen target at display resolution
 * (its own 854x480/640x480 coordinates are just scaled up in the vertex
 * shader), then blitted into a flip-model swap chain on the game's own
 * window. The window is a plain borderless (or framed) window, never
 * exclusive fullscreen, so alt-tab, screenshots and overlays behave. */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <initguid.h>   /* define the DXGI/D3D11 IIDs here instead of linking dxguid.lib */
#include <d3d11.h>
#include <d3d11_4.h>   /* ID3D11Multithread */
#include <dxgi1_6.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "log.h"
#include "rshader.h"
#include "gfx_ui.h"

#include "shaders/game_vs.h"
#include "shaders/game_ps.h"
#include "shaders/blit_vs.h"
#include "shaders/blit_ps.h"
#include "shaders/depth_copy_ps.h"
#include "shaders/show_depth_ps.h"

struct gfx_tex {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    int w, h;
};

typedef struct {
    float app_size[2];
    float textured, flat;
    float fog_color[4];
    float fog_start, fog_end, fog_on, fog_from_w;
    float alpha_test, alpha_func, alpha_ref, pad;
} draw_cb;

#define VB_BYTES (4 << 20)
#define MAX_BLEND 64
#define MAX_DEPTH 32

static int g_flip_mt;   /* the context is multithread-protected */
static int g_reshade;   /* ReShade is loaded: present on the game thread (see gfx_flip_at) */
static int g_depth_done[2];   /* effects depth already copied into target t's depth buffer */
static void flush_batch(void);
void gfx_wait_flip(void);

static struct {
    int ready;
    HWND hwnd;
    gfx_options opt;
    int app_w, app_h;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain1 *swap;
    ID3D11RenderTargetView *back_rtv;
    int back_w, back_h;
    /* scene targets: 0 = the game's frame, 1 = the in-between frame (made on first use) */
    struct {
        ID3D11Texture2D *scene, *scene_resolved, *depth;
        ID3D11RenderTargetView *scene_rtv;
        ID3D11ShaderResourceView *scene_srv;
        ID3D11DepthStencilView *dsv;
        /* effects depth (single-sample only): depth of every visible world surface */
        ID3D11Texture2D *eff;
        ID3D11RenderTargetView *eff_rtv;
        ID3D11ShaderResourceView *eff_srv;
    } tg[2];
    int cur, tg_used[2];
    int scene_w, scene_h;
    /* pipeline */
    ID3D11VertexShader *vs, *blit_vs;
    ID3D11PixelShader *ps, *blit_ps, *depth_copy_ps, *show_depth_ps;
    ID3D11DepthStencilState *ds_always;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vb, *cb;
    UINT vb_pos;
    ID3D11RasterizerState *raster;
    ID3D11SamplerState *smp_point, *smp_linear, *smp_blit_linear, *smp_blit_point;
    struct { uint32_t key; ID3D11BlendState *s; } blends[MAX_BLEND];
    int nblend;
    struct { uint32_t key; ID3D11DepthStencilState *s; } depths[MAX_DEPTH];
    int ndepth;
    /* current state */
    gfx_state st;
    gfx_tex *tex;
    int cb_dirty, state_dirty;
    draw_cb cbd;
} g;

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* ---------------------------------------------------------------- setup */

/* the GPU whose outputs include the monitor the window is on */
static IDXGIAdapter1 *display_adapter(IDXGIFactory1 *f1)
{
    HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTOPRIMARY);
    IDXGIAdapter1 *a;
    UINT i, j;
    for (i = 0; IDXGIFactory1_EnumAdapters1(f1, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput *o;
        for (j = 0; IDXGIAdapter1_EnumOutputs(a, j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od;
            int match = SUCCEEDED(IDXGIOutput_GetDesc(o, &od)) && od.Monitor == mon;
            IDXGIOutput_Release(o);
            if (match)
                return a;
        }
        IDXGIAdapter1_Release(a);
    }
    return NULL;
}

static IDXGIAdapter1 *pick_adapter(IDXGIFactory1 *f1)
{
    IDXGIFactory6 *f6 = NULL;
    IDXGIAdapter1 *a = NULL;
    /* gpu=display (and exclusive fullscreen): render where the picture is shown, so nothing gets copied
     * between GPUs (laptops with two); else prefer the high-performance GPU */
    /* exclusive fullscreen only works on the GPU that owns the monitor */
    if (g.opt.gpu_display || (g.opt.exclusive && !g.opt.windowed))
        a = display_adapter(f1);
    if (!a && SUCCEEDED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory6, (void **)&f6))) {
        IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 &IID_IDXGIAdapter1, (void **)&a);
        IDXGIFactory6_Release(f6);
    }
    if (!a)
        IDXGIFactory1_EnumAdapters1(f1, 0, &a);
    if (a) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)))
            log_printf("gfx: adapter %ls (%u MB)", d.Description, (unsigned)(d.DedicatedVideoMemory >> 20));
    }
    return a;
}

static void place_window(void);

/* Before the game has a Direct3D device (e.g. while the intro video plays on
 * its own temporary DirectDraw object): put the window where the renderer will
 * want it and paint it black, so nothing of the desktop shows around the video. */
void gfx_prepare_window(HWND hwnd, int app_w, int app_h, const gfx_options *opt)
{
    RECT rc;
    HDC dc;
    if (g.ready || !hwnd)
        return;
    g.hwnd = hwnd;
    g.opt = *opt;
    g.app_w = app_w > 0 ? app_w : 640;
    g.app_h = app_h > 0 ? app_h : 480;
    place_window();
    GetClientRect(hwnd, &rc);
    dc = GetDC(hwnd);
    if (dc) {
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        ReleaseDC(hwnd, dc);
    }
}

static void place_window(void)
{
    HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(mon, &mi);
    if (!g.opt.windowed) {
        RECT r = mi.rcMonitor;
        SetWindowLongW(g.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongW(g.hwnd, GWL_EXSTYLE, 0);
        SetWindowPos(g.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        RECT work = mi.rcWork, r;
        int cw = g.opt.window_w, ch = g.opt.window_h;
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        if (cw <= 0 || ch <= 0) {
            ch = (work.bottom - work.top) * 3 / 4;
            cw = ch * g.app_w / g.app_h;
        }
        r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
        AdjustWindowRectEx(&r, style, FALSE, 0);
        SetWindowLongW(g.hwnd, GWL_STYLE, style);
        SetWindowLongW(g.hwnd, GWL_EXSTYLE, 0);
        SetWindowPos(g.hwnd, HWND_TOP,
                     work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                     work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                     r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

static void release_back(void)
{
    REL(g.back_rtv);
}

static int make_back(void)
{
    ID3D11Texture2D *bb = NULL;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    g.back_w = rc.right > 0 ? rc.right : 1;
    g.back_h = rc.bottom > 0 ? rc.bottom : 1;
    if (FAILED(IDXGISwapChain1_ResizeBuffers(g.swap, 0, g.back_w, g.back_h, DXGI_FORMAT_UNKNOWN, 0)))
        return 0;
    if (FAILED(IDXGISwapChain1_GetBuffer(g.swap, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return 0;
    ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)bb, NULL, &g.back_rtv);
    ID3D11Texture2D_Release(bb);
    return g.back_rtv != NULL;
}

static void release_scene(void)
{
    int t;
    for (t = 0; t < 2; t++) {
        REL(g.tg[t].scene_srv);
        REL(g.tg[t].scene_rtv);
        REL(g.tg[t].dsv);
        REL(g.tg[t].scene);
        REL(g.tg[t].scene_resolved);
        REL(g.tg[t].depth);
        REL(g.tg[t].eff_srv);
        REL(g.tg[t].eff_rtv);
        REL(g.tg[t].eff);
    }
}

/* scene size: the game's aspect, as tall as the window (or N x its mode) */
static void scene_size(int *w, int *h)
{
    if (g.opt.render_scale > 0) {
        *h = g.app_h * g.opt.render_scale;
    } else {
        *h = g.back_h > 0 ? g.back_h : g.app_h;
    }
    *w = (int)((double)*h * g.app_w / g.app_h + 0.5);
    if (*w > 16384) *w = 16384;
    if (*h > 16384) *h = 16384;
}

static int make_target(int t);

/* (re)create every scene target in use */
static int make_scene(void)
{
    int t, ok = 1;
    g.tg_used[0] = 1;
    for (t = 0; t < 2; t++)
        if (g.tg_used[t])
            ok &= make_target(t);
    return ok;
}

static int make_target(int t)
{
    D3D11_TEXTURE2D_DESC d;
    int w, h;
    UINT samples = (UINT)(g.opt.msaa > 1 ? g.opt.msaa : 1), q = 0;
    scene_size(&w, &h);
    if (samples > 1 && (FAILED(ID3D11Device_CheckMultisampleQualityLevels(g.dev, DXGI_FORMAT_B8G8R8A8_UNORM, samples, &q)) || q == 0)) {
        log_printf("gfx: %ux MSAA not supported, off", samples);
        samples = 1;
    }
    memset(&d, 0, sizeof d);
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | (samples == 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
    if (FAILED(ID3D11Device_CreateTexture2D(g.dev, &d, NULL, &g.tg[t].scene)))
        return 0;
    ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)g.tg[t].scene, NULL, &g.tg[t].scene_rtv);
    if (samples > 1) {
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device_CreateTexture2D(g.dev, &d, NULL, &g.tg[t].scene_resolved);
        ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)g.tg[t].scene_resolved, NULL, &g.tg[t].scene_srv);
    } else {
        ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)g.tg[t].scene, NULL, &g.tg[t].scene_srv);
    }
    d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.SampleDesc.Count = samples;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(g.dev, &d, NULL, &g.tg[t].depth)))
        return 0;
    ID3D11Device_CreateDepthStencilView(g.dev, (ID3D11Resource *)g.tg[t].depth, NULL, &g.tg[t].dsv);
    if (samples == 1) {
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g.dev, &d, NULL, &g.tg[t].eff))) {
            ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)g.tg[t].eff, NULL, &g.tg[t].eff_rtv);
            ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)g.tg[t].eff, NULL, &g.tg[t].eff_srv);
        }
    }
    g.scene_w = w;
    g.scene_h = h;
    if (t == 0)
        log_printf("gfx: scene %dx%d (%ux MSAA) for the game's %dx%d, window %dx%d", w, h, samples, g.app_w, g.app_h,
                   g.back_w, g.back_h);
    return g.tg[t].scene_rtv && g.tg[t].scene_srv && g.tg[t].dsv;
}

static ID3D11SamplerState *make_sampler(D3D11_FILTER f, D3D11_TEXTURE_ADDRESS_MODE a, UINT aniso)
{
    D3D11_SAMPLER_DESC s;
    ID3D11SamplerState *out = NULL;
    memset(&s, 0, sizeof s);
    s.Filter = f;
    s.AddressU = s.AddressV = s.AddressW = a;
    s.MaxAnisotropy = aniso ? aniso : 1;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.MaxLOD = FLT_MAX;
    ID3D11Device_CreateSamplerState(g.dev, &s, &out);
    return out;
}

int gfx_init(HWND hwnd, int app_w, int app_h, const gfx_options *opt)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    IDXGIFactory1 *f1 = NULL;
    IDXGIFactory2 *f2 = NULL;
    IDXGIAdapter1 *adapter;
    DXGI_SWAP_CHAIN_DESC1 sd;
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (g.ready)
        return 1;
    g.hwnd = hwnd;
    g.opt = *opt;
    g.app_w = app_w;
    g.app_h = app_h;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1)))
        return 0;
    adapter = pick_adapter(f1);
    hr = D3D11CreateDevice((IDXGIAdapter *)adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                           NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
                           &g.dev, NULL, &g.ctx);
    REL(adapter);
    if (FAILED(hr)) {
        log_printf("gfx: D3D11CreateDevice failed %08lX", (unsigned long)hr);
        REL(f1);
        return 0;
    }
    {   /* the timer thread presents while the game thread draws: serialise the context */
        ID3D11Multithread *mt = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(g.dev, &IID_ID3D11Multithread, (void **)&mt))) {
            ID3D11Multithread_SetMultithreadProtected(mt, TRUE);
            g_flip_mt = 1;
            REL(mt);
        }
    }
    place_window();

    if (FAILED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory2, (void **)&f2))) {
        REL(f1);
        return 0;
    }
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown *)g.dev, hwnd, &sd, NULL, NULL, &g.swap);
    if (FAILED(hr)) {
        /* Windows 8.1 and older have no FLIP_DISCARD */
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown *)g.dev, hwnd, &sd, NULL, NULL, &g.swap);
    }
    if (SUCCEEDED(hr))
        IDXGIFactory2_MakeWindowAssociation(f2, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    REL(f2);
    REL(f1);
    if (FAILED(hr)) {
        log_printf("gfx: swap chain failed %08lX", (unsigned long)hr);
        return 0;
    }
    if (!make_back() || !make_scene())
        return 0;
    {
        HMODULE m = GetModuleHandleA("dxgi.dll"), m2 = GetModuleHandleA("d3d11.dll");
        g_reshade = (m && GetProcAddress(m, "ReShadeRegisterAddon")) || (m2 && GetProcAddress(m2, "ReShadeRegisterAddon"));
        if (g_reshade)
            log_printf("gfx: ReShade detected: frames presented on the game thread, finished depth for its effects");
    }

    ID3D11Device_CreateVertexShader(g.dev, g_vs_main, sizeof g_vs_main, NULL, &g.vs);
    ID3D11Device_CreatePixelShader(g.dev, g_ps_main, sizeof g_ps_main, NULL, &g.ps);
    ID3D11Device_CreateVertexShader(g.dev, g_vs_blit, sizeof g_vs_blit, NULL, &g.blit_vs);
    ID3D11Device_CreatePixelShader(g.dev, g_ps_blit, sizeof g_ps_blit, NULL, &g.blit_ps);
    ID3D11Device_CreatePixelShader(g.dev, g_ps_depth_copy, sizeof g_ps_depth_copy, NULL, &g.depth_copy_ps);
    ID3D11Device_CreatePixelShader(g.dev, g_ps_show_depth, sizeof g_ps_show_depth, NULL, &g.show_depth_ps);
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ID3D11Device_CreateDepthStencilState(g.dev, &dd, &g.ds_always);
    }
    ID3D11Device_CreateInputLayout(g.dev, il, 4, g_vs_main, sizeof g_vs_main, &g.layout);

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.vb);
    bd.ByteWidth = sizeof(draw_cb);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.cb);

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = g.opt.msaa > 1;
    ID3D11Device_CreateRasterizerState(g.dev, &rd, &g.raster);

    g.smp_point = make_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP, 0);
    g.smp_linear = g.opt.anisotropy > 1
        ? make_sampler(D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, (UINT)g.opt.anisotropy)
        : make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, 0);
    g.smp_blit_linear = make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, 0);
    g.smp_blit_point = make_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, 0);

    if (!g.vs || !g.ps || !g.blit_vs || !g.blit_ps || !g.layout || !g.vb || !g.cb || !g.raster) {
        log_printf("gfx: pipeline setup failed");
        return 0;
    }
    g.cb_dirty = g.state_dirty = 1;
    if (g.opt.shader[0])
        rshader_load(g.dev, g.opt.game_dir, g.opt.shader);
    g.ready = 1;
    log_printf("gfx: Direct3D 11 ready (%s)", g.opt.windowed ? "windowed" :
               g.opt.exclusive ? "exclusive fullscreen" : "borderless fullscreen");
    return 1;
}

int gfx_ready(void)
{
    return g.ready;
}

void gfx_set_mode(int app_w, int app_h)
{
    if (app_w <= 0 || app_h <= 0 || (app_w == g.app_w && app_h == g.app_h))
        return;
    g.app_w = app_w;
    g.app_h = app_h;
    if (g.ready) {
        release_scene();
        make_scene();
        g.cb_dirty = 1;
    }
}

/* ---------------------------------------------------------------- textures */

gfx_tex *gfx_tex_create(int w, int h)
{
    D3D11_TEXTURE2D_DESC d;
    gfx_tex *t;
    if (!g.ready)
        return NULL;
    t = (gfx_tex *)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    memset(&d, 0, sizeof d);
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g.dev, &d, NULL, &t->tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)t->tex, NULL, &t->srv))) {
        REL(t->tex);
        free(t);
        return NULL;
    }
    t->w = w;
    t->h = h;
    return t;
}

void gfx_tex_upload(gfx_tex *t, const uint32_t *rgba, int w, int h)
{
    if (!t || !g.ready || w != t->w || h != t->h)
        return;
    flush_batch();   /* queued draws must see the old contents */
    ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)t->tex, 0, NULL, rgba, (UINT)w * 4, 0);
}

void gfx_tex_size(const gfx_tex *t, int *w, int *h)
{
    *w = t ? t->w : 0;
    *h = t ? t->h : 0;
}

void gfx_tex_destroy(gfx_tex *t)
{
    if (!t)
        return;
    flush_batch();
    if (g.tex == t)
        g.tex = NULL;
    REL(t->srv);
    REL(t->tex);
    free(t);
}

void gfx_set_texture(gfx_tex *t)
{
    if (g.tex != t) {
        g.tex = t;
        g.cb_dirty = 1;
    }
}

void gfx_set_state(const gfx_state *s)
{
    if (memcmp(&g.st, s, sizeof *s) != 0) {
        g.st = *s;
        g.state_dirty = g.cb_dirty = 1;
    }
}

/* ---------------------------------------------------------------- drawing */

static D3D11_BLEND map_blend(int b, int alpha)
{
    switch (b) {
    case 1: return D3D11_BLEND_ZERO;
    case 2: return D3D11_BLEND_ONE;
    case 3: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 4: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 5: return D3D11_BLEND_SRC_ALPHA;
    case 6: return D3D11_BLEND_INV_SRC_ALPHA;
    case 7: return D3D11_BLEND_DEST_ALPHA;
    case 8: return D3D11_BLEND_INV_DEST_ALPHA;
    case 9: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 10: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 11: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
    }
}

static ID3D11BlendState *blend_state(void)
{
    /* world geometry records its depth: opaque draws, and ordinary alpha-blended ones
     * (most of the game's world is drawn with SRCALPHA/INVSRCALPHA, mostly solid
     * texels) weighted by coverage; additive glows and the like don't */
    int alpha_blend = g.st.blend && g.st.src_blend == 5 && g.st.dst_blend == 6;
    int eff = g.st.scene_depth && (!g.st.blend || alpha_blend);
    uint32_t key = (g.st.blend ? (1u << 16) | ((uint32_t)g.st.src_blend << 8) | g.st.dst_blend : 0) | ((uint32_t)eff << 17);
    D3D11_BLEND_DESC d;
    ID3D11BlendState *s = NULL;
    int i;
    for (i = 0; i < g.nblend; i++)
        if (g.blends[i].key == key)
            return g.blends[i].s;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = g.st.blend != 0;
    d.RenderTarget[0].SrcBlend = map_blend(g.st.src_blend, 0);
    d.RenderTarget[0].DestBlend = map_blend(g.st.dst_blend, 0);
    d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].SrcBlendAlpha = map_blend(g.st.src_blend, 1);
    d.RenderTarget[0].DestBlendAlpha = map_blend(g.st.dst_blend, 1);
    d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    d.IndependentBlendEnable = TRUE;
    d.RenderTarget[1].RenderTargetWriteMask = eff ? D3D11_COLOR_WRITE_ENABLE_RED : 0;
    if (eff && alpha_blend) {
        d.RenderTarget[1].BlendEnable = TRUE;
        d.RenderTarget[1].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        d.RenderTarget[1].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        d.RenderTarget[1].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[1].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[1].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[1].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    ID3D11Device_CreateBlendState(g.dev, &d, &s);
    if (g.nblend < MAX_BLEND) {
        g.blends[g.nblend].key = key;
        g.blends[g.nblend].s = s;
        g.nblend++;
    }
    return s;
}

static ID3D11DepthStencilState *depth_state(void)
{
    uint32_t key = ((uint32_t)g.st.z_enable << 16) | ((uint32_t)g.st.z_write << 8) | g.st.z_func;
    D3D11_DEPTH_STENCIL_DESC d;
    ID3D11DepthStencilState *s = NULL;
    int i;
    for (i = 0; i < g.ndepth; i++)
        if (g.depths[i].key == key)
            return g.depths[i].s;
    memset(&d, 0, sizeof d);
    d.DepthEnable = g.st.z_enable != 0;
    d.DepthWriteMask = g.st.z_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    /* D3DCMPFUNC and D3D11_COMPARISON_FUNC share values 1..8 */
    d.DepthFunc = (g.st.z_func >= 1 && g.st.z_func <= 8) ? (D3D11_COMPARISON_FUNC)g.st.z_func : D3D11_COMPARISON_LESS_EQUAL;
    ID3D11Device_CreateDepthStencilState(g.dev, &d, &s);
    if (g.ndepth < MAX_DEPTH) {
        g.depths[g.ndepth].key = key;
        g.depths[g.ndepth].s = s;
        g.ndepth++;
    }
    return s;
}

static void bind_scene(void)
{
    D3D11_VIEWPORT vp;
    flush_batch();
    UINT stride = sizeof(gfx_vertex), off = 0;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (float)g.scene_w;
    vp.Height = (float)g.scene_h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    {
        ID3D11RenderTargetView *rtv[2] = { g.tg[g.cur].scene_rtv, g.tg[g.cur].eff_rtv };
        ID3D11DeviceContext_OMSetRenderTargets(g.ctx, rtv[1] ? 2 : 1, rtv, g.tg[g.cur].dsv);
    }
    ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g.ctx, g.raster);
    ID3D11DeviceContext_IASetInputLayout(g.ctx, g.layout);
    ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 1, &g.vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(g.ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g.ctx, g.ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    g.state_dirty = g.cb_dirty = 1;
}

static void apply_state(void)
{
    if (g.state_dirty) {
        static const float factor[4] = { 0, 0, 0, 0 };
        ID3D11SamplerState *smp = g.st.linear ? g.smp_linear : g.smp_point;
        ID3D11DeviceContext_OMSetBlendState(g.ctx, blend_state(), factor, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(g.ctx, depth_state(), 0);
        ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 1, &smp);
        g.state_dirty = 0;
    }
    if (g.cb_dirty) {
        D3D11_MAPPED_SUBRESOURCE m;
        draw_cb *c = &g.cbd;
        ID3D11ShaderResourceView *srv = g.tex ? g.tex->srv : NULL;
        c->app_size[0] = (float)g.app_w;
        c->app_size[1] = (float)g.app_h;
        c->textured = g.tex ? 1.0f : 0.0f;
        c->flat = g.st.flat ? 1.0f : 0.0f;
        c->fog_color[0] = ((g.st.fog_color >> 16) & 0xFF) / 255.0f;
        c->fog_color[1] = ((g.st.fog_color >> 8) & 0xFF) / 255.0f;
        c->fog_color[2] = (g.st.fog_color & 0xFF) / 255.0f;
        c->fog_color[3] = 1.0f;
        c->fog_start = g.st.fog_start;
        c->fog_end = g.st.fog_end;
        c->fog_on = (g.st.fog && !g.opt.no_fog) ? 1.0f : 0.0f;
        c->fog_from_w = g.opt.fog_from_w ? 1.0f : 0.0f;
        c->alpha_test = g.st.alpha_test ? 1.0f : 0.0f;
        c->alpha_func = (float)g.st.alpha_func;
        c->alpha_ref = g.st.alpha_ref / 255.0f;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, c, sizeof *c);
            ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.cb, 0);
        }
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &srv);
        g.cb_dirty = 0;
    }
}

static void submit(D3D11_PRIMITIVE_TOPOLOGY topo, const gfx_vertex *v, int count)
{
    D3D11_MAPPED_SUBRESOURCE m;
    UINT bytes = (UINT)count * sizeof(gfx_vertex);
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (bytes > VB_BYTES)
        return;
    if (g.vb_pos + bytes > VB_BYTES) {
        g.vb_pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    if (FAILED(ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.vb, 0, mode, 0, &m)))
        return;
    memcpy((uint8_t *)m.pData + g.vb_pos, v, bytes);
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.vb, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, topo);
    ID3D11DeviceContext_Draw(g.ctx, (UINT)count, g.vb_pos / sizeof(gfx_vertex));
    g.vb_pos += bytes;
}

/* Batching: the game sends thousands of tiny draws a frame (one per polygon).
 * Consecutive ones with the same state and texture are queued as one triangle
 * list and submitted together when anything changes (state, texture, target,
 * a clear, a texture upload or destroy, presenting). Order is kept, so the
 * picture is the same with far fewer driver calls. */
#define BATCH_MAX (3 * 16384)
static gfx_vertex g_batch[BATCH_MAX];
static int g_nbatch;

static void flush_batch(void)
{
    if (g_nbatch) {
        int n = g_nbatch;
        g_nbatch = 0;
        submit(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, g_batch, n);
    }
}

void gfx_draw(int prim, const gfx_vertex *v, int count)
{
    int need, i;
    gfx_vertex *o;
    if (!g.ready || count < 3)
        return;
    if (g.state_dirty || g.cb_dirty) {
        flush_batch();   /* what's queued goes out with the state it was made with */
        apply_state();
    }
    need = prim == GFX_TRILIST ? count - count % 3 : (count - 2) * 3;
    if (need > BATCH_MAX)
        return;
    if (g_nbatch + need > BATCH_MAX)
        flush_batch();
    o = g_batch + g_nbatch;
    if (prim == GFX_TRILIST) {
        memcpy(o, v, sizeof *v * (size_t)need);
    } else if (prim == GFX_TRISTRIP) {
        for (i = 0; i + 2 < count; i++) {   /* first vertex stays first: flat colour comes from it */
            *o++ = v[i];
            *o++ = v[i + 1];
            *o++ = v[i + 2];
        }
    } else if (prim == GFX_TRIFAN) {
        for (i = 1; i + 1 < count; i++) {
            *o++ = v[0];
            *o++ = v[i];
            *o++ = v[i + 1];
        }
    } else {
        return;
    }
    g_nbatch += need;
}

void gfx_clear(const RECT *rect, int color_too, uint32_t color, int depth_too, float depth)
{
    float c[4];
    if (!g.ready)
        return;
    flush_batch();
    (void)rect;   /* the game only clears the whole screen */
    if (color_too) {
        c[0] = ((color >> 16) & 0xFF) / 255.0f;
        c[1] = ((color >> 8) & 0xFF) / 255.0f;
        c[2] = (color & 0xFF) / 255.0f;
        c[3] = 1.0f;
        ID3D11DeviceContext_ClearRenderTargetView(g.ctx, g.tg[g.cur].scene_rtv, c);
    }
    if (depth_too) {
        static const float far_[4] = { 1, 1, 1, 1 };
        ID3D11DeviceContext_ClearDepthStencilView(g.ctx, g.tg[g.cur].dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth, 0);
        if (g.tg[g.cur].eff_rtv)
            ID3D11DeviceContext_ClearRenderTargetView(g.ctx, g.tg[g.cur].eff_rtv, far_);
        g_depth_done[g.cur] = 0;
    }
    bind_scene();
}

/* ---- showing a composed frame later, from a timer thread ----
 * With interpolation the game's own frame goes up 16.7 ms after the in-between
 * one. Waiting for that on the game thread took half of its 33 ms frame away,
 * which heavy scenes (City Hall at full detail) couldn't spare. Now the frame
 * is composed into the back buffer right away and this thread presents it on
 * time while the game carries on; anything that touches the back buffer next
 * waits for it first. */
static HANDLE g_flip_thread, g_flip_go, g_flip_idle, g_flip_timer;
static volatile double g_flip_due;
static void (*g_flip_cb)(void);

static double flip_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static DWORD WINAPI flip_thread(void *p)
{
    (void)p;
    for (;;) {
        double left;
        WaitForSingleObject(g_flip_go, INFINITE);
        left = g_flip_due - flip_now_ms();
        if (left > 0.6 && g_flip_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left - 0.5) * 10000.0);
            if (SetWaitableTimer(g_flip_timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(g_flip_timer, INFINITE);
        }
        while (flip_now_ms() < g_flip_due)
            YieldProcessor();
        IDXGISwapChain1_Present(g.swap, g.opt.vsync ? 1 : 0, 0);
        if (g_flip_cb)
            g_flip_cb();
        SetEvent(g_flip_idle);
    }
}

void gfx_wait_flip(void)
{
    if (g_flip_idle)
        WaitForSingleObject(g_flip_idle, INFINITE);
}

void gfx_flip_at(double due_ms, void (*on_flip)(void))
{
    if (!g.ready)
        return;
    /* ReShade reads the depth buffer when a frame is presented: by the time a
     * deferred present ran, the next frame had started drawing into it */
    if (g_flip_mt && !g_flip_thread && !g_reshade) {
        g_flip_go = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_flip_idle = CreateEventW(NULL, TRUE, TRUE, NULL);
        g_flip_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (g_flip_go && g_flip_idle)
            g_flip_thread = CreateThread(NULL, 0, flip_thread, NULL, 0, NULL);
        if (g_flip_thread)
            SetThreadPriority(g_flip_thread, THREAD_PRIORITY_HIGHEST);
        log_printf("gfx: %s", g_flip_thread ? "game frames presented from a timer thread"
                                            : "timer thread failed, presenting on the game thread");
    }
    if (!g_flip_thread) {   /* fallback: the old way, wait here */
        while (flip_now_ms() < due_ms)
            Sleep(0);
        IDXGISwapChain1_Present(g.swap, g.opt.vsync ? 1 : 0, 0);
        if (on_flip)
            on_flip();
        return;
    }
    gfx_wait_flip();
    g_flip_cb = on_flip;
    g_flip_due = due_ms;
    ResetEvent(g_flip_idle);
    SetEvent(g_flip_go);
}

void gfx_present(void)
{
    gfx_present_target(g.cur);
}

/* ---- changing settings while the game runs (in-game menu); game thread only,
 * between frames ---- */

const gfx_options *gfx_get_options(void)
{
    return &g.opt;
}

void gfx_set_vsync(int on)
{
    g.opt.vsync = on;
}

void gfx_set_nearest_upscale(int on)
{
    g.opt.nearest_upscale = on;
}

void gfx_set_anisotropy(int n)
{
    if (!g.ready)
        return;
    flush_batch();
    g.opt.anisotropy = n;
    REL(g.smp_linear);
    g.smp_linear = n > 1 ? make_sampler(D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, (UINT)n)
                         : make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, 0);
    g.state_dirty = 1;
}

/* resolution of the game's image and antialiasing: the scene targets are rebuilt */
void gfx_set_scene(int render_scale, int msaa)
{
    D3D11_RASTERIZER_DESC rd;
    if (!g.ready || (render_scale == g.opt.render_scale && msaa == g.opt.msaa))
        return;
    flush_batch();
    gfx_wait_flip();
    g.opt.render_scale = render_scale;
    g.opt.msaa = msaa;
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
    release_scene();
    make_scene();
    REL(g.raster);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = msaa > 1;
    ID3D11Device_CreateRasterizerState(g.dev, &rd, &g.raster);
    g_depth_done[0] = g_depth_done[1] = 0;
    bind_scene();
}

/* RetroArch preset ("" = none) */
int gfx_set_shader(const char *path)
{
    if (!g.ready)
        return 0;
    flush_batch();
    gfx_wait_flip();
    strcpy_s(g.opt.shader, sizeof g.opt.shader, path ? path : "");
    return rshader_reload(g.dev, g.opt.game_dir, g.opt.shader);
}

void gfx_use_target(int t)
{
    if (!g.ready || t < 0 || t > 1)
        return;
    if (!g.tg_used[t]) {
        g.tg_used[t] = 1;
        if (!make_target(t)) {
            log_printf("gfx: second scene target failed");
            g.tg_used[t] = 0;
            return;
        }
    }
    g.cur = t;
    bind_scene();
}

/* Exclusive fullscreen ([video] exclusive=1): the picture goes straight to the
 * screen without the desktop compositor, which costs less GPU on some machines.
 * Taken while the game window is in front and given back when it isn't, so
 * alt-tab still works (the game keeps running in its borderless window). */
static void exclusive_follow_focus(void)
{
    BOOL fs = FALSE;
    int want;
    if (!g.opt.exclusive || g.opt.windowed)
        return;
    want = GetForegroundWindow() == g.hwnd && !IsIconic(g.hwnd);
    IDXGISwapChain1_GetFullscreenState(g.swap, &fs, NULL);
    if (!!fs == want)
        return;
    if (FAILED(IDXGISwapChain1_SetFullscreenState(g.swap, want, NULL))) {
        static int warned;
        if (!warned++)
            log_printf("gfx: exclusive fullscreen not available here, staying borderless");
        g.opt.exclusive = 0;
        return;
    }
    log_printf("gfx: exclusive fullscreen %s", want ? "on" : "off (game not in front)");
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
    release_back();
    make_back();
    if (!want)
        place_window();
}

/* The game draws some world surfaces (ground pieces, decals) testing depth
 * but not writing it, which left holes in the depth buffer that effects
 * tools such as ReShade read (the floor looked infinitely far away). The
 * effects depth target has every visible surface; at the end of the frame it
 * becomes the depth buffer. Nothing of ours reads depth after this point. */
void gfx_finish_depth(int t)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *none = NULL;
    if (!g.ready || t < 0 || t > 1 || g_depth_done[t] || !g.tg[t].eff_srv || !g.depth_copy_ps || !g.ds_always)
        return;
    flush_batch();
    g_depth_done[t] = 1;
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, g.tg[t].dsv);
    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (float)g.scene_w;
    vp.Height = (float)g.scene_h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(g.ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(g.ctx, g.ds_always, 0);
    ID3D11DeviceContext_IASetInputLayout(g.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g.ctx, g.blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g.ctx, g.depth_copy_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &g.tg[t].eff_srv);
    ID3D11DeviceContext_Draw(g.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &none);
    bind_scene();
}

void gfx_compose_target(int t)
{
    static const float black[4] = { 0, 0, 0, 1 };
    D3D11_VIEWPORT vp;
    RECT rc;
    ID3D11ShaderResourceView *none = NULL;
    double sx, sy, s;
    if (!g.ready)
        return;
    flush_batch();
    gfx_wait_flip();   /* the back buffer may still hold a frame waiting for its moment */
    if (t < 0 || t > 1 || !g.tg_used[t])
        t = 0;
    gfx_finish_depth(t);
    exclusive_follow_focus();
    if (g.tg[t].scene_resolved)
        ID3D11DeviceContext_ResolveSubresource(g.ctx, (ID3D11Resource *)g.tg[t].scene_resolved, 0,
                                               (ID3D11Resource *)g.tg[t].scene, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    /* follow the window size */
    GetClientRect(g.hwnd, &rc);
    if (rc.right > 0 && rc.bottom > 0 && (rc.right != g.back_w || rc.bottom != g.back_h)) {
        int ow, oh, nw, nh;
        scene_size(&ow, &oh);
        ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
        release_back();
        make_back();
        scene_size(&nw, &nh);
        if (nw != ow || nh != oh) {
            release_scene();
            make_scene();
        }
    }
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 1, &g.back_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g.ctx, g.back_rtv, black);
    /* keep the game's aspect (pillar/letterbox) unless told to stretch */
    sx = (double)g.back_w / g.app_w;
    sy = (double)g.back_h / g.app_h;
    s = sx < sy ? sx : sy;
    vp.Width = g.opt.stretch ? (float)g.back_w : (float)(g.app_w * s);
    vp.Height = g.opt.stretch ? (float)g.back_h : (float)(g.app_h * s);
    vp.TopLeftX = (g.back_w - vp.Width) / 2;
    vp.TopLeftY = (g.back_h - vp.Height) / 2;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(g.ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(g.ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g.ctx, g.blit_vs, NULL, 0);
    if (g.opt.show_depth && g.tg[t].eff_srv) {
        ID3D11DeviceContext_PSSetShader(g.ctx, g.show_depth_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &g.tg[t].eff_srv);
    } else {
        ID3D11DeviceContext_PSSetShader(g.ctx, g.blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &g.tg[t].scene_srv);
    }
    ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 1, g.opt.nearest_upscale ? &g.smp_blit_point : &g.smp_blit_linear);
    if (g.opt.show_depth || !rshader_active() ||
        !rshader_frame(g.ctx, g.tg[t].scene_srv, g.back_rtv, vp.TopLeftX, vp.TopLeftY, (unsigned)vp.Width,
                       (unsigned)vp.Height))
        ID3D11DeviceContext_Draw(g.ctx, 3, 0);   /* plain upscale (or the shader failed this frame) */
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &none);
    gfx_ui_render(g.dev, g.ctx, g.back_rtv, g.back_w, g.back_h);   /* in-game menu, if open */
    bind_scene();
}

void gfx_present_target(int t)
{
    gfx_compose_target(t);
    if (g.ready)
        IDXGISwapChain1_Present(g.swap, g.opt.vsync ? 1 : 0, 0);
}
