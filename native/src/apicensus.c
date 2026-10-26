/* DirectDraw / Direct3D API census (dev tool, [debug] api_census=1).
 *
 * Counts every COM method the game calls on DirectDraw, its surfaces and
 * Direct3D, and logs the parameters that decide what a replacement renderer
 * has to support: surface descriptions, render/texture-stage states, lock and
 * blit flags, QueryInterface IIDs. Dumped to mml.log every 600 frames.
 *
 * How: DirectDrawCreate's import slot is redirected so the first object is
 * seen as it's made. Every vtable that shows up gets a counting thunk in each
 * slot (inc [counter]; jmp [original]); the methods that hand out new objects
 * or carry interesting arguments get a C wrapper instead, which also patches
 * the vtables of the objects they return. */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <initguid.h>   /* define the IIDs here instead of linking dxguid.lib */
#include <ddraw.h>
#include <d3d.h>
#include <stdint.h>
#include <string.h>

#include "apicensus.h"
#include "hook.h"
#include "log.h"

#define G_IAT_DIRECTDRAWCREATE 0x5083BCu

enum { I_DD, I_DD4, I_D3D3, I_SURF4, I_DEV3, I_VP3, I_TEX2, I_PAL, I_CLIP, I_SURF1, I_MAT3, I_COUNT };
static const char *const I_NAME[I_COUNT] = {
    "IDirectDraw", "IDirectDraw4", "IDirect3D3", "IDirectDrawSurface4", "IDirect3DDevice3",
    "IDirect3DViewport3", "IDirect3DTexture2", "IDirectDrawPalette", "IDirectDrawClipper",
    "IDirectDrawSurface(v1)", "IDirect3DMaterial3" };
static const int I_SLOTS[I_COUNT] = {
    sizeof(IDirectDrawVtbl) / 4, sizeof(IDirectDraw4Vtbl) / 4, sizeof(IDirect3D3Vtbl) / 4,
    sizeof(IDirectDrawSurface4Vtbl) / 4, sizeof(IDirect3DDevice3Vtbl) / 4,
    sizeof(IDirect3DViewport3Vtbl) / 4, sizeof(IDirect3DTexture2Vtbl) / 4,
    sizeof(IDirectDrawPaletteVtbl) / 4, sizeof(IDirectDrawClipperVtbl) / 4,
    sizeof(IDirectDrawSurfaceVtbl) / 4, sizeof(IDirect3DMaterial3Vtbl) / 4 };

#define MAX_SLOTS 64
#define MAX_VTBLS 32
typedef struct {
    void **vtbl;
    int iface;
    void *orig[MAX_SLOTS];
} vt_t;
static vt_t g_vt[MAX_VTBLS];
static int g_nvt;
static volatile LONG g_calls[I_COUNT][MAX_SLOTS];
static uint8_t *g_thunks;
static size_t g_thunk_used;
static int g_on;

static void *orig_of(int iface, int slot, void *obj)
{
    void **vtbl = *(void ***)obj;
    int i;
    for (i = 0; i < g_nvt; i++)
        if (g_vt[i].vtbl == vtbl && g_vt[i].iface == iface)
            return g_vt[i].orig[slot];
    return vtbl[slot];
}

#define CALL(iface, slot, type, obj) ((type)orig_of(iface, slot, obj))

/* ---- detail logging, bounded ---- */
static int g_detail_left = 160;
#define DETAIL(...) do { if (g_detail_left > 0) { g_detail_left--; log_printf(__VA_ARGS__); } } while (0)

typedef struct { DWORD state, value; unsigned n; } rs_t;
static rs_t g_rs[256], g_tss[256];
static int g_nrs, g_ntss;

static void note_state(rs_t *tab, int *n, DWORD state, DWORD value)
{
    int i;
    for (i = 0; i < *n; i++)
        if (tab[i].state == state && tab[i].value == value) {
            tab[i].n++;
            return;
        }
    if (*n < 256) {
        tab[*n].state = state;
        tab[*n].value = value;
        tab[*n].n = 1;
        (*n)++;
    }
}

static void patch_vtbl(void *obj, int iface);

/* ---- C wrappers (each also counts its slot) ---- */

#define COUNT(iface, slot) InterlockedIncrement(&g_calls[iface][slot])

static void log_iid(const char *who, REFIID r)
{
    DETAIL("  %s QI {%08lX-%04X-%04X-%02X%02X}", who, r->Data1, r->Data2, r->Data3, r->Data4[0], r->Data4[1]);
}

static int iface_of_iid(REFIID r)
{
    if (IsEqualIID(r, &IID_IDirectDraw4)) return I_DD4;
    if (IsEqualIID(r, &IID_IDirect3D3)) return I_D3D3;
    if (IsEqualIID(r, &IID_IDirectDrawSurface4)) return I_SURF4;
    if (IsEqualIID(r, &IID_IDirect3DTexture2)) return I_TEX2;
    if (IsEqualIID(r, &IID_IDirectDrawSurface)) return I_SURF1;
    return -1;
}

static HRESULT WINAPI w_dd_qi(IDirectDraw *self, REFIID r, void **out)
{
    HRESULT hr = CALL(I_DD, 0, HRESULT(WINAPI *)(IDirectDraw *, REFIID, void **), self)(self, r, out);
    COUNT(I_DD, 0);
    log_iid("IDirectDraw", r);
    if (SUCCEEDED(hr) && out && *out && iface_of_iid(r) >= 0)
        patch_vtbl(*out, iface_of_iid(r));
    return hr;
}

static HRESULT WINAPI w_dd4_qi(IDirectDraw4 *self, REFIID r, void **out)
{
    HRESULT hr = CALL(I_DD4, 0, HRESULT(WINAPI *)(IDirectDraw4 *, REFIID, void **), self)(self, r, out);
    COUNT(I_DD4, 0);
    log_iid("IDirectDraw4", r);
    if (SUCCEEDED(hr) && out && *out && iface_of_iid(r) >= 0)
        patch_vtbl(*out, iface_of_iid(r));
    return hr;
}

static void log_desc(const char *what, const DDSURFACEDESC2 *d)
{
    if (!d)
        return;
    DETAIL("  %s: flags %08lX caps %08lX/%08lX %lux%lu backbuffers %lu pf flags %08lX bpp %lu masks R%08lX G%08lX B%08lX A%08lX ckey %08lX..%08lX",
           what, d->dwFlags, d->ddsCaps.dwCaps, d->ddsCaps.dwCaps2, d->dwWidth, d->dwHeight, d->dwBackBufferCount,
           d->ddpfPixelFormat.dwFlags, d->ddpfPixelFormat.dwRGBBitCount, d->ddpfPixelFormat.dwRBitMask,
           d->ddpfPixelFormat.dwGBitMask, d->ddpfPixelFormat.dwBBitMask, d->ddpfPixelFormat.dwRGBAlphaBitMask,
           d->ddckCKSrcBlt.dwColorSpaceLowValue, d->ddckCKSrcBlt.dwColorSpaceHighValue);
}

static HRESULT WINAPI w_dd4_create_surface(IDirectDraw4 *self, LPDDSURFACEDESC2 d, LPDIRECTDRAWSURFACE4 *out, IUnknown *u)
{
    HRESULT hr = CALL(I_DD4, 6, HRESULT(WINAPI *)(IDirectDraw4 *, LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE4 *, IUnknown *), self)(self, d, out, u);
    COUNT(I_DD4, 6);
    log_desc(SUCCEEDED(hr) ? "CreateSurface" : "CreateSurface FAILED", d);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_SURF4);
    return hr;
}

static HRESULT WINAPI w_dd4_create_palette(IDirectDraw4 *self, DWORD f, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE *out, IUnknown *u)
{
    HRESULT hr = CALL(I_DD4, 5, HRESULT(WINAPI *)(IDirectDraw4 *, DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE *, IUnknown *), self)(self, f, e, out, u);
    COUNT(I_DD4, 5);
    DETAIL("  CreatePalette flags %08lX", f);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_PAL);
    return hr;
}

static HRESULT WINAPI w_dd4_create_clipper(IDirectDraw4 *self, DWORD f, LPDIRECTDRAWCLIPPER *out, IUnknown *u)
{
    HRESULT hr = CALL(I_DD4, 4, HRESULT(WINAPI *)(IDirectDraw4 *, DWORD, LPDIRECTDRAWCLIPPER *, IUnknown *), self)(self, f, out, u);
    COUNT(I_DD4, 4);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_CLIP);
    return hr;
}

static HRESULT WINAPI w_dd4_set_display_mode(IDirectDraw4 *self, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD f)
{
    HRESULT hr = CALL(I_DD4, 21, HRESULT(WINAPI *)(IDirectDraw4 *, DWORD, DWORD, DWORD, DWORD, DWORD), self)(self, w, h, bpp, rate, f);
    COUNT(I_DD4, 21);
    DETAIL("  SetDisplayMode %lux%lux%lu rate %lu flags %lX -> %08lX", w, h, bpp, rate, f, (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI w_dd4_set_coop(IDirectDraw4 *self, HWND wnd, DWORD f)
{
    HRESULT hr = CALL(I_DD4, 20, HRESULT(WINAPI *)(IDirectDraw4 *, HWND, DWORD), self)(self, wnd, f);
    COUNT(I_DD4, 20);
    DETAIL("  SetCooperativeLevel hwnd %p flags %08lX", (void *)wnd, f);
    return hr;
}

static HRESULT WINAPI w_d3d3_create_device(IDirect3D3 *self, REFCLSID c, LPDIRECTDRAWSURFACE4 s, LPDIRECT3DDEVICE3 *out, LPUNKNOWN u)
{
    HRESULT hr = CALL(I_D3D3, 8, HRESULT(WINAPI *)(IDirect3D3 *, REFCLSID, LPDIRECTDRAWSURFACE4, LPDIRECT3DDEVICE3 *, LPUNKNOWN), self)(self, c, s, out, u);
    COUNT(I_D3D3, 8);
    DETAIL("  CreateDevice {%08lX} -> %08lX", c->Data1, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_DEV3);
    return hr;
}

static HRESULT WINAPI w_d3d3_create_viewport(IDirect3D3 *self, LPDIRECT3DVIEWPORT3 *out, IUnknown *u)
{
    HRESULT hr = CALL(I_D3D3, 6, HRESULT(WINAPI *)(IDirect3D3 *, LPDIRECT3DVIEWPORT3 *, IUnknown *), self)(self, out, u);
    COUNT(I_D3D3, 6);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_VP3);
    return hr;
}

static HRESULT WINAPI w_surf_qi(IDirectDrawSurface4 *self, REFIID r, void **out)
{
    HRESULT hr = CALL(I_SURF4, 0, HRESULT(WINAPI *)(IDirectDrawSurface4 *, REFIID, void **), self)(self, r, out);
    COUNT(I_SURF4, 0);
    log_iid("IDirectDrawSurface4", r);
    if (SUCCEEDED(hr) && out && *out && iface_of_iid(r) >= 0)
        patch_vtbl(*out, iface_of_iid(r));
    return hr;
}

static HRESULT WINAPI w_surf_get_attached(IDirectDrawSurface4 *self, LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE4 *out)
{
    HRESULT hr = CALL(I_SURF4, 12, HRESULT(WINAPI *)(IDirectDrawSurface4 *, LPDDSCAPS2, LPDIRECTDRAWSURFACE4 *), self)(self, caps, out);
    COUNT(I_SURF4, 12);
    DETAIL("  GetAttachedSurface caps %08lX", caps ? caps->dwCaps : 0);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_SURF4);
    return hr;
}

static HRESULT WINAPI w_surf_lock(IDirectDrawSurface4 *self, LPRECT rc, LPDDSURFACEDESC2 d, DWORD f, HANDLE h)
{
    HRESULT hr = CALL(I_SURF4, 25, HRESULT(WINAPI *)(IDirectDrawSurface4 *, LPRECT, LPDDSURFACEDESC2, DWORD, HANDLE), self)(self, rc, d, f, h);
    COUNT(I_SURF4, 25);
    if (SUCCEEDED(hr) && d)
        DETAIL("  Lock flags %08lX rect %s -> %lux%lu pitch %ld bpp %lu caps %08lX", f, rc ? "yes" : "whole",
               d->dwWidth, d->dwHeight, d->lPitch, d->ddpfPixelFormat.dwRGBBitCount, d->ddsCaps.dwCaps);
    return hr;
}

static HRESULT WINAPI w_surf_blt(IDirectDrawSurface4 *self, LPRECT dr, LPDIRECTDRAWSURFACE4 src, LPRECT sr, DWORD f, LPDDBLTFX fx)
{
    HRESULT hr = CALL(I_SURF4, 5, HRESULT(WINAPI *)(IDirectDrawSurface4 *, LPRECT, LPDIRECTDRAWSURFACE4, LPRECT, DWORD, LPDDBLTFX), self)(self, dr, src, sr, f, fx);
    COUNT(I_SURF4, 5);
    DETAIL("  Blt flags %08lX src %s dst %s fill %08lX", f, src ? "surface" : "none",
           dr ? "rect" : "whole", fx ? fx->dwFillColor : 0);
    return hr;
}

static HRESULT WINAPI w_surf_get_dc(IDirectDrawSurface4 *self, HDC *dc)
{
    HRESULT hr = CALL(I_SURF4, 17, HRESULT(WINAPI *)(IDirectDrawSurface4 *, HDC *), self)(self, dc);
    COUNT(I_SURF4, 17);
    DETAIL("  GetDC (GDI drawing onto a surface)");
    return hr;
}

static HRESULT WINAPI w_surf_set_color_key(IDirectDrawSurface4 *self, DWORD f, LPDDCOLORKEY k)
{
    COUNT(I_SURF4, 29);
    DETAIL("  SetColorKey flags %08lX key %08lX..%08lX", f, k ? k->dwColorSpaceLowValue : 0, k ? k->dwColorSpaceHighValue : 0);
    return CALL(I_SURF4, 29, HRESULT(WINAPI *)(IDirectDrawSurface4 *, DWORD, LPDDCOLORKEY), self)(self, f, k);
}

static HRESULT WINAPI w_dd_create_clipper(IDirectDraw *self, DWORD f, LPDIRECTDRAWCLIPPER *out, IUnknown *u)
{
    HRESULT hr = CALL(I_DD, 4, HRESULT(WINAPI *)(IDirectDraw *, DWORD, LPDIRECTDRAWCLIPPER *, IUnknown *), self)(self, f, out, u);
    COUNT(I_DD, 4);
    DETAIL("  IDirectDraw::CreateClipper flags %08lX", f);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_CLIP);
    return hr;
}

static HRESULT WINAPI w_d3d3_create_material(IDirect3D3 *self, LPDIRECT3DMATERIAL3 *out, IUnknown *u)
{
    HRESULT hr = CALL(I_D3D3, 5, HRESULT(WINAPI *)(IDirect3D3 *, LPDIRECT3DMATERIAL3 *, IUnknown *), self)(self, out, u);
    COUNT(I_D3D3, 5);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_MAT3);
    return hr;
}

static HRESULT WINAPI w_mat_set(IDirect3DMaterial3 *self, LPD3DMATERIAL m)
{
    COUNT(I_MAT3, 3);
    if (m)
        DETAIL("  Material SetMaterial diffuse %.3f %.3f %.3f %.3f", m->diffuse.r, m->diffuse.g, m->diffuse.b, m->diffuse.a);
    return CALL(I_MAT3, 3, HRESULT(WINAPI *)(IDirect3DMaterial3 *, LPD3DMATERIAL), self)(self, m);
}

typedef struct { DWORD type, fvf, flags; unsigned n; } dp_t;
static dp_t g_dp[32];
static int g_ndp;

static HRESULT WINAPI w_dev_set_rs(IDirect3DDevice3 *self, D3DRENDERSTATETYPE s, DWORD v)
{
    COUNT(I_DEV3, 22);
    note_state(g_rs, &g_nrs, (DWORD)s, v);
    return CALL(I_DEV3, 22, HRESULT(WINAPI *)(IDirect3DDevice3 *, D3DRENDERSTATETYPE, DWORD), self)(self, s, v);
}

static HRESULT WINAPI w_dev_set_tss(IDirect3DDevice3 *self, DWORD stage, D3DTEXTURESTAGESTATETYPE s, DWORD v)
{
    COUNT(I_DEV3, 40);
    note_state(g_tss, &g_ntss, (stage << 16) | (DWORD)s, v);
    return CALL(I_DEV3, 40, HRESULT(WINAPI *)(IDirect3DDevice3 *, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD), self)(self, stage, s, v);
}

static HRESULT WINAPI w_dev_set_texture(IDirect3DDevice3 *self, DWORD stage, LPDIRECT3DTEXTURE2 t)
{
    COUNT(I_DEV3, 38);
    if (t)
        patch_vtbl(t, I_TEX2);
    return CALL(I_DEV3, 38, HRESULT(WINAPI *)(IDirect3DDevice3 *, DWORD, LPDIRECT3DTEXTURE2), self)(self, stage, t);
}

static HRESULT WINAPI w_dev_draw_prim(IDirect3DDevice3 *self, D3DPRIMITIVETYPE pt, DWORD fvf, LPVOID v, DWORD n, DWORD f)
{
    int i;
    COUNT(I_DEV3, 28);
    for (i = 0; i < g_ndp; i++)
        if (g_dp[i].type == (DWORD)pt && g_dp[i].fvf == fvf && g_dp[i].flags == f)
            break;
    if (i == g_ndp && g_ndp < 32) {
        g_dp[i].type = pt; g_dp[i].fvf = fvf; g_dp[i].flags = f; g_dp[i].n = 0;
        g_ndp++;
    }
    if (i < g_ndp)
        g_dp[i].n++;
    (void)n;
    return CALL(I_DEV3, 28, HRESULT(WINAPI *)(IDirect3DDevice3 *, D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD), self)(self, pt, fvf, v, n, f);
}

/* slot -> wrapper, per interface */
typedef struct { int iface, slot; void *fn; } wrap_t;
static const wrap_t g_wraps[] = {
    { I_DD, 0, (void *)w_dd_qi }, { I_DD, 4, (void *)w_dd_create_clipper },
    { I_D3D3, 5, (void *)w_d3d3_create_material }, { I_MAT3, 3, (void *)w_mat_set },
    { I_SURF4, 29, (void *)w_surf_set_color_key },
    { I_DD4, 0, (void *)w_dd4_qi }, { I_DD4, 4, (void *)w_dd4_create_clipper },
    { I_DD4, 5, (void *)w_dd4_create_palette }, { I_DD4, 6, (void *)w_dd4_create_surface },
    { I_DD4, 20, (void *)w_dd4_set_coop }, { I_DD4, 21, (void *)w_dd4_set_display_mode },
    { I_D3D3, 6, (void *)w_d3d3_create_viewport }, { I_D3D3, 8, (void *)w_d3d3_create_device },
    { I_SURF4, 0, (void *)w_surf_qi }, { I_SURF4, 5, (void *)w_surf_blt },
    { I_SURF4, 12, (void *)w_surf_get_attached }, { I_SURF4, 17, (void *)w_surf_get_dc },
    { I_SURF4, 25, (void *)w_surf_lock },
    { I_DEV3, 22, (void *)w_dev_set_rs }, { I_DEV3, 28, (void *)w_dev_draw_prim },
    { I_DEV3, 38, (void *)w_dev_set_texture }, { I_DEV3, 40, (void *)w_dev_set_tss },
};

static uint8_t *thunk_alloc(size_t n)
{
    uint8_t *p;
    if (!g_thunks)
        g_thunks = VirtualAlloc(NULL, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_thunks || g_thunk_used + n > 0x20000)
        return NULL;
    p = g_thunks + g_thunk_used;
    g_thunk_used += n;
    return p;
}

static void patch_vtbl(void *obj, int iface)
{
    void **vtbl = *(void ***)obj;
    vt_t *v;
    int i, s, n = I_SLOTS[iface];
    for (i = 0; i < g_nvt; i++)
        if (g_vt[i].vtbl == vtbl)
            return;   /* already instrumented (one vtable per class) */
    if (g_nvt >= MAX_VTBLS || n > MAX_SLOTS)
        return;
    v = &g_vt[g_nvt++];
    v->vtbl = vtbl;
    v->iface = iface;
    for (s = 0; s < n; s++) {
        void *repl = NULL;
        size_t w;
        v->orig[s] = vtbl[s];
        for (w = 0; w < sizeof g_wraps / sizeof g_wraps[0]; w++)
            if (g_wraps[w].iface == iface && g_wraps[w].slot == s)
                repl = g_wraps[w].fn;
        if (!repl) {
            /* inc dword [counter] ; jmp dword [orig] */
            uint8_t *t = thunk_alloc(12);
            uint32_t ctr = (uint32_t)(uintptr_t)&g_calls[iface][s], org = (uint32_t)(uintptr_t)&v->orig[s];
            if (!t)
                return;
            t[0] = 0xFF; t[1] = 0x05; memcpy(t + 2, &ctr, 4);
            t[6] = 0xFF; t[7] = 0x25; memcpy(t + 8, &org, 4);
            repl = t;
        }
        mem_write((uintptr_t)&vtbl[s], &repl, sizeof repl);
    }
    log_printf("apicensus: instrumented %s vtable %p (%d slots)", I_NAME[iface], (void *)vtbl, n);
}

static HRESULT(WINAPI *g_real_ddc)(GUID *, LPDIRECTDRAW *, IUnknown *);

static HRESULT WINAPI w_direct_draw_create(GUID *g, LPDIRECTDRAW *out, IUnknown *u)
{
    HRESULT hr = g_real_ddc(g, out, u);
    log_printf("apicensus: DirectDrawCreate(%p) -> %08lX", (void *)g, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out)
        patch_vtbl(*out, I_DD);
    return hr;
}

void apicensus_install(void)
{
    void *mine = (void *)w_direct_draw_create;
    g_real_ddc = *(HRESULT(WINAPI **)(GUID *, LPDIRECTDRAW *, IUnknown *))G_IAT_DIRECTDRAWCREATE;
    mem_write(G_IAT_DIRECTDRAWCREATE, &mine, sizeof mine);
    g_on = 1;
    log_printf("apicensus: watching DirectDrawCreate");
}

void apicensus_frame(void)
{
    static unsigned frames;
    int i, s;
    if (!g_on || ++frames % 600 != 0)
        return;
    log_printf("apicensus after %u frames:", frames);
    for (i = 0; i < I_COUNT; i++)
        for (s = 0; s < I_SLOTS[i] && s < MAX_SLOTS; s++)
            if (g_calls[i][s])
                log_printf("  %-24s slot %2d  %ld", I_NAME[i], s, g_calls[i][s]);
    log_printf("  DrawPrimitive kinds (type fvf flags x count):");
    for (i = 0; i < g_ndp; i++)
        log_printf("    %lu %08lX %08lX x%u", g_dp[i].type, g_dp[i].fvf, g_dp[i].flags, g_dp[i].n);
    log_printf("  render states (state=value x count):");
    for (i = 0; i < g_nrs; i++)
        log_printf("    RS %3lu = %08lX  x%u", g_rs[i].state, g_rs[i].value, g_rs[i].n);
    log_printf("  texture stage states (stage:state=value x count):");
    for (i = 0; i < g_ntss; i++)
        log_printf("    TSS %lu:%2lu = %08lX  x%u", g_tss[i].state >> 16, g_tss[i].state & 0xFFFF, g_tss[i].value, g_tss[i].n);
}
