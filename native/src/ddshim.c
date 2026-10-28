/* DirectDraw 4 / Direct3D 3 implemented on top of gfx.h.
 *
 * Only what MEGAMAN.EXE actually uses (established with the API census, see
 * apicensus.c): a flip chain, a Z buffer, 16-bit textures written through
 * Lock/Unlock and copied with Blt, about fifteen render states, and
 * DrawPrimitive with screen-space vertices. Every other method exists with
 * the right argument count (stdcall: the callee pops its arguments, so a stub
 * with the wrong count would corrupt the stack) and returns DDERR_UNSUPPORTED,
 * logging its name the first time it's hit.
 *
 * The exe's imports of DirectDrawCreate / DirectDrawEnumerateA are pointed
 * here by ddshim_install(), so DDrawCompat (or system DirectDraw) is never
 * used for rendering. */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <initguid.h>
#include <ddraw.h>
#include <d3d.h>
#include <mmsystem.h>
#include <stdio.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ddshim.h"
#include "display.h"
#include "interp.h"
#include "frame_rec.h"
#include "clipfix.h"
#include "texreplace.h"
#include "bglayer.h"
#include "mml.h"
#include "gfx.h"
#include "hook.h"
#include "lockon.h"
#include "log.h"
#include "packets.h"

#define G_IAT_DIRECTDRAWCREATE    0x5083BCu
#define G_IAT_DIRECTDRAWENUMERATE 0x5083C0u
#define G_IAT_MCISENDSTRINGA      0x5083A4u

/* ------------------------------------------------------------------ stubs */

typedef void *P;
#define A1 P a
#define A2 P a, P b
#define A3 P a, P b, P c
#define A4 P a, P b, P c, P d
#define A5 P a, P b, P c, P d, P e
#define A6 P a, P b, P c, P d, P e, P f
#define A7 P a, P b, P c, P d, P e, P f, P g_
#define A8 P a, P b, P c, P d, P e, P f, P g_, P h

static void stub_hit(const char *name, volatile LONG *once)
{
    if (InterlockedExchange(once, 1) == 0)
        log_printf("ddshim: unimplemented %s called", name);
}

#define STUB_FN(iface, name, n) \
    static HRESULT WINAPI stub_##iface##_##name(A##n) { static volatile LONG once; \
        stub_hit(#iface "::" #name, &once); return DDERR_UNSUPPORTED; }
#define STUB_PTR(iface, name, n) (void *)stub_##iface##_##name,

/* Method lists in vtable order: X(name, argument count including this). */
#define IUNK(X) X(QueryInterface, 3) X(AddRef, 1) X(Release, 1)
#define DD1_METHODS(X) IUNK(X) X(Compact, 1) X(CreateClipper, 4) X(CreatePalette, 5) X(CreateSurface, 4) \
    X(DuplicateSurface, 3) X(EnumDisplayModes, 5) X(EnumSurfaces, 5) X(FlipToGDISurface, 1) X(GetCaps, 3) \
    X(GetDisplayMode, 2) X(GetFourCCCodes, 3) X(GetGDISurface, 2) X(GetMonitorFrequency, 2) X(GetScanLine, 2) \
    X(GetVerticalBlankStatus, 2) X(Initialize, 2) X(RestoreDisplayMode, 1) X(SetCooperativeLevel, 3) \
    X(SetDisplayMode, 4) X(WaitForVerticalBlank, 3)
#define DD4_METHODS(X) IUNK(X) X(Compact, 1) X(CreateClipper, 4) X(CreatePalette, 5) X(CreateSurface, 4) \
    X(DuplicateSurface, 3) X(EnumDisplayModes, 5) X(EnumSurfaces, 5) X(FlipToGDISurface, 1) X(GetCaps, 3) \
    X(GetDisplayMode, 2) X(GetFourCCCodes, 3) X(GetGDISurface, 2) X(GetMonitorFrequency, 2) X(GetScanLine, 2) \
    X(GetVerticalBlankStatus, 2) X(Initialize, 2) X(RestoreDisplayMode, 1) X(SetCooperativeLevel, 3) \
    X(SetDisplayMode, 6) X(WaitForVerticalBlank, 3) X(GetAvailableVidMem, 4) X(GetSurfaceFromDC, 3) \
    X(RestoreAllSurfaces, 1) X(TestCooperativeLevel, 1) X(GetDeviceIdentifier, 3)
#define D3D3_METHODS(X) IUNK(X) X(EnumDevices, 3) X(CreateLight, 3) X(CreateMaterial, 3) X(CreateViewport, 3) \
    X(FindDevice, 3) X(CreateDevice, 5) X(CreateVertexBuffer, 5) X(EnumZBufferFormats, 4) X(EvictManagedTextures, 1)
#define SURF_COMMON(X) IUNK(X) X(AddAttachedSurface, 2) X(AddOverlayDirtyRect, 2) X(Blt, 6) X(BltBatch, 4) \
    X(BltFast, 6) X(DeleteAttachedSurface, 3) X(EnumAttachedSurfaces, 3) X(EnumOverlayZOrders, 4) X(Flip, 3) \
    X(GetAttachedSurface, 3) X(GetBltStatus, 2) X(GetCaps, 2) X(GetClipper, 2) X(GetColorKey, 3) X(GetDC, 2) \
    X(GetFlipStatus, 2) X(GetOverlayPosition, 3) X(GetPalette, 2) X(GetPixelFormat, 2) X(GetSurfaceDesc, 2) \
    X(Initialize, 3) X(IsLost, 1) X(Lock, 5) X(ReleaseDC, 2) X(Restore, 1) X(SetClipper, 2) X(SetColorKey, 3) \
    X(SetOverlayPosition, 3) X(SetPalette, 2) X(Unlock, 2) X(UpdateOverlay, 6) X(UpdateOverlayDisplay, 2) \
    X(UpdateOverlayZOrder, 3)
#define SURF1_METHODS(X) SURF_COMMON(X)
#define SURF4_METHODS(X) SURF_COMMON(X) X(GetDDInterface, 2) X(PageLock, 2) X(PageUnlock, 2) X(SetSurfaceDesc, 3) \
    X(SetPrivateData, 5) X(GetPrivateData, 4) X(FreePrivateData, 2) X(GetUniquenessValue, 2) X(ChangeUniquenessValue, 1)
#define DEV3_METHODS(X) IUNK(X) X(GetCaps, 3) X(GetStats, 2) X(AddViewport, 2) X(DeleteViewport, 2) X(NextViewport, 4) \
    X(EnumTextureFormats, 3) X(BeginScene, 1) X(EndScene, 1) X(GetDirect3D, 2) X(SetCurrentViewport, 2) \
    X(GetCurrentViewport, 2) X(SetRenderTarget, 3) X(GetRenderTarget, 2) X(Begin, 4) X(BeginIndexed, 6) \
    X(Vertex, 2) X(Index, 2) X(End, 2) X(GetRenderState, 3) X(SetRenderState, 3) X(GetLightState, 3) \
    X(SetLightState, 3) X(SetTransform, 3) X(GetTransform, 3) X(MultiplyTransform, 3) X(DrawPrimitive, 6) \
    X(DrawIndexedPrimitive, 8) X(SetClipStatus, 2) X(GetClipStatus, 2) X(DrawPrimitiveStrided, 6) \
    X(DrawIndexedPrimitiveStrided, 8) X(DrawPrimitiveVB, 6) X(DrawIndexedPrimitiveVB, 6) \
    X(ComputeSphereVisibility, 6) X(GetTexture, 3) X(SetTexture, 3) X(GetTextureStageState, 4) \
    X(SetTextureStageState, 4) X(ValidateDevice, 2)
#define VP3_METHODS(X) IUNK(X) X(Initialize, 2) X(GetViewport, 2) X(SetViewport, 2) X(TransformVertices, 5) \
    X(LightElements, 3) X(SetBackground, 2) X(GetBackground, 3) X(SetBackgroundDepth, 2) X(GetBackgroundDepth, 3) \
    X(Clear, 4) X(AddLight, 2) X(DeleteLight, 2) X(NextLight, 4) X(GetViewport2, 2) X(SetViewport2, 2) \
    X(SetBackgroundDepth2, 2) X(GetBackgroundDepth2, 3) X(Clear2, 7)
#define MAT3_METHODS(X) IUNK(X) X(SetMaterial, 2) X(GetMaterial, 2) X(GetHandle, 3)
#define TEX2_METHODS(X) IUNK(X) X(GetHandle, 3) X(PaletteChanged, 3) X(Load, 2)
#define CLIP_METHODS(X) IUNK(X) X(GetClipList, 4) X(GetHWnd, 2) X(Initialize, 3) X(IsClipListChanged, 2) \
    X(SetClipList, 3) X(SetHWnd, 3)

#define STUBS_FOR(iface, LIST) \
    LIST(STUB_FN_##iface) static void *vt_##iface[] = { LIST(STUB_PTR_##iface) };
#define STUB_FN_dd1(n, c) STUB_FN(dd1, n, c)
#define STUB_PTR_dd1(n, c) STUB_PTR(dd1, n, c)
#define STUB_FN_dd4(n, c) STUB_FN(dd4, n, c)
#define STUB_PTR_dd4(n, c) STUB_PTR(dd4, n, c)
#define STUB_FN_d3d3(n, c) STUB_FN(d3d3, n, c)
#define STUB_PTR_d3d3(n, c) STUB_PTR(d3d3, n, c)
#define STUB_FN_surf1(n, c) STUB_FN(surf1, n, c)
#define STUB_PTR_surf1(n, c) STUB_PTR(surf1, n, c)
#define STUB_FN_surf4(n, c) STUB_FN(surf4, n, c)
#define STUB_PTR_surf4(n, c) STUB_PTR(surf4, n, c)
#define STUB_FN_dev3(n, c) STUB_FN(dev3, n, c)
#define STUB_PTR_dev3(n, c) STUB_PTR(dev3, n, c)
#define STUB_FN_vp3(n, c) STUB_FN(vp3, n, c)
#define STUB_PTR_vp3(n, c) STUB_PTR(vp3, n, c)
#define STUB_FN_mat3(n, c) STUB_FN(mat3, n, c)
#define STUB_PTR_mat3(n, c) STUB_PTR(mat3, n, c)
#define STUB_FN_tex2(n, c) STUB_FN(tex2, n, c)
#define STUB_PTR_tex2(n, c) STUB_PTR(tex2, n, c)
#define STUB_FN_clip(n, c) STUB_FN(clip, n, c)
#define STUB_PTR_clip(n, c) STUB_PTR(clip, n, c)

/* the generated stubs ignore their arguments by design */
#pragma warning(push)
#pragma warning(disable : 4100)
STUBS_FOR(dd1, DD1_METHODS)
STUBS_FOR(dd4, DD4_METHODS)
STUBS_FOR(d3d3, D3D3_METHODS)
STUBS_FOR(surf1, SURF1_METHODS)
STUBS_FOR(surf4, SURF4_METHODS)
STUBS_FOR(dev3, DEV3_METHODS)
STUBS_FOR(vp3, VP3_METHODS)
STUBS_FOR(mat3, MAT3_METHODS)
STUBS_FOR(tex2, TEX2_METHODS)
STUBS_FOR(clip, CLIP_METHODS)
#pragma warning(pop)

/* slot index of a method, from the same lists */
#define SLOT_ENUM(n, c) SLOT_##n,
enum { DD4_METHODS(SLOT_ENUM) DD4_SLOTS };
#undef SLOT_ENUM
#define SET(vt, slot, fn) ((vt)[slot] = (void *)(fn))

/* ------------------------------------------------------------------ objects */

typedef struct ddraw ddraw;
typedef struct surface surface;
typedef struct device device;
typedef struct viewport viewport;
typedef struct material material;

struct ddraw {
    void **vt1, **vt4, **vt3d;
    LONG ref;
    HWND hwnd;
    int mode_w, mode_h, mode_bpp;
};

enum { K_PRIMARY, K_BACK, K_ZBUF, K_TEXTURE, K_OFFSCREEN };

struct surface {
    void **vt4, **vt1, **vtex;
    LONG ref;
    ddraw *dd;
    int kind;
    DWORD caps, caps2;
    int w, h;
    DDPIXELFORMAT pf;
    uint8_t *mem;
    LONG pitch;
    surface *attached;     /* primary -> back buffer, back buffer -> Z */
    gfx_tex *tex;
    int dirty;
    unsigned used_frame;   /* interp census: last frame this texture was drawn with */
    DDCOLORKEY ck;
    int has_ck;
    surface *next, *prev;   /* every live surface (ddshim_reupload_textures) */
};
static surface *g_surfaces;

static unsigned g_frame_no = 1, g_midframe_uploads;
int g_interp_census;
int g_smooth_bg;   /* [video] smooth_sky: bglayer.c is installed */
int g_blend_census;   /* [debug] blend_census: what the 3D world draws semi-transparent */
static struct { uint8_t op, blend, src, dst; unsigned draws, verts; float z0, z1; } g_bc[32];
#define G_FRAME_STEP_ADDR 0x77EAC1u   /* byte: 60 Hz ticks per game frame (2 = 30 fps) */
#define G_GTE_NEAR_ADDR  0x77EBC0u
#define G_GTE_H_ADDR     0x77EBDCu
#define G_GTE_OFX_ADDR   0x77EBCCu
#define G_GTE_OFY_ADDR   0x77EBD0u
static unsigned g_iv_exact, g_iv_borrowed, g_iv_static;

struct device {
    void **vt;
    LONG ref;
    ddraw *dd;
    surface *target;
    viewport *vp;
    surface *tex;
    DWORD rs[256];
    DWORD tss[2][32];
};

struct viewport {
    void **vt;
    LONG ref;
    D3DVIEWPORT2 v;
    D3DMATERIALHANDLE bg;
};

struct material {
    void **vt;
    LONG ref;
    D3DMATERIAL m;
};

typedef struct {
    void **vt;
    LONG ref;
} clipper;

#define FROM(type, field, p) ((type *)((uint8_t *)(p) - offsetof(type, field)))

static gfx_options g_opt;
static int g_installed;

/* ------------------------------------------------------------------ helpers */

static uint32_t mask_shift(DWORD m)
{
    uint32_t s = 0;
    if (!m)
        return 0;
    while (!(m & 1)) { m >>= 1; s++; }
    return s;
}

static uint32_t mask_bits(DWORD m)
{
    uint32_t n = 0;
    while (m) { n += m & 1; m >>= 1; }
    return n;
}

static uint32_t expand(uint32_t v, DWORD mask)
{
    uint32_t bits = mask_bits(mask), x;
    if (!bits)
        return 255;
    x = (v & mask) >> mask_shift(mask);
    return bits >= 8 ? x >> (bits - 8) : (x * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
}

/* convert the CPU copy to RGBA8 and send it to the GPU */
static void upload(surface *s)
{
    uint32_t *rgba;
    int x, y, bpp = s->pf.dwRGBBitCount / 8;
    DWORD rm = s->pf.dwRBitMask, gm = s->pf.dwGBitMask, bm = s->pf.dwBBitMask, am = s->pf.dwRGBAlphaBitMask;
    int has_alpha = (s->pf.dwFlags & DDPF_ALPHAPIXELS) && am;
    const uint32_t *rep;
    int up_w, up_h, tw, th;
    if (!s->mem || bpp < 2)
        return;
    rgba = (uint32_t *)malloc((size_t)s->w * s->h * 4);
    if (!rgba)
        return;
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = s->mem + (size_t)y * s->pitch;
        for (x = 0; x < s->w; x++) {
            uint32_t v = bpp == 2 ? *(const uint16_t *)(row + x * 2) : *(const uint32_t *)(row + x * bpp);
            uint32_t a = has_alpha ? expand(v, am) : 255;
            if (s->has_ck && v >= s->ck.dwColorSpaceLowValue && v <= s->ck.dwColorSpaceHighValue)
                a = 0;
            rgba[(size_t)y * s->w + x] = expand(v, rm) | (expand(v, gm) << 8) | (expand(v, bm) << 16) | (a << 24);
        }
    }
    /* an HD replacement (any size) for this exact texture, if there is one */
    rep = texreplace_filter(rgba, s->w, s->h, &up_w, &up_h);
    if (!rep) {
        rep = rgba;
        up_w = s->w;
        up_h = s->h;
    }
    gfx_tex_size(s->tex, &tw, &th);
    if (s->tex && (tw != up_w || th != up_h)) {
        rec_tex_destroy(s->tex);   /* a recorded frame may still draw with it */
        s->tex = NULL;
    }
    if (!s->tex)
        s->tex = gfx_tex_create(up_w, up_h);
    if (s->tex)
        gfx_tex_upload(s->tex, rep, up_w, up_h);
    free(rgba);
    s->dirty = 0;
    if (s->used_frame == g_frame_no)
        g_midframe_uploads++;   /* drawn with already this frame, now changed */
}

static void set_pf_1555(DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0x7C00;
    pf->dwGBitMask = 0x03E0;
    pf->dwBBitMask = 0x001F;
    pf->dwRGBAlphaBitMask = 0x8000;
}

static void set_pf_565(DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0xF800;
    pf->dwGBitMask = 0x07E0;
    pf->dwBBitMask = 0x001F;
}

static void set_pf_z16(DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_ZBUFFER;
    pf->dwZBufferBitDepth = 16;
    pf->dwZBitMask = 0xFFFF;
}

static void ensure_gfx(ddraw *dd)
{
    if (!gfx_ready() && dd->hwnd) {
        /* DDSCL_FPUSETUP: the game asked Direct3D for single-precision x87, as it always got */
        unsigned int cw;
        _controlfp_s(&cw, _PC_24, _MCW_PC);
        if (!gfx_init(dd->hwnd, dd->mode_w ? dd->mode_w : 640, dd->mode_h ? dd->mode_h : 480, &g_opt))
            log_printf("ddshim: renderer failed to start");
    }
}

/* ------------------------------------------------------------------ IUnknown helpers */

static ULONG add_ref(LONG *r) { return (ULONG)InterlockedIncrement(r); }

/* ------------------------------------------------------------------ surfaces */

static surface *surf_new(ddraw *dd, int kind, int w, int h, const DDPIXELFORMAT *pf, DWORD caps, DWORD caps2);

static HRESULT WINAPI surf_qi(surface *s, REFIID r, void **out)
{
    if (!out)
        return E_POINTER;
    if (IsEqualIID(r, &IID_IDirectDrawSurface4) || IsEqualIID(r, &IID_IUnknown))
        *out = &s->vt4;
    else if (IsEqualIID(r, &IID_IDirectDrawSurface) || IsEqualIID(r, &IID_IDirectDrawSurface2) ||
             IsEqualIID(r, &IID_IDirectDrawSurface3))
        *out = &s->vt1;
    else if (IsEqualIID(r, &IID_IDirect3DTexture2) || IsEqualIID(r, &IID_IDirect3DTexture))
        *out = &s->vtex;
    else {
        *out = NULL;
        return E_NOINTERFACE;
    }
    add_ref(&s->ref);
    return S_OK;
}

static ULONG surf_release_obj(surface *s)
{
    LONG n = InterlockedDecrement(&s->ref);
    if (n == 0) {
        if (s->kind == K_PRIMARY && s->attached)
            surf_release_obj(s->attached);
        rec_tex_destroy(s->tex);
        if (s->prev)
            s->prev->next = s->next;
        else
            g_surfaces = s->next;
        if (s->next)
            s->next->prev = s->prev;
        free(s->mem);
        free(s);
    }
    return (ULONG)(n < 0 ? 0 : n);
}

/* the three interfaces of a surface share one object */
static HRESULT WINAPI s4_qi(void **i, REFIID r, void **o) { return surf_qi(FROM(surface, vt4, i), r, o); }
static ULONG WINAPI s4_addref(void **i) { return add_ref(&FROM(surface, vt4, i)->ref); }
static ULONG WINAPI s4_release(void **i) { return surf_release_obj(FROM(surface, vt4, i)); }
static HRESULT WINAPI s1_qi(void **i, REFIID r, void **o) { return surf_qi(FROM(surface, vt1, i), r, o); }
static ULONG WINAPI s1_addref(void **i) { return add_ref(&FROM(surface, vt1, i)->ref); }
static ULONG WINAPI s1_release(void **i) { return surf_release_obj(FROM(surface, vt1, i)); }
static HRESULT WINAPI t2_qi(void **i, REFIID r, void **o) { return surf_qi(FROM(surface, vtex, i), r, o); }
static ULONG WINAPI t2_addref(void **i) { return add_ref(&FROM(surface, vtex, i)->ref); }
static ULONG WINAPI t2_release(void **i) { return surf_release_obj(FROM(surface, vtex, i)); }

static void fill_desc(surface *s, DDSURFACEDESC2 *d)
{
    DWORD size = d->dwSize ? d->dwSize : sizeof *d;
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
    d->dwWidth = s->w;
    d->dwHeight = s->h;
    d->lPitch = s->pitch;
    d->ddpfPixelFormat = s->pf;
    d->ddsCaps.dwCaps = s->caps;
    if (size >= sizeof(DDSURFACEDESC2))
        d->ddsCaps.dwCaps2 = s->caps2;
    if (s->kind == K_PRIMARY) {
        d->dwFlags |= DDSD_BACKBUFFERCOUNT;
        d->dwBackBufferCount = 1;
    }
    if (s->has_ck) {
        d->dwFlags |= DDSD_CKSRCBLT;
        d->ddckCKSrcBlt = s->ck;
    }
}

static HRESULT WINAPI s_add_attached(void **i, void **att)
{
    surface *s = FROM(surface, vt4, i), *z = att ? FROM(surface, vt4, att) : NULL;
    if (z && z->kind == K_ZBUF) {
        if (s->attached != z) {
            add_ref(&z->ref);
            s->attached = z;
        }
    }
    return DD_OK;
}

static HRESULT WINAPI s_get_attached(void **i, LPDDSCAPS2 caps, void **out)
{
    surface *s = FROM(surface, vt4, i), *a = s->attached;
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (!caps || !a)
        return DDERR_NOTFOUND;
    if ((caps->dwCaps & DDSCAPS_BACKBUFFER) && a->kind == K_BACK) {
        add_ref(&a->ref);
        *out = &a->vt4;
        return DD_OK;
    }
    if ((caps->dwCaps & DDSCAPS_ZBUFFER) && a->kind == K_ZBUF) {
        add_ref(&a->ref);
        *out = &a->vt4;
        return DD_OK;
    }
    if (s->kind == K_PRIMARY && (caps->dwCaps & DDSCAPS_ZBUFFER) && a->attached) {
        add_ref(&a->attached->ref);
        *out = &a->attached->vt4;
        return DD_OK;
    }
    return DDERR_NOTFOUND;
}

static HRESULT WINAPI s_get_attached_v1(void **i, LPDDSCAPS caps, void **out)
{
    DDSCAPS2 c2;
    HRESULT hr;
    surface *s = FROM(surface, vt1, i);
    memset(&c2, 0, sizeof c2);
    c2.dwCaps = caps ? caps->dwCaps : 0;
    hr = s_get_attached((void **)&s->vt4, &c2, out);
    if (SUCCEEDED(hr))
        *out = &FROM(surface, vt4, *out)->vt1;
    return hr;
}

static HRESULT WINAPI s_lock(void **i, LPRECT rc, LPDDSURFACEDESC2 d, DWORD flags, HANDLE ev)
{
    surface *s = FROM(surface, vt4, i);
    (void)flags; (void)ev;
    if (!d)
        return DDERR_INVALIDPARAMS;
    if (!s->mem)
        return DDERR_UNSUPPORTED;
    fill_desc(s, d);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = s->mem;
    if (rc)
        d->lpSurface = s->mem + (size_t)rc->top * s->pitch + (size_t)rc->left * (s->pf.dwRGBBitCount / 8);
    return DD_OK;
}

static HRESULT WINAPI s_unlock(void **i, LPRECT rc)
{
    surface *s = FROM(surface, vt4, i);
    (void)rc;
    if (s->kind == K_TEXTURE || s->kind == K_OFFSCREEN)
        s->dirty = 1;
    return DD_OK;
}

static HRESULT WINAPI s_unlock_v1(void **i, LPVOID p)
{
    (void)p;
    return s_unlock((void **)&FROM(surface, vt1, i)->vt4, NULL);
}

/* plain memory copy between same-format surfaces (the game copies whole
 * textures from system memory to "video memory") */
static void copy_rect(surface *dst, const RECT *dr, surface *src, const RECT *sr)
{
    RECT d = { 0, 0, dst->w, dst->h }, s = { 0, 0, src->w, src->h };
    int bpp = dst->pf.dwRGBBitCount / 8, y, x, w, h;
    if (dr) d = *dr;
    if (sr) s = *sr;
    if (!dst->mem || !src->mem || bpp != (int)(src->pf.dwRGBBitCount / 8))
        return;
    w = d.right - d.left;
    h = d.bottom - d.top;
    if (w == s.right - s.left && h == s.bottom - s.top) {
        for (y = 0; y < h; y++)
            memcpy(dst->mem + (size_t)(d.top + y) * dst->pitch + (size_t)d.left * bpp,
                   src->mem + (size_t)(s.top + y) * src->pitch + (size_t)s.left * bpp, (size_t)w * bpp);
    } else {   /* stretch, nearest */
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int sx = s.left + x * (s.right - s.left) / w, sy = s.top + y * (s.bottom - s.top) / h;
                memcpy(dst->mem + (size_t)(d.top + y) * dst->pitch + (size_t)(d.left + x) * bpp,
                       src->mem + (size_t)sy * src->pitch + (size_t)sx * bpp, bpp);
            }
    }
    dst->dirty = 1;
}

static HRESULT WINAPI s_blt(void **i, LPRECT dr, void **srci, LPRECT sr, DWORD flags, LPDDBLTFX fx)
{
    surface *s = FROM(surface, vt4, i), *src = srci ? FROM(surface, vt4, srci) : NULL;
    if (flags & DDBLT_COLORFILL) {
        if (s->kind == K_BACK || s->kind == K_PRIMARY) {
            uint32_t c = fx ? fx->dwFillColor : 0;
            /* 16-bit 565 fill colour to 0xRRGGBB */
            uint32_t rgb = ((c >> 11 & 0x1F) * 255 / 31) << 16 | ((c >> 5 & 0x3F) * 255 / 63) << 8 | (c & 0x1F) * 255 / 31;
            ensure_gfx(s->dd);
            rec_clear(dr, 1, rgb, 0, 1.0f);
        } else if (s->mem) {
            int x, y, bpp = s->pf.dwRGBBitCount / 8;
            RECT r = { 0, 0, s->w, s->h };
            if (dr) r = *dr;
            for (y = r.top; y < r.bottom; y++)
                for (x = r.left; x < r.right; x++)
                    memcpy(s->mem + (size_t)y * s->pitch + (size_t)x * bpp, &fx->dwFillColor, bpp);
            s->dirty = 1;
        }
        return DD_OK;
    }
    if (flags & DDBLT_DEPTHFILL) {
        ensure_gfx(s->dd);
        rec_clear(dr, 0, 0, 1, 1.0f);
        return DD_OK;
    }
    if (s->kind == K_PRIMARY && src && src->kind == K_BACK) {
        /* the fades present this way instead of Flip: each frame dims the
         * last one without clearing it, so it must reach the screen. No
         * in-between frame: its target doesn't hold the picture being dimmed */
        ensure_gfx(s->dd);
        rec_present(1);
        return DD_OK;
    }
    if (src)
        copy_rect(s, dr, src, sr);
    return DD_OK;
}

static HRESULT WINAPI s_bltfast(void **i, DWORD x, DWORD y, void **srci, LPRECT sr, DWORD flags)
{
    surface *s = FROM(surface, vt4, i), *src = srci ? FROM(surface, vt4, srci) : NULL;
    RECT dr, srr;
    (void)flags;
    if (!src)
        return DDERR_INVALIDPARAMS;
    srr.left = 0; srr.top = 0; srr.right = src->w; srr.bottom = src->h;
    if (sr) srr = *sr;
    dr.left = x; dr.top = y; dr.right = x + (srr.right - srr.left); dr.bottom = y + (srr.bottom - srr.top);
    copy_rect(s, &dr, src, &srr);
    return DD_OK;
}

static HRESULT WINAPI s_flip(void **i, void **target, DWORD flags)
{
    surface *s = FROM(surface, vt4, i);
    (void)target; (void)flags;
    ensure_gfx(s->dd);
    /* with interpolation: in-between frame(s) first, the game's frame half a tick later */
    rec_present(*(volatile uint8_t *)G_FRAME_STEP_ADDR);
    return DD_OK;
}

static HRESULT WINAPI s_get_desc(void **i, LPDDSURFACEDESC2 d)
{
    if (!d)
        return DDERR_INVALIDPARAMS;
    fill_desc(FROM(surface, vt4, i), d);
    return DD_OK;
}

static HRESULT WINAPI s_get_desc_v1(void **i, LPDDSURFACEDESC d)
{
    DDSURFACEDESC2 d2;
    if (!d)
        return DDERR_INVALIDPARAMS;
    d2.dwSize = sizeof d2;
    fill_desc(FROM(surface, vt1, i), &d2);
    memcpy(d, &d2, sizeof *d);
    d->dwSize = sizeof *d;
    return DD_OK;
}

static HRESULT WINAPI s_get_caps(void **i, LPDDSCAPS2 c)
{
    surface *s = FROM(surface, vt4, i);
    if (!c)
        return DDERR_INVALIDPARAMS;
    memset(c, 0, sizeof *c);
    c->dwCaps = s->caps;
    c->dwCaps2 = s->caps2;
    return DD_OK;
}

static HRESULT WINAPI s_get_caps_v1(void **i, LPDDSCAPS c)
{
    if (!c)
        return DDERR_INVALIDPARAMS;
    c->dwCaps = FROM(surface, vt1, i)->caps;
    return DD_OK;
}

static HRESULT WINAPI s_get_pf(void **i, LPDDPIXELFORMAT pf)
{
    if (!pf)
        return DDERR_INVALIDPARAMS;
    *pf = FROM(surface, vt4, i)->pf;
    return DD_OK;
}

static HRESULT WINAPI s_get_pf_v1(void **i, LPDDPIXELFORMAT pf)
{
    if (!pf)
        return DDERR_INVALIDPARAMS;
    *pf = FROM(surface, vt1, i)->pf;
    return DD_OK;
}

static HRESULT WINAPI s_set_ck(void **i, DWORD flags, LPDDCOLORKEY k)
{
    surface *s = FROM(surface, vt4, i);
    if (!(flags & DDCKEY_SRCBLT))
        return DD_OK;
    if (k) {
        s->ck = *k;
        s->has_ck = 1;
    } else {
        s->has_ck = 0;
    }
    s->dirty = 1;
    return DD_OK;
}

static HRESULT WINAPI s_get_ck(void **i, DWORD flags, LPDDCOLORKEY k)
{
    surface *s = FROM(surface, vt4, i);
    if (!(flags & DDCKEY_SRCBLT) || !s->has_ck)
        return DDERR_NOCOLORKEY;
    if (k)
        *k = s->ck;
    return DD_OK;
}

static HRESULT WINAPI s_ok1(void **i) { (void)i; return DD_OK; }
static HRESULT WINAPI s_ok2(void **i, void *a) { (void)i; (void)a; return DD_OK; }
static HRESULT WINAPI s_no_palette(void **i, void *a) { (void)i; (void)a; return DDERR_NOPALETTEATTACHED; }

static HRESULT WINAPI s_get_dd(void **i, void **out)
{
    surface *s = FROM(surface, vt4, i);
    if (!out)
        return DDERR_INVALIDPARAMS;
    add_ref(&s->dd->ref);
    *out = &s->dd->vt4;
    return DD_OK;
}

static HRESULT WINAPI s_uniq(void **i, LPDWORD v) { (void)i; if (v) *v = 1; return DD_OK; }

/* IDirect3DTexture2 */
static HRESULT WINAPI t2_get_handle(void **i, void *dev, LPD3DTEXTUREHANDLE h)
{
    (void)dev;
    if (h)
        *h = (D3DTEXTUREHANDLE)(uintptr_t)FROM(surface, vtex, i);
    return D3D_OK;
}

static HRESULT WINAPI t2_palette_changed(void **i, DWORD a, DWORD b) { (void)i; (void)a; (void)b; return D3D_OK; }

static HRESULT WINAPI t2_load(void **i, void **srci)
{
    surface *s = FROM(surface, vtex, i);
    if (srci)
        copy_rect(s, NULL, FROM(surface, vtex, srci), NULL);
    return D3D_OK;
}

static surface *surf_new(ddraw *dd, int kind, int w, int h, const DDPIXELFORMAT *pf, DWORD caps, DWORD caps2)
{
    surface *s = (surface *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->next = g_surfaces;
    if (g_surfaces)
        g_surfaces->prev = s;
    g_surfaces = s;
    s->vt4 = vt_surf4;
    s->vt1 = vt_surf1;
    s->vtex = vt_tex2;
    s->ref = 1;
    s->dd = dd;
    s->kind = kind;
    s->caps = caps;
    s->caps2 = caps2;
    s->w = w;
    s->h = h;
    if (pf)
        s->pf = *pf;
    s->pitch = (LONG)w * (s->pf.dwRGBBitCount ? s->pf.dwRGBBitCount / 8 : 2);
    if (kind == K_TEXTURE || kind == K_OFFSCREEN) {
        s->mem = (uint8_t *)calloc((size_t)s->pitch, (size_t)h);
        if (!s->mem) {
            free(s);
            return NULL;
        }
        s->dirty = 1;
    }
    return s;
}

/* ------------------------------------------------------------------ viewport / material / clipper */

static material *g_materials[64];
static int g_nmat;

static HRESULT WINAPI vp_qi(void **i, REFIID r, void **o) { (void)r; *o = i; add_ref(&FROM(viewport, vt, i)->ref); return S_OK; }
static ULONG WINAPI vp_addref(void **i) { return add_ref(&FROM(viewport, vt, i)->ref); }
static ULONG WINAPI vp_release(void **i)
{
    viewport *v = FROM(viewport, vt, i);
    LONG n = InterlockedDecrement(&v->ref);
    if (n == 0)
        free(v);
    return (ULONG)(n < 0 ? 0 : n);
}
static HRESULT WINAPI vp_set_bg(void **i, D3DMATERIALHANDLE h) { FROM(viewport, vt, i)->bg = h; return D3D_OK; }
static HRESULT WINAPI vp_set2(void **i, LPD3DVIEWPORT2 v) { if (v) FROM(viewport, vt, i)->v = *v; return D3D_OK; }
static HRESULT WINAPI vp_get2(void **i, LPD3DVIEWPORT2 v) { if (v) *v = FROM(viewport, vt, i)->v; return D3D_OK; }

static uint32_t bg_color(viewport *v)
{
    int k;
    for (k = 0; k < g_nmat; k++)
        if ((D3DMATERIALHANDLE)(uintptr_t)g_materials[k] == v->bg) {
            D3DCOLORVALUE c = g_materials[k]->m.diffuse;
            return ((uint32_t)(c.r * 255) << 16) | ((uint32_t)(c.g * 255) << 8) | (uint32_t)(c.b * 255);
        }
    return 0;
}

static HRESULT WINAPI vp_clear(void **i, DWORD n, LPD3DRECT rects, DWORD flags)
{
    viewport *v = FROM(viewport, vt, i);
    (void)n; (void)rects;
    rec_clear(NULL, (flags & D3DCLEAR_TARGET) != 0, bg_color(v), (flags & D3DCLEAR_ZBUFFER) != 0, 1.0f);
    return D3D_OK;
}

static HRESULT WINAPI vp_clear2(void **i, DWORD n, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD st)
{
    (void)i; (void)n; (void)rects; (void)st;
    rec_clear(NULL, (flags & D3DCLEAR_TARGET) != 0, color & 0xFFFFFF, (flags & D3DCLEAR_ZBUFFER) != 0, z);
    return D3D_OK;
}

static HRESULT WINAPI mat_qi(void **i, REFIID r, void **o) { (void)r; *o = i; add_ref(&FROM(material, vt, i)->ref); return S_OK; }
static ULONG WINAPI mat_addref(void **i) { return add_ref(&FROM(material, vt, i)->ref); }
static ULONG WINAPI mat_release(void **i) { LONG n = InterlockedDecrement(&FROM(material, vt, i)->ref); return (ULONG)(n < 0 ? 0 : n); }
static HRESULT WINAPI mat_set(void **i, LPD3DMATERIAL m) { if (m) FROM(material, vt, i)->m = *m; return D3D_OK; }
static HRESULT WINAPI mat_get(void **i, LPD3DMATERIAL m) { if (m) *m = FROM(material, vt, i)->m; return D3D_OK; }
static HRESULT WINAPI mat_handle(void **i, void *dev, LPD3DMATERIALHANDLE h)
{
    (void)dev;
    if (h)
        *h = (D3DMATERIALHANDLE)(uintptr_t)FROM(material, vt, i);
    return D3D_OK;
}

static HRESULT WINAPI clip_qi(void **i, REFIID r, void **o) { (void)r; *o = i; add_ref(&FROM(clipper, vt, i)->ref); return S_OK; }
static ULONG WINAPI clip_addref(void **i) { return add_ref(&FROM(clipper, vt, i)->ref); }
static ULONG WINAPI clip_release(void **i)
{
    clipper *c = FROM(clipper, vt, i);
    LONG n = InterlockedDecrement(&c->ref);
    if (n == 0)
        free(c);
    return (ULONG)(n < 0 ? 0 : n);
}
static HRESULT WINAPI clip_set_hwnd(void **i, DWORD f, HWND w) { (void)i; (void)f; (void)w; return DD_OK; }

/* ------------------------------------------------------------------ device */

static HRESULT WINAPI dev_qi(void **i, REFIID r, void **o) { (void)r; *o = i; add_ref(&FROM(device, vt, i)->ref); return S_OK; }
static ULONG WINAPI dev_addref(void **i) { return add_ref(&FROM(device, vt, i)->ref); }
static ULONG WINAPI dev_release(void **i) { LONG n = InterlockedDecrement(&FROM(device, vt, i)->ref); return (ULONG)(n < 0 ? 0 : n); }
static HRESULT WINAPI dev_ok1(void **i) { (void)i; return D3D_OK; }

static HRESULT WINAPI dev_add_vp(void **i, void **vp) { (void)i; if (vp) add_ref(&FROM(viewport, vt, vp)->ref); return D3D_OK; }
static HRESULT WINAPI dev_set_vp(void **i, void **vp)
{
    device *d = FROM(device, vt, i);
    d->vp = vp ? FROM(viewport, vt, vp) : NULL;
    return D3D_OK;
}
static HRESULT WINAPI dev_get_vp(void **i, void **vp)
{
    device *d = FROM(device, vt, i);
    if (!vp)
        return DDERR_INVALIDPARAMS;
    *vp = d->vp ? &d->vp->vt : NULL;
    if (d->vp)
        add_ref(&d->vp->ref);
    return D3D_OK;
}

static HRESULT WINAPI dev_enum_tex_formats(void **i, LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx)
{
    DDPIXELFORMAT pf;
    (void)i;
    if (!cb)
        return DDERR_INVALIDPARAMS;
    set_pf_1555(&pf);
    if (cb(&pf, ctx) == D3DENUMRET_CANCEL)
        return D3D_OK;
    set_pf_565(&pf);
    cb(&pf, ctx);
    return D3D_OK;
}

static HRESULT WINAPI dev_get_d3d(void **i, void **out)
{
    device *d = FROM(device, vt, i);
    if (!out)
        return DDERR_INVALIDPARAMS;
    add_ref(&d->dd->ref);
    *out = &d->dd->vt3d;
    return D3D_OK;
}

static HRESULT WINAPI dev_set_rs(void **i, D3DRENDERSTATETYPE s, DWORD v)
{
    if ((unsigned)s < 256)
        FROM(device, vt, i)->rs[s] = v;
    return D3D_OK;
}

static HRESULT WINAPI dev_get_rs(void **i, D3DRENDERSTATETYPE s, LPDWORD v)
{
    if (v)
        *v = (unsigned)s < 256 ? FROM(device, vt, i)->rs[s] : 0;
    return D3D_OK;
}

static HRESULT WINAPI dev_set_tss(void **i, DWORD stage, D3DTEXTURESTAGESTATETYPE s, DWORD v)
{
    if (stage < 2 && (unsigned)s < 32)
        FROM(device, vt, i)->tss[stage][s] = v;
    return D3D_OK;
}

static HRESULT WINAPI dev_get_tss(void **i, DWORD stage, D3DTEXTURESTAGESTATETYPE s, LPDWORD v)
{
    if (v)
        *v = (stage < 2 && (unsigned)s < 32) ? FROM(device, vt, i)->tss[stage][s] : 0;
    return D3D_OK;
}

static HRESULT WINAPI dev_set_texture(void **i, DWORD stage, void **t)
{
    device *d = FROM(device, vt, i);
    if (stage == 0)
        d->tex = t ? FROM(surface, vtex, t) : NULL;
    return D3D_OK;
}

static HRESULT WINAPI dev_get_texture(void **i, DWORD stage, void **t)
{
    device *d = FROM(device, vt, i);
    if (!t)
        return DDERR_INVALIDPARAMS;
    *t = (stage == 0 && d->tex) ? &d->tex->vtex : NULL;
    if (*t)
        add_ref(&d->tex->ref);
    return D3D_OK;
}

static void device_state(device *d, gfx_state *s)
{
    const DWORD *rs = d->rs;
    memset(s, 0, sizeof *s);
    s->blend = rs[D3DRENDERSTATE_ALPHABLENDENABLE] != 0;
    s->src_blend = (uint8_t)rs[D3DRENDERSTATE_SRCBLEND];
    s->dst_blend = (uint8_t)rs[D3DRENDERSTATE_DESTBLEND];
    s->z_enable = rs[D3DRENDERSTATE_ZENABLE] != 0;
    s->z_write = rs[D3DRENDERSTATE_ZWRITEENABLE] != 0;
    s->z_func = (uint8_t)rs[D3DRENDERSTATE_ZFUNC];
    s->alpha_test = rs[D3DRENDERSTATE_ALPHATESTENABLE] != 0;
    s->alpha_func = (uint8_t)rs[D3DRENDERSTATE_ALPHAFUNC];
    s->alpha_ref = (uint8_t)rs[D3DRENDERSTATE_ALPHAREF];
    s->flat = rs[D3DRENDERSTATE_SHADEMODE] == D3DSHADE_FLAT;
    s->linear = d->tss[0][D3DTSS_MAGFILTER] >= D3DTFG_LINEAR;
    s->fog = rs[D3DRENDERSTATE_FOGENABLE] != 0 && rs[D3DRENDERSTATE_FOGTABLEMODE] != D3DFOG_NONE;
    s->fog_color = rs[D3DRENDERSTATE_FOGCOLOR] & 0xFFFFFF;
    memcpy(&s->fog_start, &rs[D3DRENDERSTATE_FOGTABLESTART], 4);
    memcpy(&s->fog_end, &rs[D3DRENDERSTATE_FOGTABLEEND], 4);
}

static void prepare_texture(device *d)
{
    surface *t = d->tex;
    if (t && t->dirty)
        upload(t);
    if (t)
        t->used_frame = g_frame_no;
    rec_texture(t ? t->tex : NULL);
}

/* [debug] fog_debug=1: log what fogged geometry looks like (fog distance source research) */
int g_fog_debug;
static unsigned g_dropped_tris, g_world_tris;   /* clamped-vertex filter stats, see filter_clamped */
static struct { unsigned draws, sa_hist[5]; float z0, z1, w0, w1, start, end; DWORD color; unsigned sa0, sa1; } g_fd;

static void fog_debug(const gfx_state *s, const gfx_vertex *v, DWORD n)
{
    static unsigned frames_seen;
    DWORD k;
    if (!s->fog)
        return;
    if (!g_fd.draws) {
        g_fd.z0 = g_fd.w0 = FLT_MAX;
        g_fd.z1 = g_fd.w1 = -FLT_MAX;
        g_fd.sa0 = 255;
        g_fd.sa1 = 0;
    }
    g_fd.draws++;
    g_fd.start = s->fog_start;
    g_fd.end = s->fog_end;
    g_fd.color = s->fog_color;
    for (k = 0; k < n; k++) {
        float w = v[k].rhw > 0 ? 1.0f / v[k].rhw : 0;
        if (v[k].z < g_fd.z0) g_fd.z0 = v[k].z;
        if (v[k].z > g_fd.z1) g_fd.z1 = v[k].z;
        if (w < g_fd.w0) g_fd.w0 = w;
        if (w > g_fd.w1) g_fd.w1 = w;
        {
            unsigned sa = v[k].specular >> 24;
            if (sa < g_fd.sa0) g_fd.sa0 = sa;
            if (sa > g_fd.sa1) g_fd.sa1 = sa;
            g_fd.sa_hist[sa == 255 ? 4 : sa / 64]++;
        }
    }
    (void)frames_seen;
}

/* [debug] probe=x,y (game coordinates): once every 2 s, log the triangles of
 * one frame that cover that point, with their vertices and state */
int g_probe_x = -1, g_probe_y = -1;
static int g_probe_armed, g_probe_logged;

static float edge(const gfx_vertex *a, const gfx_vertex *b, float px, float py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static int covers(const gfx_vertex *a, const gfx_vertex *b, const gfx_vertex *c, float px, float py)
{
    float e0 = edge(a, b, px, py), e1 = edge(b, c, px, py), e2 = edge(c, a, px, py);
    return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

static void probe(const device *d, int pt, const gfx_vertex *v, DWORD n)
{
    DWORD k;
    if (!g_probe_armed || g_probe_logged >= 12)
        return;
    for (k = 0; k + 2 < n; k += (pt == D3DPT_TRIANGLELIST ? 3 : 1)) {
        const gfx_vertex *a = &v[k], *b = &v[k + 1], *c = &v[k + 2];
        int j;
        if (!covers(a, b, c, (float)g_probe_x, (float)g_probe_y))
            continue;
        g_probe_logged++;
        log_printf("probe: tri in draw of %lu (type %d) tex %p z %lu/%lu/%lu blend %lu %lu/%lu fog %lu alphatest %lu",
                   n, pt, (void *)d->tex, d->rs[D3DRENDERSTATE_ZENABLE], d->rs[D3DRENDERSTATE_ZWRITEENABLE],
                   d->rs[D3DRENDERSTATE_ZFUNC], d->rs[D3DRENDERSTATE_ALPHABLENDENABLE], d->rs[D3DRENDERSTATE_SRCBLEND],
                   d->rs[D3DRENDERSTATE_DESTBLEND], d->rs[D3DRENDERSTATE_FOGENABLE], d->rs[D3DRENDERSTATE_ALPHATESTENABLE]);
        for (j = 0; j < 3; j++) {
            const gfx_vertex *p = j == 0 ? a : j == 1 ? b : c;
            log_printf("   v%d x %.1f y %.1f z %.5f rhw %.5f diffuse %08lX spec %08lX uv %.3f %.3f",
                       j, p->x, p->y, p->z, p->rhw, (unsigned long)p->diffuse, (unsigned long)p->specular, p->u, p->v);
        }
        if (g_probe_logged >= 12)
            return;
    }
}

static void bg_frame(void);

void ddshim_frame(void)
{
    static unsigned frames;
    static unsigned clamp_frames;
    static unsigned interp_frames, bc_frames;
    g_frame_no++;
    if (g_blend_census && ++bc_frames % 60 == 0) {
        int k;
        log_printf("blend census: camera mode %d scripted %d distance %d pitch %d (60 frames)",
                   *(uint8_t *)0x77E955u, *(uint8_t *)0x77E956u, *(int32_t *)0x77E95Cu, *(int32_t *)0x77E964u);
        for (k = 0; k < 32 && g_bc[k].draws; k++)
            log_printf("   world op %02X blend %d (%d,%d): %u draws %u verts z %.4f..%.4f", g_bc[k].op, g_bc[k].blend,
                       g_bc[k].src, g_bc[k].dst, g_bc[k].draws, g_bc[k].verts, g_bc[k].z0, g_bc[k].z1);
        memset(g_bc, 0, sizeof g_bc);
    }
    if (rec_enabled())
        bg_frame();
    if (g_interp_census && ++interp_frames % 60 == 0) {
        log_printf("interp census: world vertices exact %u, borrowed %u, static %u; textures changed mid-frame %u (60 frames)",
                   g_iv_exact, g_iv_borrowed, g_iv_static, g_midframe_uploads);
        g_midframe_uploads = 0;
        g_iv_exact = g_iv_borrowed = g_iv_static = 0;
    }
    if (g_fog_debug && ++clamp_frames % 60 == 0) {
        log_printf("clamped-vertex triangles dropped: %u (of %u in draws that had one)", g_dropped_tris, g_world_tris);
        g_dropped_tris = g_world_tris = 0;
    }
    if (g_probe_x >= 0) {
        if (g_probe_armed)
            g_probe_armed = 0;
        else if (++frames % 60 == 0) {
            g_probe_armed = 1;
            g_probe_logged = 0;
            log_printf("probe: frame at (%d,%d)", g_probe_x, g_probe_y);
        }
        return;
    }
    if (!g_fog_debug || ++frames % 60 != 0)
        return;
    if (g_fd.draws)
        log_printf("fog: %u fogged draws, start %.3f end %.3f color %06lX, z %.4f..%.4f, 1/rhw %.3f..%.3f, specular alpha %u..%u [0-63:%u 64-127:%u 128-191:%u 192-254:%u 255:%u]",
                   g_fd.draws, g_fd.start, g_fd.end, g_fd.color, g_fd.z0, g_fd.z1, g_fd.w0, g_fd.w1, g_fd.sa0, g_fd.sa1,
                   g_fd.sa_hist[0], g_fd.sa_hist[1], g_fd.sa_hist[2], g_fd.sa_hist[3], g_fd.sa_hist[4]);
    else
        log_printf("fog: no fogged draws");
    memset(&g_fd, 0, sizeof g_fd);
}

/* The game never clips terrain against the near plane: a vertex behind the
 * camera gets its depth clamped (z = 1.0, rhw = 0.1) and lands thousands of
 * pixels off-screen. The old pipeline's driver silently discarded such
 * triangles (they exceed the rasterizer's coordinate range), which is where
 * the "holes in the floor at low camera angles" come from. A modern GPU clips
 * them properly instead, so they'd show up as huge wrong polygons. terrain.c
 * now clips those polygons before they get here; this drop is the safety net
 * for anything it doesn't cover (or terrain_clip=0), matching the old look. */
int g_drop_clamped = 1;

static int clamped(const gfx_vertex *v)
{
    return v->z >= 0.99999f && v->rhw <= 0.10001f;
}

/* returns the number of vertices written to out (a triangle list) */
static DWORD filter_clamped(int pt, const gfx_vertex *v, DWORD n, gfx_vertex *out, DWORD cap)
{
    DWORD k, m = 0;
    for (k = 0; k + 2 < n; k += (pt == D3DPT_TRIANGLELIST ? 3 : 1)) {
        const gfx_vertex *a = &v[k], *b = &v[k + 1], *c = &v[k + 2];
        g_world_tris++;
        if (clamped(a) || clamped(b) || clamped(c)) {
            g_dropped_tris++;
            continue;
        }
        if (m + 3 > cap)
            break;
        if (pt == D3DPT_TRIANGLESTRIP && (k & 1)) {   /* keep strip winding consistent (culling is off anyway) */
            out[m++] = *b; out[m++] = *a; out[m++] = *c;
        } else {
            out[m++] = *a; out[m++] = *b; out[m++] = *c;
        }
    }
    return m;
}

/* Interpolation: where each world vertex of this draw was one game tick ago,
 * in camera space (the replay blends in 3D and projects). Vertices the GTE projected know their own
 * previous camera-space position. The rest (midpoints of the polygons the game
 * subdivides near the camera, clipped edges, screen-space grass and sprites)
 * are rebuilt from screen + depth, moved with the camera, plus whatever extra
 * motion their own polygon's captured corners had (none for the static world,
 * the character's own for its parts). Buildings and props aren't captured
 * at all and move with the camera only; a small semi-transparent draw with
 * no captured corner (a shadow) borrows from the nearest captured vertex at
 * about the same depth. */

/* [debug] interp_tint=1: colour world draws by how they're interpolated */
int g_interp_tint;
static uint32_t g_tint;   /* colour for the draw world_prevs just handled, 0 = none */

static const rec_prev *world_prevs(const gfx_vertex *raw, DWORD n)
{
    enum { NONE, EXACT, REBUILT };
    static rec_prev out[4096];
    static interp_vec3 cur[4096], prev[4096], res[4096];
    static unsigned char how[4096], has_res[4096];
    float hx = mml_h_x(), hy = *(const float *)G_GTE_H_ADDR, near_z = *(const float *)G_GTE_NEAR_ADDR;
    float ofx = (float)*(const int32_t *)G_GTE_OFX_ADDR, ofy = (float)*(const int32_t *)G_GTE_OFY_ADDR;
    DWORD k, j;
    int exact = 0, little = 0, moves, rematched = 0, borrowed = 0, rebuilt = 0;
    if (n > sizeof out / sizeof out[0])
        return NULL;
    for (k = 0; k < n; k++) {
        float sx = raw[k].x * 0.5f, sy = raw[k].y * 0.5f;
        interp_vec3 *c = &cur[k];
        how[k] = NONE;
        has_res[k] = 0;
        int found = interp_lookup(sx, sy, c, &prev[k]);
        if (found == 3) {
            /* the ground: exact position, moved with the camera only */
            if (interp_static_prev(c, &prev[k])) {
                how[k] = EXACT;
                res[k].x = res[k].y = res[k].z = 0;
                has_res[k] = 1;
                exact++;
            }
        } else if (found == 2) {
            how[k] = EXACT;
            if (interp_debug_last_rematched())
                rematched = 1;
            has_res[k] = (unsigned char)interp_residual(c, &prev[k], &res[k]);
            exact += has_res[k];
        } else if (!(raw[k].z == 0.1f && raw[k].rhw == 1.0f) && raw[k].z < 0.98f) {
            /* not captured: rebuild its camera-space position from screen + depth */
            c->z = raw[k].z * 16384.0f;
            c->x = (sx - ofx) * c->z / hx;
            c->y = (sy - ofy) * c->z / hy;
            if (c->z >= near_z && interp_static_prev(c, &prev[k]))
                how[k] = REBUILT;
        }
    }
    if (!exact && g_cur_pkt && (g_cur_pkt[7] & 2)) {
        /* nothing captured in this draw: only something small and
         * semi-transparent (a shadow, a glow, at most ~90 units) rides on a
         * moving object. Buildings and props aren't captured either, but
         * they're scenery: borrowing would drag a sidewalk quad or a trash
         * can along with Mega Man when he's next to it */
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (k = 0; k < n; k++) {
            const float *c = &cur[k].x;
            if (how[k] != REBUILT)
                continue;
            for (j = 0; j < 3; j++) {
                if (c[j] < lo[j]) lo[j] = c[j];
                if (c[j] > hi[j]) hi[j] = c[j];
            }
        }
        little = hi[0] - lo[0] < 150.0f && hi[1] - lo[1] < 150.0f && hi[2] - lo[2] < 150.0f;
    }
    for (k = 0; k < n; k++) {
        interp_vec3 r = { 0, 0, 0 };
        if (how[k] != REBUILT)
            continue;
        if (exact) {
            /* the polygon's own captured corners (a polygon's vertices sit
             * close together in the draw), nearer ones counting more */
            float wsum = 0;
            DWORD j0 = k > 12 ? k - 12 : 0, j1 = k + 13 < n ? k + 13 : n;
            for (j = j0; j < j1; j++) {
                float dx, dy, w;
                if (!has_res[j])
                    continue;
                dx = raw[j].x - raw[k].x;
                dy = raw[j].y - raw[k].y;
                w = 1.0f / (dx * dx + dy * dy + 1.0f);
                r.x += res[j].x * w;
                r.y += res[j].y * w;
                r.z += res[j].z * w;
                wsum += w;
            }
            if (wsum > 0) {
                r.x /= wsum;
                r.y /= wsum;
                r.z /= wsum;
            }
        } else if (little) {
            borrowed |= interp_borrow_residual(raw[k].x * 0.5f, raw[k].y * 0.5f, cur[k].z, &r);   /* else camera motion only */
        }
        rebuilt = 1;
        prev[k].x += r.x;
        prev[k].y += r.y;
        prev[k].z += r.z;
        res[k] = r;
    }
    /* A draw whose own motion (beyond the camera's) is more than 60% of its
     * distance is a mismatch or a teleport: it holds still for the in-between
     * frame, all of it, since a polygon with only some corners blended tears.
     * Camera cuts are caught for the whole frame by interp.c. Corners that end
     * up behind the near plane mid-blend are fine: the replay cuts the
     * triangles there, in 3D, like the game does. */
    for (k = 0; k < n; k++) {
        const interp_vec3 *c = &cur[k], *p = &prev[k];
        interp_vec3 d = res[k];
        float rc = c->x * c->x + c->y * c->y + c->z * c->z, rp = p->x * p->x + p->y * p->y + p->z * p->z;
        if (how[k] == NONE)
            continue;
        if (how[k] == EXACT && !has_res[k]) {
            d.x = p->x - c->x;
            d.y = p->y - c->y;
            d.z = p->z - c->z;
        }
        if (c->z < near_z || d.x * d.x + d.y * d.y + d.z * d.z > 0.36f * (rc > rp ? rc : rp))
            break;
    }
    moves = k == n;
    if (g_interp_tint) {
        /* red: borrowed a nearby object's motion; blue: held still; yellow: took
         * another copy of its model; green: nothing captured, camera motion only */
        g_tint = !moves ? 0xFF2060FF : borrowed ? 0xFFFF2020 : rematched ? 0xFFFFE000 :
                 (rebuilt && !exact) ? 0xFF20E040 : 0;
    }
    for (k = 0; k < n; k++) {
        out[k].valid = REC_STILL;
        if (moves && how[k] != NONE) {
            out[k].valid = REC_PERSP;
            out[k].cx = cur[k].x;
            out[k].cy = cur[k].y;
            out[k].cz = cur[k].z;
            out[k].px = prev[k].x;
            out[k].py = prev[k].y;
            out[k].pz = prev[k].z;
            out[k].kx = 2.0f * hx;
            out[k].ky = 2.0f * hy;
            out[k].near_z = near_z;
        }
        if (!out[k].valid)
            g_iv_static++;
        else if (how[k] == EXACT)
            g_iv_exact++;
        else
            g_iv_borrowed++;
    }
    return out;
}

/* Interpolation for 2D backgrounds (the sky): the game scrolls them with the
 * camera once per tick, so match each to the previous frame's draw with the
 * same bucket, order in it, texture and size, and blend their positions too. */
#define BG_MAX 256
#define BG_VERTS 8
typedef struct {
    int bucket, ord;
    gfx_tex *tex;
    DWORD n;
    float x[BG_VERTS], y[BG_VERTS];
} bg_draw;
static bg_draw g_bg[2][BG_MAX];
static int g_nbg[2], g_bg_cur, g_bg_ord[8];
/* the layer's scroll since last frame, from its first cleanly matched draw */
static float g_bg_dx[8], g_bg_dy[8];
static int g_bg_has_d[8];
static float g_bg_last_dx[8], g_bg_last_dy[8];   /* last frame's, for tiles drawn before any match */
static int g_bg_last_has[8];

static void bg_frame(void)
{
    g_bg_cur ^= 1;
    g_nbg[g_bg_cur] = 0;
    memset(g_bg_ord, 0, sizeof g_bg_ord);
    {
        int b;
        for (b = 0; b < 8; b++) {
            g_bg_last_has[b] = g_bg_has_d[b];
            g_bg_last_dx[b] = g_bg_dx[b];
            g_bg_last_dy[b] = g_bg_dy[b];
        }
    }
    memset(g_bg_has_d, 0, sizeof g_bg_has_d);
}

static const rec_prev *background_prevs(const gfx_vertex *v, DWORD n, gfx_tex *tex)
{
    if (g_interp_census && (g_frame_no % 150) == 0) {
        DWORD q;
        log_printf("bg draw: bucket %d op %02X pkt %p n %lu tex %p", g_cur_bucket, g_cur_pkt ? g_cur_pkt[7] : 0,
                   (const void *)g_cur_pkt, n, (void *)tex);
        for (q = 0; q < n; q++)
            log_printf("   v%lu  x %.3f y %.3f  u %.4f v %.4f", q, v[q].x, v[q].y, v[q].u, v[q].v);
    }
    static rec_prev out[BG_VERTS];
    const bg_draw *old = g_bg[g_bg_cur ^ 1];
    bg_draw *mine;
    int b = g_cur_bucket, ord, j, found = -1;
    DWORD k;
    if (b < 0 || b > 7 || n > BG_VERTS || g_nbg[g_bg_cur] >= BG_MAX)
        return NULL;
    ord = g_bg_ord[b]++;
    mine = &g_bg[g_bg_cur][g_nbg[g_bg_cur]++];
    mine->bucket = b;
    mine->ord = ord;
    mine->tex = tex;
    mine->n = n;
    for (k = 0; k < n; k++) {
        mine->x[k] = v[k].x;
        mine->y[k] = v[k].y;
    }
    for (j = 0; j < g_nbg[g_bg_cur ^ 1]; j++)
        if (old[j].bucket == b && old[j].ord == ord && old[j].tex == tex && old[j].n == n) {
            found = j;
            break;
        }
    int own = 0;
    float odx = 0.0f, ody = 0.0f;
    if (found >= 0) {
        /* a clean match moves every vertex by the same small amount */
        float dx0 = old[found].x[0] - v[0].x, dy0 = old[found].y[0] - v[0].y;
        int clean = dx0 > -160.0f && dx0 < 160.0f && dy0 > -120.0f && dy0 < 120.0f;
        for (k = 1; k < n && clean; k++) {
            float ex = old[found].x[k] - v[k].x - dx0, ey = old[found].y[k] - v[k].y - dy0;
            clean = ex > -0.5f && ex < 0.5f && ey > -0.5f && ey < 0.5f;
        }
        if (clean) {
            own = 1;
            odx = dx0;
            ody = dy0;
            if (!g_bg_has_d[b]) {
                g_bg_dx[b] = dx0;
                g_bg_dy[b] = dy0;
                g_bg_has_d[b] = 1;
            }
        }
    }
    /* textured tiles of the layer that wrapped around or have no match scroll with the rest */
    if (!own) {
        if (!tex)
            return NULL;   /* untextured (fades, boxes) never borrow the scroll */
        if (g_bg_has_d[b]) {
            odx = g_bg_dx[b];
            ody = g_bg_dy[b];
        } else if (g_bg_last_has[b]) {
            odx = g_bg_last_dx[b];
            ody = g_bg_last_dy[b];
        } else {
            return NULL;
        }
    }
    for (k = 0; k < n; k++) {
        out[k].x = v[k].x + odx;
        out[k].y = v[k].y + ody;
        out[k].z = v[k].z;
        out[k].valid = REC_SCREEN;
    }
    return out;
}

static HRESULT WINAPI dev_draw_prim(void **i, D3DPRIMITIVETYPE pt, DWORD fvf, LPVOID verts, DWORD n, DWORD flags)
{
    device *d = FROM(device, vt, i);
    const rec_prev *prevs = NULL;
    int layer_tile = 0, fvf_ok_for_layers = fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1);
    gfx_state s;
    static volatile LONG warned;
    (void)flags;
    if (fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)) {
        if (InterlockedExchange(&warned, 1) == 0)
            log_printf("ddshim: DrawPrimitive with unsupported FVF %08lX ignored", fvf);
        return D3D_OK;
    }
    ensure_gfx(d->dd);
    device_state(d, &s);
    if (g_cur_pkt && lockon_owns((const void *)g_cur_pkt) && n <= 8) {
        /* lock-on reticle: drawn as opaque flat shapes so the game's blend mode
         * can't touch it, made see-through here */
        static gfx_vertex rv[8];
        DWORD k;
        for (k = 0; k < n; k++) {
            rv[k] = ((const gfx_vertex *)verts)[k];
            rv[k].diffuse = (rv[k].diffuse & 0x00FFFFFF) | (lockon_alpha() << 24);
        }
        verts = rv;
        s.blend = 1;
        s.src_blend = D3DBLEND_SRCALPHA;
        s.dst_blend = D3DBLEND_INVSRCALPHA;
    }
    prepare_texture(d);
    /* the 3D world's depth counts for effects (not 2D overlays drawn in its buckets) */
    if ((g_cur_bucket == 3 || g_cur_bucket == 4) && s.z_enable) {
        const gfx_vertex *v0 = (const gfx_vertex *)(g_display_raw_verts ? g_display_raw_verts : verts);
        s.scene_depth = !(v0->z == 0.1f && v0->rhw == 1.0f) && v0->z < 0.98f;
    }
    rec_state(&s);
    if (g_fog_debug)
        fog_debug(&s, (const gfx_vertex *)verts, n);
    if (g_smooth_bg && g_cur_pkt && n <= 8 && fvf_ok_for_layers) {
        /* background layer tiles (the sky): sub-pixel position, and its real scroll for blending */
        static gfx_vertex fixed[8];
        static rec_prev bprev[8];
        float ox, oy, px, py, sc = 1.0f;
        int bl = bglayer_offsets((const void *)g_cur_pkt, &ox, &oy, &px, &py);
        if (bl) {
            const gfx_vertex *raw = g_display_raw_verts ? (const gfx_vertex *)g_display_raw_verts
                                                        : (const gfx_vertex *)verts;
            DWORD k;
            if (n >= 2 && raw[1].x != raw[0].x)
                sc = (((const gfx_vertex *)verts)[1].x - ((const gfx_vertex *)verts)[0].x) / (raw[1].x - raw[0].x);
            for (k = 0; k < n; k++) {
                fixed[k] = ((const gfx_vertex *)verts)[k];
                fixed[k].x += ox * sc;
                fixed[k].y += oy;
                bprev[k].x = fixed[k].x + px * sc;
                bprev[k].y = fixed[k].y + py;
                bprev[k].z = fixed[k].z;
                bprev[k].valid = bl == 2 ? REC_SCREEN : REC_STILL;
            }
            verts = fixed;
            if (rec_enabled())
                prevs = bprev;
            layer_tile = 1;
        }
    }
    if (g_blend_census && g_cur_bucket == 4) {
        uint8_t op = g_cur_pkt ? (uint8_t)(g_cur_pkt[7] & 0xFC) : 0;
        int k;
        for (k = 0; k < 32 && g_bc[k].draws &&
                    !(g_bc[k].op == op && g_bc[k].blend == s.blend && g_bc[k].src == s.src_blend && g_bc[k].dst == s.dst_blend); k++)
            ;
        if (k < 32) {
            DWORD q;
            if (!g_bc[k].draws) {
                g_bc[k].op = op; g_bc[k].blend = s.blend; g_bc[k].src = s.src_blend; g_bc[k].dst = s.dst_blend;
                g_bc[k].z0 = 1e9f; g_bc[k].z1 = -1e9f;
            }
            g_bc[k].draws++;
            g_bc[k].verts += n;
            for (q = 0; q < n; q++) {
                float z = ((const gfx_vertex *)verts)[q].z;
                if (z < g_bc[k].z0) g_bc[k].z0 = z;
                if (z > g_bc[k].z1) g_bc[k].z1 = z;
            }
        }
    }
    if (layer_tile)
        ;
    else if (rec_enabled() && g_cur_bucket == 4)
        prevs = world_prevs(g_display_raw_verts ? (const gfx_vertex *)g_display_raw_verts : (const gfx_vertex *)verts, n);
    else if (rec_enabled() && g_cur_bucket >= 5)
        prevs = background_prevs((const gfx_vertex *)verts, n, d->tex ? d->tex->tex : NULL);
    if (g_drop_clamped && g_cur_bucket == 4 && (pt == D3DPT_TRIANGLESTRIP || pt == D3DPT_TRIANGLELIST)) {
        static gfx_vertex tmp[3 * 2048];
        DWORD k;
        for (k = 0; k < n; k++)
            if (clamped(&((const gfx_vertex *)verts)[k]))
                break;
        if (k < n) {   /* only rebuild draws that actually contain a clamped vertex */
            DWORD m = filter_clamped((int)pt, (const gfx_vertex *)verts, n, tmp, 3 * 2048);
            if (m)
                rec_draw(GFX_TRILIST, tmp, NULL, (int)m);
            return D3D_OK;
        }
    }
    if (g_cur_bucket == 4)
        verts = (LPVOID)clipfix_apply((const gfx_vertex *)verts,
                                            g_display_raw_verts ? (const gfx_vertex *)g_display_raw_verts
                                                                : (const gfx_vertex *)verts, (unsigned)n);
    if (g_tint) {
        static gfx_vertex tv[4096];
        DWORD q;
        if (n <= 4096) {
            for (q = 0; q < n; q++) {
                tv[q] = ((const gfx_vertex *)verts)[q];
                tv[q].diffuse = g_tint;
            }
            verts = tv;
        }
        g_tint = 0;
    }
    rec_draw((int)pt, (const gfx_vertex *)verts, prevs, (int)n);
    return D3D_OK;
}


static void fill_device_desc(D3DDEVICEDESC *dd)
{
    D3DPRIMCAPS *tc = &dd->dpcTriCaps;
    DWORD size = dd->dwSize ? dd->dwSize : sizeof *dd;
    memset(dd, 0, size);
    dd->dwSize = size;
    dd->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRICAPS | D3DDD_LINECAPS |
                  D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH;
    dd->dcmColorModel = D3DCOLOR_RGB;
    dd->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                    D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                    D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_HWRASTERIZATION;
    tc->dwSize = sizeof *tc;
    tc->dwMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    tc->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_ZTEST |
                       D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_ZBIAS;
    tc->dwZCmpCaps = tc->dwAlphaCmpCaps = 0xFF;
    tc->dwSrcBlendCaps = tc->dwDestBlendCaps = 0x1FFF;
    tc->dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND |
                      D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD | D3DPSHADECAPS_SPECULARGOURAUDRGB;
    tc->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY |
                        D3DPTEXTURECAPS_POW2;
    tc->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MINFPOINT |
                              D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    tc->dwTextureBlendCaps = D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_DECAL |
                             D3DPTBLENDCAPS_COPY;
    tc->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_INDEPENDENTUV;
    dd->dpcLineCaps = *tc;
    dd->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    dd->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
    if (size >= sizeof(D3DDEVICEDESC)) {
        dd->dwMinTextureWidth = dd->dwMinTextureHeight = 1;
        dd->dwMaxTextureWidth = dd->dwMaxTextureHeight = 2048;
        dd->dwMaxTextureRepeat = 2048;
        dd->dwMaxTextureAspectRatio = 2048;
        dd->dwMaxVertexCount = 65535;
        dd->wMaxTextureBlendStages = 2;
        dd->wMaxSimultaneousTextures = 1;
    }
}

static HRESULT WINAPI dev_get_caps(void **i, LPD3DDEVICEDESC hw, LPD3DDEVICEDESC hel)
{
    (void)i;
    if (hw)
        fill_device_desc(hw);
    if (hel)
        fill_device_desc(hel);
    return D3D_OK;
}

static HRESULT WINAPI dev_validate(void **i, LPDWORD passes) { (void)i; if (passes) *passes = 1; return D3D_OK; }

/* ------------------------------------------------------------------ IDirect3D3 */

static HRESULT WINAPI d3_qi(void **i, REFIID r, void **o);
static ULONG WINAPI d3_addref(void **i) { return add_ref(&FROM(ddraw, vt3d, i)->ref); }
static ULONG WINAPI d3_release(void **i) { LONG n = InterlockedDecrement(&FROM(ddraw, vt3d, i)->ref); return (ULONG)(n < 0 ? 0 : n); }

static HRESULT WINAPI d3_enum_devices(void **i, LPD3DENUMDEVICESCALLBACK cb, LPVOID ctx)
{
    D3DDEVICEDESC hw, hel;
    (void)i;
    if (!cb)
        return DDERR_INVALIDPARAMS;
    hw.dwSize = sizeof hw;
    hel.dwSize = sizeof hel;
    fill_device_desc(&hw);
    fill_device_desc(&hel);
    cb((LPGUID)&IID_IDirect3DHALDevice, "Direct3D HAL", "Modernization renderer", &hw, &hel, ctx);
    return D3D_OK;
}

static HRESULT WINAPI d3_create_material(void **i, void **out, IUnknown *u)
{
    material *m = (material *)calloc(1, sizeof *m);
    (void)i; (void)u;
    if (!out || !m)
        return DDERR_OUTOFMEMORY;
    m->vt = vt_mat3;
    m->ref = 1;
    if (g_nmat < 64)
        g_materials[g_nmat++] = m;
    *out = &m->vt;
    return D3D_OK;
}

static HRESULT WINAPI d3_create_viewport(void **i, void **out, IUnknown *u)
{
    viewport *v = (viewport *)calloc(1, sizeof *v);
    (void)i; (void)u;
    if (!out || !v)
        return DDERR_OUTOFMEMORY;
    v->vt = vt_vp3;
    v->ref = 1;
    *out = &v->vt;
    return D3D_OK;
}

static HRESULT WINAPI d3_create_device(void **i, REFCLSID cls, void **target, void **out, IUnknown *u)
{
    ddraw *dd = FROM(ddraw, vt3d, i);
    device *d;
    (void)cls; (void)u;
    if (!out)
        return DDERR_INVALIDPARAMS;
    d = (device *)calloc(1, sizeof *d);
    if (!d)
        return DDERR_OUTOFMEMORY;
    d->vt = vt_dev3;
    d->ref = 1;
    d->dd = dd;
    d->target = target ? FROM(surface, vt4, target) : NULL;
    d->rs[D3DRENDERSTATE_ZENABLE] = 1;
    d->rs[D3DRENDERSTATE_ZWRITEENABLE] = 1;
    d->rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    d->rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    d->rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    d->rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    d->rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
    ensure_gfx(dd);
    *out = &d->vt;
    log_printf("ddshim: device created");
    return D3D_OK;
}

static HRESULT WINAPI d3_enum_z(void **i, REFCLSID cls, LPD3DENUMPIXELFORMATSCALLBACK cb, LPVOID ctx)
{
    DDPIXELFORMAT pf;
    (void)i; (void)cls;
    if (!cb)
        return DDERR_INVALIDPARAMS;
    set_pf_z16(&pf);
    cb(&pf, ctx);
    return D3D_OK;
}

static HRESULT WINAPI d3_ok1(void **i) { (void)i; return D3D_OK; }

/* ------------------------------------------------------------------ IDirectDraw / IDirectDraw4 */

static HRESULT dd_qi(ddraw *dd, REFIID r, void **o)
{
    if (!o)
        return E_POINTER;
    if (IsEqualIID(r, &IID_IDirectDraw4))
        *o = &dd->vt4;
    else if (IsEqualIID(r, &IID_IDirectDraw) || IsEqualIID(r, &IID_IUnknown))
        *o = &dd->vt1;
    else if (IsEqualIID(r, &IID_IDirect3D3))
        *o = &dd->vt3d;
    else {
        *o = NULL;
        return E_NOINTERFACE;
    }
    add_ref(&dd->ref);
    return S_OK;
}

static HRESULT WINAPI d1_qi(void **i, REFIID r, void **o) { return dd_qi(FROM(ddraw, vt1, i), r, o); }
static HRESULT WINAPI d4_qi(void **i, REFIID r, void **o) { return dd_qi(FROM(ddraw, vt4, i), r, o); }
static HRESULT WINAPI d3_qi(void **i, REFIID r, void **o) { return dd_qi(FROM(ddraw, vt3d, i), r, o); }
static ULONG WINAPI d1_addref(void **i) { return add_ref(&FROM(ddraw, vt1, i)->ref); }
static ULONG WINAPI d4_addref(void **i) { return add_ref(&FROM(ddraw, vt4, i)->ref); }
static ULONG WINAPI d1_release(void **i) { LONG n = InterlockedDecrement(&FROM(ddraw, vt1, i)->ref); return (ULONG)(n < 0 ? 0 : n); }
static ULONG WINAPI d4_release(void **i) { LONG n = InterlockedDecrement(&FROM(ddraw, vt4, i)->ref); return (ULONG)(n < 0 ? 0 : n); }

static HRESULT WINAPI d_create_clipper(void **i, DWORD f, void **out, IUnknown *u)
{
    clipper *c = (clipper *)calloc(1, sizeof *c);
    (void)i; (void)f; (void)u;
    if (!out || !c)
        return DDERR_OUTOFMEMORY;
    c->vt = vt_clip;
    c->ref = 1;
    *out = &c->vt;
    return DD_OK;
}

static HRESULT WINAPI d_get_caps(void **i, LPDDCAPS drv, LPDDCAPS hel)
{
    LPDDCAPS c[2];
    int k;
    (void)i;
    c[0] = drv;
    c[1] = hel;
    for (k = 0; k < 2; k++) {
        DWORD size;
        if (!c[k])
            continue;
        size = c[k]->dwSize ? c[k]->dwSize : sizeof(DDCAPS);
        memset(c[k], 0, size);
        c[k]->dwSize = size;
        c[k]->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_COLORKEY |
                       DDCAPS_BLTSTRETCH;
        c[k]->dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED;
        c[k]->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        c[k]->ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_FLIP |
                               DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY |
                               DDSCAPS_SYSTEMMEMORY;
        c[k]->dwVidMemTotal = c[k]->dwVidMemFree = 512u << 20;
    }
    return DD_OK;
}

static HWND g_game_hwnd;

/* Size and blank the window before the game has a device (the intro plays first).
 * Placed for the mode the game is about to set, so a windowed window keeps one
 * size from the intro on. */
static void prepare_window(HWND w)
{
    gfx_prepare_window(w, display_native_width(), 480, &g_opt);
}

static HRESULT WINAPI d_set_coop(void **i, HWND w, DWORD flags)
{
    ddraw *dd = FROM(ddraw, vt4, i);
    (void)flags;
    if (w) {
        dd->hwnd = w;
        g_game_hwnd = w;
        prepare_window(w);
    }
    return DD_OK;
}

static HRESULT WINAPI d1_set_coop(void **i, HWND w, DWORD flags)
{
    return d_set_coop((void **)&FROM(ddraw, vt1, i)->vt4, w, flags);
}

static HRESULT WINAPI d_set_mode(void **i, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD flags)
{
    ddraw *dd = FROM(ddraw, vt4, i);
    (void)rate; (void)flags;
    dd->mode_w = (int)w;
    dd->mode_h = (int)h;
    dd->mode_bpp = (int)bpp;
    log_printf("ddshim: game display mode %lux%lu", w, h);
    if (gfx_ready())
        gfx_set_mode((int)w, (int)h);
    return DD_OK;
}

static HRESULT WINAPI d1_set_mode(void **i, DWORD w, DWORD h, DWORD bpp)
{
    return d_set_mode((void **)&FROM(ddraw, vt1, i)->vt4, w, h, bpp, 0, 0);
}

static HRESULT WINAPI d_get_mode(void **i, LPDDSURFACEDESC2 d)
{
    ddraw *dd = FROM(ddraw, vt4, i);
    if (!d)
        return DDERR_INVALIDPARAMS;
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
    d->dwWidth = dd->mode_w ? dd->mode_w : 640;
    d->dwHeight = dd->mode_h ? dd->mode_h : 480;
    d->lPitch = d->dwWidth * 2;
    set_pf_565(&d->ddpfPixelFormat);
    return DD_OK;
}

static HRESULT WINAPI d_create_surface(void **i, LPDDSURFACEDESC2 desc, void **out, IUnknown *u)
{
    ddraw *dd = FROM(ddraw, vt4, i);
    DWORD caps = desc ? desc->ddsCaps.dwCaps : 0;
    DDPIXELFORMAT pf;
    surface *s;
    int w, h;
    (void)u;
    if (!desc || !out)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    w = (desc->dwFlags & DDSD_WIDTH) ? (int)desc->dwWidth : 0;
    h = (desc->dwFlags & DDSD_HEIGHT) ? (int)desc->dwHeight : 0;

    if (caps & DDSCAPS_PRIMARYSURFACE) {
        surface *back;
        set_pf_565(&pf);
        w = dd->mode_w ? dd->mode_w : 640;
        h = dd->mode_h ? dd->mode_h : 480;
        s = surf_new(dd, K_PRIMARY, w, h, &pf, caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY, 0);
        if (!s)
            return DDERR_OUTOFMEMORY;
        if ((desc->dwFlags & DDSD_BACKBUFFERCOUNT) && desc->dwBackBufferCount) {
            back = surf_new(dd, K_BACK, w, h, &pf, DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY, 0);
            s->attached = back;
        }
        ensure_gfx(dd);
        *out = &s->vt4;
        return DD_OK;
    }
    if (caps & DDSCAPS_ZBUFFER) {
        set_pf_z16(&pf);
        s = surf_new(dd, K_ZBUF, w, h, &pf, caps, 0);
        if (!s)
            return DDERR_OUTOFMEMORY;
        *out = &s->vt4;
        return DD_OK;
    }
    if (caps & (DDSCAPS_TEXTURE | DDSCAPS_OFFSCREENPLAIN)) {
        if (desc->dwFlags & DDSD_PIXELFORMAT)
            pf = desc->ddpfPixelFormat;
        else
            set_pf_565(&pf);
        if (!w || !h || pf.dwRGBBitCount < 16)
            return DDERR_INVALIDPIXELFORMAT;
        s = surf_new(dd, (caps & DDSCAPS_TEXTURE) ? K_TEXTURE : K_OFFSCREEN, w, h, &pf, caps,
                     desc->ddsCaps.dwCaps2);
        if (!s)
            return DDERR_OUTOFMEMORY;
        if (desc->dwFlags & DDSD_CKSRCBLT) {
            s->ck = desc->ddckCKSrcBlt;
            s->has_ck = 1;
        }
        *out = &s->vt4;
        return DD_OK;
    }
    log_printf("ddshim: CreateSurface with caps %08lX not supported", caps);
    return DDERR_INVALIDCAPS;
}

static HRESULT WINAPI d_vidmem(void **i, LPDDSCAPS2 c, LPDWORD total, LPDWORD freem)
{
    (void)i; (void)c;
    if (total) *total = 512u << 20;
    if (freem) *freem = 512u << 20;
    return DD_OK;
}

static HRESULT WINAPI d_ok1(void **i) { (void)i; return DD_OK; }

static HRESULT WINAPI d_identifier(void **i, LPDDDEVICEIDENTIFIER id, DWORD f)
{
    (void)i; (void)f;
    if (id) {
        memset(id, 0, sizeof *id);
        strcpy_s(id->szDriver, sizeof id->szDriver, "mml");
        strcpy_s(id->szDescription, sizeof id->szDescription, "Modernization renderer (Direct3D 11)");
    }
    return DD_OK;
}

/* ------------------------------------------------------------------ setup */

static void build_vtables(void)
{
    /* IDirectDraw (v1) */
    SET(vt_dd1, 0, d1_qi); SET(vt_dd1, 1, d1_addref); SET(vt_dd1, 2, d1_release);
    SET(vt_dd1, 4, d_create_clipper); SET(vt_dd1, 11, d_get_caps); SET(vt_dd1, 19, d_ok1);
    SET(vt_dd1, 20, d1_set_coop); SET(vt_dd1, 21, d1_set_mode);
    /* IDirectDraw4 */
    SET(vt_dd4, 0, d4_qi); SET(vt_dd4, 1, d4_addref); SET(vt_dd4, 2, d4_release);
    SET(vt_dd4, SLOT_CreateClipper, d_create_clipper); SET(vt_dd4, SLOT_CreateSurface, d_create_surface);
    SET(vt_dd4, SLOT_GetCaps, d_get_caps); SET(vt_dd4, SLOT_GetDisplayMode, d_get_mode);
    SET(vt_dd4, SLOT_RestoreDisplayMode, d_ok1); SET(vt_dd4, SLOT_SetCooperativeLevel, d_set_coop);
    SET(vt_dd4, SLOT_SetDisplayMode, d_set_mode); SET(vt_dd4, SLOT_GetAvailableVidMem, d_vidmem);
    SET(vt_dd4, SLOT_RestoreAllSurfaces, d_ok1); SET(vt_dd4, SLOT_TestCooperativeLevel, d_ok1);
    SET(vt_dd4, SLOT_GetDeviceIdentifier, d_identifier); SET(vt_dd4, SLOT_FlipToGDISurface, d_ok1);
    /* IDirect3D3 */
    SET(vt_d3d3, 0, d3_qi); SET(vt_d3d3, 1, d3_addref); SET(vt_d3d3, 2, d3_release);
    SET(vt_d3d3, 3, d3_enum_devices); SET(vt_d3d3, 5, d3_create_material); SET(vt_d3d3, 6, d3_create_viewport);
    SET(vt_d3d3, 8, d3_create_device); SET(vt_d3d3, 10, d3_enum_z); SET(vt_d3d3, 11, d3_ok1);
    /* IDirectDrawSurface4 */
    SET(vt_surf4, 0, s4_qi); SET(vt_surf4, 1, s4_addref); SET(vt_surf4, 2, s4_release);
    SET(vt_surf4, 3, s_add_attached); SET(vt_surf4, 5, s_blt); SET(vt_surf4, 7, s_bltfast);
    SET(vt_surf4, 11, s_flip); SET(vt_surf4, 12, s_get_attached); SET(vt_surf4, 13, s_ok2);
    SET(vt_surf4, 14, s_get_caps); SET(vt_surf4, 15, s_ok2); SET(vt_surf4, 16, s_get_ck);
    SET(vt_surf4, 18, s_ok2); SET(vt_surf4, 20, s_no_palette); SET(vt_surf4, 21, s_get_pf);
    SET(vt_surf4, 22, s_get_desc); SET(vt_surf4, 24, s_ok1); SET(vt_surf4, 25, s_lock);
    SET(vt_surf4, 27, s_ok1); SET(vt_surf4, 28, s_ok2); SET(vt_surf4, 29, s_set_ck); SET(vt_surf4, 32, s_unlock);
    SET(vt_surf4, 36, s_get_dd); SET(vt_surf4, 37, s_ok2); SET(vt_surf4, 38, s_ok2);
    SET(vt_surf4, 43, s_uniq); SET(vt_surf4, 44, s_ok1);
    /* IDirectDrawSurface (v1): the game gets one via QueryInterface; route to the same object */
    SET(vt_surf1, 0, s1_qi); SET(vt_surf1, 1, s1_addref); SET(vt_surf1, 2, s1_release);
    SET(vt_surf1, 12, s_get_attached_v1); SET(vt_surf1, 14, s_get_caps_v1); SET(vt_surf1, 20, s_no_palette);
    SET(vt_surf1, 21, s_get_pf_v1); SET(vt_surf1, 22, s_get_desc_v1); SET(vt_surf1, 24, s_ok1);
    SET(vt_surf1, 27, s_ok1); SET(vt_surf1, 32, s_unlock_v1);
    /* IDirect3DTexture2 */
    SET(vt_tex2, 0, t2_qi); SET(vt_tex2, 1, t2_addref); SET(vt_tex2, 2, t2_release);
    SET(vt_tex2, 3, t2_get_handle); SET(vt_tex2, 4, t2_palette_changed); SET(vt_tex2, 5, t2_load);
    /* IDirect3DDevice3 */
    SET(vt_dev3, 0, dev_qi); SET(vt_dev3, 1, dev_addref); SET(vt_dev3, 2, dev_release);
    SET(vt_dev3, 3, dev_get_caps); SET(vt_dev3, 5, dev_add_vp); SET(vt_dev3, 6, dev_ok1);
    SET(vt_dev3, 8, dev_enum_tex_formats); SET(vt_dev3, 9, dev_ok1); SET(vt_dev3, 10, dev_ok1);
    SET(vt_dev3, 11, dev_get_d3d); SET(vt_dev3, 12, dev_set_vp); SET(vt_dev3, 13, dev_get_vp);
    SET(vt_dev3, 21, dev_get_rs); SET(vt_dev3, 22, dev_set_rs); SET(vt_dev3, 28, dev_draw_prim);
    SET(vt_dev3, 37, dev_get_texture); SET(vt_dev3, 38, dev_set_texture); SET(vt_dev3, 39, dev_get_tss);
    SET(vt_dev3, 40, dev_set_tss); SET(vt_dev3, 41, dev_validate);
    /* IDirect3DViewport3 */
    SET(vt_vp3, 0, vp_qi); SET(vt_vp3, 1, vp_addref); SET(vt_vp3, 2, vp_release);
    SET(vt_vp3, 8, vp_set_bg); SET(vt_vp3, 12, vp_clear); SET(vt_vp3, 16, vp_get2); SET(vt_vp3, 17, vp_set2);
    SET(vt_vp3, 20, vp_clear2);
    /* IDirect3DMaterial3 */
    SET(vt_mat3, 0, mat_qi); SET(vt_mat3, 1, mat_addref); SET(vt_mat3, 2, mat_release);
    SET(vt_mat3, 3, mat_set); SET(vt_mat3, 4, mat_get); SET(vt_mat3, 5, mat_handle);
    /* IDirectDrawClipper */
    SET(vt_clip, 0, clip_qi); SET(vt_clip, 1, clip_addref); SET(vt_clip, 2, clip_release);
    SET(vt_clip, 8, clip_set_hwnd);
}

/* The intro video (CAPCOM.AVI) plays through MCI into the game window at a
 * fixed "put vfw destination at 0 0 640 480". Under DDrawCompat that 640x480
 * mode got stretched to the screen; with our renderer the window is already
 * monitor-sized, so fit the video to it instead (4:3, centred). */
static MCIERROR(WINAPI *g_real_mci)(LPCSTR, LPSTR, UINT, HWND);

static MCIERROR WINAPI shim_mci_send_string(LPCSTR cmd, LPSTR ret, UINT retlen, HWND cb)
{
    static const char prefix[] = "put vfw destination at";
    RECT rc;
    /* the intro setup resizes the window to 640x480 after SetCooperativeLevel,
     * so place it again right before the video gets its rectangle */
    if (cmd && _strnicmp(cmd, prefix, sizeof prefix - 1) == 0 && g_game_hwnd)
        prepare_window(g_game_hwnd);
    if (cmd && _strnicmp(cmd, prefix, sizeof prefix - 1) == 0 && g_game_hwnd && GetClientRect(g_game_hwnd, &rc) &&
        rc.right > 0 && rc.bottom > 0) {
        char fitted[96];
        int w = rc.right, h = rc.bottom, vw = w, vh = h;
        if (w * 3 > h * 4)
            vw = h * 4 / 3;
        else
            vh = w * 3 / 4;
        _snprintf_s(fitted, sizeof fitted, _TRUNCATE, "put vfw destination at %d %d %d %d",
                    (w - vw) / 2, (h - vh) / 2, vw, vh);
        log_printf("ddshim: intro video %dx%d at %d,%d (window %dx%d)", vw, vh, (w - vw) / 2, (h - vh) / 2, w, h);
        return g_real_mci(fitted, ret, retlen, cb);
    }
    return g_real_mci(cmd, ret, retlen, cb);
}

static HRESULT WINAPI shim_direct_draw_create(GUID *guid, void **out, IUnknown *u)
{
    ddraw *dd;
    (void)guid; (void)u;
    if (!out)
        return DDERR_INVALIDPARAMS;
    dd = (ddraw *)calloc(1, sizeof *dd);
    if (!dd)
        return DDERR_OUTOFMEMORY;
    dd->vt1 = vt_dd1;
    dd->vt4 = vt_dd4;
    dd->vt3d = vt_d3d3;
    dd->ref = 1;
    *out = &dd->vt1;
    return DD_OK;
}

static HRESULT WINAPI shim_direct_draw_enumerate(LPDDENUMCALLBACKA cb, LPVOID ctx)
{
    if (cb)
        cb(NULL, "Modernization renderer", "display", ctx);
    return DD_OK;
}

/* For our intro player: the renderer up before the game's first draw, in the
 * widescreen mode the game will use. */
int ddshim_video_begin(HWND w)
{
    if (!g_installed || !w)
        return 0;
    if (!gfx_ready()) {
        unsigned int cw;
        _controlfp_s(&cw, _PC_24, _MCW_PC);
        if (!gfx_init(w, display_native_width(), 480, &g_opt))
            return 0;
    }
    gfx_set_mode(display_native_width(), 480);
    return 1;
}

int ddshim_install(const gfx_options *opt)
{
    void *create = (void *)shim_direct_draw_create, *enumerate = (void *)shim_direct_draw_enumerate;
    void *mci = (void *)shim_mci_send_string;
    g_opt = *opt;
    g_real_mci = *(MCIERROR(WINAPI **)(LPCSTR, LPSTR, UINT, HWND))G_IAT_MCISENDSTRINGA;
    mem_write(G_IAT_MCISENDSTRINGA, &mci, sizeof mci);
    build_vtables();
    if (!mem_write(G_IAT_DIRECTDRAWCREATE, &create, sizeof create) ||
        !mem_write(G_IAT_DIRECTDRAWENUMERATE, &enumerate, sizeof enumerate))
        return 0;
    g_installed = 1;
    log_printf("ddshim: DirectDraw replaced by the Direct3D 11 renderer");
    return 1;
}

/* in-game menu: textures go through the upload (and HD replacement) again */
void ddshim_reupload_textures(void)
{
    surface *s;
    for (s = g_surfaces; s; s = s->next)
        if (s->kind == K_TEXTURE && s->mem)
            s->dirty = 1;
}
