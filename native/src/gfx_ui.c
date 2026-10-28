/* Our own UI over the final picture (the in-game menu): shapes and text in
 * window pixels, drawn after the game's image and any shader preset so it
 * stays sharp. Text comes from a coverage atlas rasterised with GDI at a size
 * that follows the window height (rebuilt when that changes): a body font and a
 * bolder display font for titles. The UI's owner registers a draw callback;
 * gfx_d3d11.c calls gfx_ui_render while composing every presented frame. */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_ui.h"
#include "log.h"

#include "shaders/ui_vs.h"
#include "shaders/ui_ps.h"

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

#define ATLAS_W 2048
#define ATLAS_H 2048
#define FIRST_CHAR 32
#define LAST_CHAR 126
#define MAX_VERTS 60000
#define NFONTS 2

typedef struct {
    float x, y, u, v;
    uint32_t color;   /* 0xAARRGGBB, read as B8G8R8A8 */
} ui_vert;

static struct {
    int ready, failed;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vb, *cb;
    ID3D11BlendState *blend;
    ID3D11SamplerState *smp;
    ID3D11RasterizerState *raster;
    ID3D11Texture2D *font;
    ID3D11ShaderResourceView *font_srv;
    int font_px;                       /* body line height the atlas was made for */
    float line[NFONTS];                /* line height of each font at scale 1 */
    struct { float u0, v0, u1, v1, w, h, adv, lead; } glyph[NFONTS][LAST_CHAR + 1];
    ui_vert verts[MAX_VERTS];
    int nverts;
    struct { gfx_ui_draw_fn fn; void *user; } draw[4];
    int ndraw;
} U;

void gfx_ui_add_draw(gfx_ui_draw_fn draw, void *user)
{
    if (U.ndraw < (int)(sizeof U.draw / sizeof U.draw[0])) {
        U.draw[U.ndraw].fn = draw;
        U.draw[U.ndraw].user = user;
        U.ndraw++;
    }
}

static int setup(ID3D11Device *dev)
{
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    ID3D11Device_CreateVertexShader(dev, g_vs_ui, sizeof g_vs_ui, NULL, &U.vs);
    ID3D11Device_CreatePixelShader(dev, g_ps_ui, sizeof g_ps_ui, NULL, &U.ps);
    ID3D11Device_CreateInputLayout(dev, il, 3, g_vs_ui, sizeof g_vs_ui, &U.layout);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof U.verts;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &U.vb);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;   /* filled with UpdateSubresource */
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = 0;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &U.cb);
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &bl, &U.blend);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &sd, &U.smp);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &U.raster);
    if (!U.vs || !U.ps || !U.layout || !U.vb || !U.cb || !U.blend || !U.smp || !U.raster) {
        log_printf("ui: setup failed");
        U.failed = 1;
        return 0;
    }
    U.ready = 1;
    return 1;
}

/* rasterise ASCII for each font with GDI into one coverage atlas (R8); font 0
 * is the body text at px line height, font 1 the display font at 1.5x */
static int make_font(ID3D11Device *dev, int px)
{
    static const struct { const char *face, *fallback; int weight, italic; float size; } F[NFONTS] = {
        { "Segoe UI", "Arial", FW_SEMIBOLD, 0, 1.0f },
        { "Bahnschrift", "Arial Black", FW_BOLD, 1, 1.5f },
    };
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    HGDIOBJ old_bmp;
    uint8_t *cov;
    int x = 8, y = 8, row_h = 0, c, f, ok = 0;
    if (!dc)
        return 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = ATLAS_W;
    bi.bmiHeader.biHeight = -ATLAS_H;   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    cov = (uint8_t *)malloc((size_t)ATLAS_W * ATLAS_H);
    if (!bmp || !cov) {
        if (bmp) DeleteObject(bmp);
        free(cov);
        DeleteDC(dc);
        return 0;
    }
    old_bmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    memset(bits, 0, (size_t)ATLAS_W * ATLAS_H * 4);
    for (f = 0; f < NFONTS; f++) {
        int size = (int)(px * F[f].size + 0.5f);
        HFONT font = CreateFontA(-size, 0, 0, 0, F[f].weight, (DWORD)F[f].italic, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, F[f].face);
        HFONT old_font;
        TEXTMETRICA tm;
        char face[64];
        if (font) {   /* GDI quietly substitutes a missing face: check, then use the fallback */
            old_font = (HFONT)SelectObject(dc, font);
            GetTextFaceA(dc, sizeof face, face);
            SelectObject(dc, old_font);
            if (_stricmp(face, F[f].face) != 0) {
                DeleteObject(font);
                font = CreateFontA(-size, 0, 0, 0, F[f].weight, (DWORD)F[f].italic, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                                   F[f].fallback);
            }
        }
        if (!font)
            continue;
        old_font = (HFONT)SelectObject(dc, font);
        GetTextMetricsA(dc, &tm);
        U.line[f] = (float)tm.tmHeight;
        if (x > 8) {   /* each font starts a new row */
            x = 8;
            y += row_h + 4;
        }
        row_h = tm.tmHeight;
        for (c = FIRST_CHAR; c <= LAST_CHAR; c++) {
            char ch = (char)c;
            SIZE sz;
            int w, lead = tm.tmHeight / 8;
            GetTextExtentPoint32A(dc, &ch, 1, &sz);
            w = sz.cx + 2 * lead + 4;   /* room for italic/overhang and filtering */
            if (x + w > ATLAS_W) {
                x = 8;
                y += row_h + 4;
            }
            if (y + tm.tmHeight > ATLAS_H)
                break;
            TextOutA(dc, x + lead, y, &ch, 1);
            U.glyph[f][c].u0 = (float)x / ATLAS_W;
            U.glyph[f][c].v0 = (float)y / ATLAS_H;
            U.glyph[f][c].u1 = (float)(x + w) / ATLAS_W;
            U.glyph[f][c].v1 = (float)(y + tm.tmHeight) / ATLAS_H;
            U.glyph[f][c].w = (float)w;
            U.glyph[f][c].h = (float)tm.tmHeight;
            U.glyph[f][c].adv = (float)sz.cx;
            U.glyph[f][c].lead = (float)lead;
            x += w + 2;
        }
        SelectObject(dc, old_font);
        DeleteObject(font);
    }
    GdiFlush();
    for (c = 0; c < ATLAS_W * ATLAS_H; c++)   /* coverage (any channel) */
        cov[c] = (uint8_t)(((uint32_t *)bits)[c] & 0xFF);
    for (c = 0; c < 4; c++)   /* a solid 4x4 block at (0,0) for plain shapes */
        memset(cov + c * ATLAS_W, 0xFF, 4);
    {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA init;
        memset(&td, 0, sizeof td);
        td.Width = ATLAS_W;
        td.Height = ATLAS_H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = cov;
        init.SysMemPitch = ATLAS_W;
        init.SysMemSlicePitch = 0;
        REL(U.font_srv);
        REL(U.font);
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &init, &U.font)) &&
            SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)U.font, NULL, &U.font_srv)))
            ok = 1;
    }
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    free(cov);
    if (ok)
        U.font_px = px;
    return ok;
}

#define SOLID_UV (1.5f / ATLAS_W)

static void vert(ui_vert *v, float x, float y, float u, float t, uint32_t c)
{
    v->x = x;
    v->y = y;
    v->u = u;
    v->v = t;
    v->color = c;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 uint32_t top, uint32_t bottom)
{
    ui_vert *v;
    if (U.nverts + 6 > MAX_VERTS)
        return;
    v = U.verts + U.nverts;
    vert(v + 0, x0, y0, u0, v0, top);
    vert(v + 1, x1, y0, u1, v0, top);
    vert(v + 2, x0, y1, u0, v1, bottom);
    v[3] = v[2];
    v[4] = v[1];
    vert(v + 5, x1, y1, u1, v1, bottom);
    U.nverts += 6;
}

void gfx_ui_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb)
{
    ui_vert *v;
    if (U.nverts + 3 > MAX_VERTS)
        return;
    v = U.verts + U.nverts;
    vert(v + 0, x0, y0, SOLID_UV, SOLID_UV, argb);
    vert(v + 1, x1, y1, SOLID_UV, SOLID_UV, argb);
    vert(v + 2, x2, y2, SOLID_UV, SOLID_UV, argb);
    U.nverts += 3;
}

void gfx_ui_quad(const float xy[8], const uint32_t c[4])
{
    static const int order[6] = { 0, 1, 3, 3, 1, 2 };
    ui_vert *v;
    int i;
    if (U.nverts + 6 > MAX_VERTS)
        return;
    v = U.verts + U.nverts;
    for (i = 0; i < 6; i++)
        vert(v + i, xy[order[i] * 2], xy[order[i] * 2 + 1], SOLID_UV, SOLID_UV, c[order[i]]);
    U.nverts += 6;
}

void gfx_ui_poly(const float *xy, int n, uint32_t argb)
{
    int i;
    for (i = 1; i + 1 < n; i++)
        gfx_ui_tri(xy[0], xy[1], xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], argb);
}

void gfx_ui_rect(float x, float y, float w, float h, uint32_t argb)
{
    quad(x, y, x + w, y + h, SOLID_UV, SOLID_UV, SOLID_UV, SOLID_UV, argb, argb);
}

void gfx_ui_vgrad(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
    quad(x, y, x + w, y + h, SOLID_UV, SOLID_UV, SOLID_UV, SOLID_UV, top, bottom);
}

void gfx_ui_hgrad(float x, float y, float w, float h, uint32_t left, uint32_t right)
{
    float xy[8];
    uint32_t c[4];
    xy[0] = x;     xy[1] = y;
    xy[2] = x + w; xy[3] = y;
    xy[4] = x + w; xy[5] = y + h;
    xy[6] = x;     xy[7] = y + h;
    c[0] = c[3] = left;
    c[1] = c[2] = right;
    gfx_ui_quad(xy, c);
}

static int segments(float r)
{
    return r < 6 ? 12 : r < 20 ? 20 : 32;
}

void gfx_ui_circle(float cx, float cy, float r, uint32_t argb)
{
    int i, n = segments(r);
    for (i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        gfx_ui_tri(cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, argb);
    }
}

void gfx_ui_ring(float cx, float cy, float r, float thick, uint32_t argb)
{
    int i, n = segments(r);
    float ri = r - thick, xy[8];
    uint32_t c[4];
    c[0] = c[1] = c[2] = c[3] = argb;
    for (i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        xy[0] = cx + cosf(a0) * ri; xy[1] = cy + sinf(a0) * ri;
        xy[2] = cx + cosf(a0) * r;  xy[3] = cy + sinf(a0) * r;
        xy[4] = cx + cosf(a1) * r;  xy[5] = cy + sinf(a1) * r;
        xy[6] = cx + cosf(a1) * ri; xy[7] = cy + sinf(a1) * ri;
        gfx_ui_quad(xy, c);
    }
}

float gfx_ui_line(float scale)
{
    return U.font_px * scale;
}

float gfx_ui_font_line(int font, float scale)
{
    return U.line[font & 1] * scale;
}

float gfx_ui_font_width(int font, const char *s, float scale)
{
    float w = 0;
    font &= 1;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c >= FIRST_CHAR && c <= LAST_CHAR)
            w += U.glyph[font][c].adv * scale;
    }
    return w;
}

float gfx_ui_font_text(int font, float x, float y, const char *s, uint32_t top, uint32_t bottom, float scale)
{
    float x0 = x;
    font &= 1;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < FIRST_CHAR || c > LAST_CHAR)
            continue;
        if (c != ' ') {
            float gx = x - U.glyph[font][c].lead * scale;
            quad(gx, y, gx + U.glyph[font][c].w * scale, y + U.glyph[font][c].h * scale,
                 U.glyph[font][c].u0, U.glyph[font][c].v0, U.glyph[font][c].u1, U.glyph[font][c].v1, top, bottom);
        }
        x += U.glyph[font][c].adv * scale;
    }
    return x - x0;
}

float gfx_ui_text_width(const char *s, float scale)
{
    return gfx_ui_font_width(0, s, scale);
}

float gfx_ui_text(float x, float y, const char *s, uint32_t argb, float scale)
{
    return gfx_ui_font_text(0, x, y, s, argb, argb, scale);
}

void gfx_ui_render(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, int w, int h)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    UINT stride = sizeof(ui_vert), off = 0;
    float cb[4];
    int px, i;
    if (!U.ndraw || U.failed || w <= 0 || h <= 0)
        return;
    if (!U.ready && !setup(dev))
        return;
    px = h / 30 < 14 ? 14 : h / 30 > 96 ? 96 : h / 30;   /* text follows the window size */
    if (px != U.font_px && !make_font(dev, px)) {
        log_printf("ui: font failed");
        U.failed = 1;
        return;
    }
    U.nverts = 0;
    for (i = 0; i < U.ndraw; i++)
        U.draw[i].fn(U.draw[i].user, w, h);
    if (!U.nverts)
        return;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)U.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, U.verts, sizeof(ui_vert) * (size_t)U.nverts);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)U.vb, 0);
    cb[0] = 1.0f / w;
    cb[1] = 1.0f / h;
    cb[2] = cb[3] = 0;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)U.cb, 0, NULL, cb, 0, 0);
    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, U.raster);
    ID3D11DeviceContext_OMSetBlendState(ctx, U.blend, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, U.layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &U.vb, &stride, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, U.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &U.cb);
    ID3D11DeviceContext_PSSetShader(ctx, U.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &U.font_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &U.smp);
    ID3D11DeviceContext_Draw(ctx, (UINT)U.nverts, 0);
}
