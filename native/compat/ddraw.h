#pragma once
/* The DirectDraw and Direct3D (DirectX 6/7) types, constants and interface IDs ddshim uses, for
 * platforms without the DirectX headers. The structs follow the documented layouts the game was
 * built against (it reads and writes them itself); tools/compat/dx_check.py compares every size,
 * member offset and value with the Windows SDK's headers. */
#include <windows.h>

typedef float D3DVALUE, *LPD3DVALUE;
typedef DWORD D3DCOLOR;
typedef DWORD D3DMATERIALHANDLE, *LPD3DMATERIALHANDLE;
typedef DWORD D3DTEXTUREHANDLE, *LPD3DTEXTUREHANDLE;
typedef DWORD D3DCOLORMODEL;
typedef DWORD D3DPRIMITIVETYPE;
typedef DWORD D3DRENDERSTATETYPE;
typedef DWORD D3DTEXTURESTAGESTATETYPE;
#define D3DCOLOR_MONO 1   /* D3DCOLORMODEL */
#define D3DCOLOR_RGB 2

/* the interfaces: opaque here (ddshim's objects are its own) */
typedef struct IDirectDraw IDirectDraw, *LPDIRECTDRAW;
typedef struct IDirectDraw4 IDirectDraw4, *LPDIRECTDRAW4;
typedef struct IDirectDrawSurface IDirectDrawSurface, *LPDIRECTDRAWSURFACE;
typedef struct IDirectDrawSurface4 IDirectDrawSurface4, *LPDIRECTDRAWSURFACE4;
typedef struct IDirectDrawClipper IDirectDrawClipper, *LPDIRECTDRAWCLIPPER;
typedef struct IDirectDrawPalette IDirectDrawPalette, *LPDIRECTDRAWPALETTE;
typedef struct IDirect3D3 IDirect3D3, *LPDIRECT3D3;
typedef struct IDirect3DDevice3 IDirect3DDevice3, *LPDIRECT3DDEVICE3;
typedef struct IDirect3DTexture2 IDirect3DTexture2, *LPDIRECT3DTEXTURE2;
typedef struct IDirect3DViewport3 IDirect3DViewport3, *LPDIRECT3DVIEWPORT3;
typedef struct IDirect3DMaterial3 IDirect3DMaterial3, *LPDIRECT3DMATERIAL3;

typedef struct {
    DWORD dwColorSpaceLowValue, dwColorSpaceHighValue;
} DDCOLORKEY, *LPDDCOLORKEY;

typedef struct {
    DWORD dwCaps;
} DDSCAPS, *LPDDSCAPS;

typedef struct {
    DWORD dwCaps, dwCaps2, dwCaps3;
    union { DWORD dwCaps4, dwVolumeDepth; };
} DDSCAPS2, *LPDDSCAPS2;

typedef struct {
    DWORD dwSize, dwFlags, dwFourCC;
    union { DWORD dwRGBBitCount, dwYUVBitCount, dwZBufferBitDepth, dwAlphaBitDepth, dwLuminanceBitCount,
                  dwBumpBitCount, dwPrivateFormatBitCount; };
    union { DWORD dwRBitMask, dwYBitMask, dwStencilBitDepth, dwLuminanceBitMask, dwBumpDuBitMask, dwOperations; };
    union { DWORD dwGBitMask, dwUBitMask, dwZBitMask, dwBumpDvBitMask; struct { WORD wFlipMSTypes, wBltMSTypes; }
                  MultiSampleCaps; };
    union { DWORD dwBBitMask, dwVBitMask, dwStencilBitMask, dwBumpLuminanceBitMask; };
    union { DWORD dwRGBAlphaBitMask, dwYUVAlphaBitMask, dwLuminanceAlphaBitMask, dwRGBZBitMask, dwYUVZBitMask; };
} DDPIXELFORMAT, *LPDDPIXELFORMAT;

typedef struct {
    DWORD dwSize, dwFlags, dwHeight, dwWidth;
    union { LONG lPitch; DWORD dwLinearSize; };
    DWORD dwBackBufferCount;
    union { DWORD dwMipMapCount, dwZBufferBitDepth, dwRefreshRate; };
    DWORD dwAlphaBitDepth, dwReserved;
    LPVOID lpSurface;
    DDCOLORKEY ddckCKDestOverlay, ddckCKDestBlt, ddckCKSrcOverlay, ddckCKSrcBlt;
    DDPIXELFORMAT ddpfPixelFormat;
    DDSCAPS ddsCaps;
} DDSURFACEDESC, *LPDDSURFACEDESC;

typedef struct {
    DWORD dwSize, dwFlags, dwHeight, dwWidth;
    union { LONG lPitch; DWORD dwLinearSize; };
    union { DWORD dwBackBufferCount, dwDepth; };
    union { DWORD dwMipMapCount, dwRefreshRate, dwSrcVBHandle; };
    DWORD dwAlphaBitDepth, dwReserved;
    LPVOID lpSurface;
    union { DDCOLORKEY ddckCKDestOverlay; DWORD dwEmptyFaceColor; };
    DDCOLORKEY ddckCKDestBlt, ddckCKSrcOverlay, ddckCKSrcBlt;
    union { DDPIXELFORMAT ddpfPixelFormat; DWORD dwFVF; };
    DDSCAPS2 ddsCaps;
    DWORD dwTextureStage;
} DDSURFACEDESC2, *LPDDSURFACEDESC2;

typedef struct {
    DWORD dwSize, dwDDFX, dwROP, dwDDROP, dwRotationAngle, dwZBufferOpCode, dwZBufferLow, dwZBufferHigh,
        dwZBufferBaseDest, dwZDestConstBitDepth;
    union { DWORD dwZDestConst; LPDIRECTDRAWSURFACE lpDDSZBufferDest; };
    DWORD dwZSrcConstBitDepth;
    union { DWORD dwZSrcConst; LPDIRECTDRAWSURFACE lpDDSZBufferSrc; };
    DWORD dwAlphaEdgeBlendBitDepth, dwAlphaEdgeBlend, dwReserved, dwAlphaDestConstBitDepth;
    union { DWORD dwAlphaDestConst; LPDIRECTDRAWSURFACE lpDDSAlphaDest; };
    DWORD dwAlphaSrcConstBitDepth;
    union { DWORD dwAlphaSrcConst; LPDIRECTDRAWSURFACE lpDDSAlphaSrc; };
    union { DWORD dwFillColor, dwFillDepth, dwFillPixel; LPDIRECTDRAWSURFACE lpDDSPattern; };
    DDCOLORKEY ddckDestColorkey, ddckSrcColorkey;
} DDBLTFX, *LPDDBLTFX;

typedef struct {   /* DDCAPS_DX7 (380 bytes) */
    DWORD dwSize, dwCaps, dwCaps2, dwCKeyCaps, dwFXCaps, dwFXAlphaCaps, dwPalCaps, dwSVCaps,
        dwAlphaBltConstBitDepths, dwAlphaBltPixelBitDepths, dwAlphaBltSurfaceBitDepths,
        dwAlphaOverlayConstBitDepths, dwAlphaOverlayPixelBitDepths, dwAlphaOverlaySurfaceBitDepths,
        dwZBufferBitDepths, dwVidMemTotal, dwVidMemFree, dwMaxVisibleOverlays, dwCurrVisibleOverlays,
        dwNumFourCCCodes, dwAlignBoundarySrc, dwAlignSizeSrc, dwAlignBoundaryDest, dwAlignSizeDest,
        dwAlignStrideAlign, dwRops[8];
    DDSCAPS ddsOldCaps;
    DWORD dwMinOverlayStretch, dwMaxOverlayStretch, dwMinLiveVideoStretch, dwMaxLiveVideoStretch,
        dwMinHwCodecStretch, dwMaxHwCodecStretch, dwReserved1, dwReserved2, dwReserved3, dwSVBCaps, dwSVBCKeyCaps,
        dwSVBFXCaps, dwSVBRops[8], dwVSBCaps, dwVSBCKeyCaps, dwVSBFXCaps, dwVSBRops[8], dwSSBCaps, dwSSBCKeyCaps,
        dwSSBFXCaps, dwSSBRops[8], dwMaxVideoPorts, dwCurrVideoPorts, dwSVBCaps2, dwNLVBCaps, dwNLVBCaps2,
        dwNLVBCKeyCaps, dwNLVBFXCaps, dwNLVBRops[8];
    DDSCAPS2 ddsCaps;
} DDCAPS, *LPDDCAPS;

typedef struct {
    char szDriver[512], szDescription[512];
    LARGE_INTEGER liDriverVersion;
    DWORD dwVendorId, dwDeviceId, dwSubSysId, dwRevision;
    GUID guidDeviceIdentifier;
} DDDEVICEIDENTIFIER, *LPDDDEVICEIDENTIFIER;

typedef struct {
    union { D3DVALUE r, dvR; };
    union { D3DVALUE g, dvG; };
    union { D3DVALUE b, dvB; };
    union { D3DVALUE a, dvA; };
} D3DCOLORVALUE;

typedef struct {
    DWORD dwSize;
    union { D3DCOLORVALUE diffuse, dcvDiffuse; };
    union { D3DCOLORVALUE ambient, dcvAmbient; };
    union { D3DCOLORVALUE specular, dcvSpecular; };
    union { D3DCOLORVALUE emissive, dcvEmissive; };
    union { D3DVALUE power, dvPower; };
    D3DTEXTUREHANDLE hTexture;
    DWORD dwRampSize;
} D3DMATERIAL, *LPD3DMATERIAL;

typedef struct {
    DWORD dwSize, dwX, dwY, dwWidth, dwHeight;
    D3DVALUE dvClipX, dvClipY, dvClipWidth, dvClipHeight, dvMinZ, dvMaxZ;
} D3DVIEWPORT2, *LPD3DVIEWPORT2;

typedef struct {
    union { LONG x1, lX1; };
    union { LONG y1, lY1; };
    union { LONG x2, lX2; };
    union { LONG y2, lY2; };
} D3DRECT, *LPD3DRECT;

typedef struct {
    DWORD dwSize, dwMiscCaps, dwRasterCaps, dwZCmpCaps, dwSrcBlendCaps, dwDestBlendCaps, dwAlphaCmpCaps,
        dwShadeCaps, dwTextureCaps, dwTextureFilterCaps, dwTextureBlendCaps, dwTextureAddressCaps, dwStippleWidth,
        dwStippleHeight;
} D3DPRIMCAPS;

typedef struct { DWORD dwSize, dwCaps; } D3DTRANSFORMCAPS;
typedef struct { DWORD dwSize, dwCaps, dwLightingModel, dwNumLights; } D3DLIGHTINGCAPS;

typedef struct {   /* (DirectX 7 headers: 252 bytes) */
    DWORD dwSize, dwFlags;
    D3DCOLORMODEL dcmColorModel;
    DWORD dwDevCaps;
    D3DTRANSFORMCAPS dtcTransformCaps;
    BOOL bClipping;
    D3DLIGHTINGCAPS dlcLightingCaps;
    D3DPRIMCAPS dpcLineCaps, dpcTriCaps;
    DWORD dwDeviceRenderBitDepth, dwDeviceZBufferBitDepth, dwMaxBufferSize, dwMaxVertexCount;
    DWORD dwMinTextureWidth, dwMinTextureHeight, dwMaxTextureWidth, dwMaxTextureHeight, dwMinStippleWidth,
        dwMaxStippleWidth, dwMinStippleHeight, dwMaxStippleHeight;
    DWORD dwMaxTextureRepeat, dwMaxTextureAspectRatio, dwMaxAnisotropy;
    D3DVALUE dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight, dvGuardBandBottom, dvExtentsAdjust;
    DWORD dwStencilCaps, dwFVFCaps, dwTextureOpCaps;
    WORD wMaxTextureBlendStages, wMaxSimultaneousTextures;
} D3DDEVICEDESC, *LPD3DDEVICEDESC;

typedef BOOL(WINAPI *LPDDENUMCALLBACKA)(GUID *guid, LPSTR description, LPSTR name, LPVOID ctx);
typedef HRESULT(CALLBACK *LPD3DENUMDEVICESCALLBACK)(GUID *guid, LPSTR description, LPSTR name, LPD3DDEVICEDESC hw,
                                                    LPD3DDEVICEDESC hel, LPVOID ctx);
typedef HRESULT(CALLBACK *LPD3DENUMPIXELFORMATSCALLBACK)(LPDDPIXELFORMAT pf, LPVOID ctx);

/* values as the Windows SDK defines them */
#define D3DBLEND_INVSRCALPHA                         0x00000006u
#define D3DBLEND_ONE                                 0x00000002u
#define D3DBLEND_SRCALPHA                            0x00000005u
#define D3DBLEND_ZERO                                0x00000001u
#define D3DCLEAR_TARGET                              0x00000001u
#define D3DCLEAR_ZBUFFER                             0x00000002u
#define D3DCMP_ALWAYS                                0x00000008u
#define D3DCMP_LESSEQUAL                             0x00000004u
#define D3DDD_COLORMODEL                             0x00000001u
#define D3DDD_DEVCAPS                                0x00000002u
#define D3DDD_DEVICERENDERBITDEPTH                   0x00000080u
#define D3DDD_DEVICEZBUFFERBITDEPTH                  0x00000100u
#define D3DDD_LINECAPS                               0x00000020u
#define D3DDD_TRICAPS                                0x00000040u
#define D3DDEVCAPS_CANRENDERAFTERFLIP                0x00000800u
#define D3DDEVCAPS_DRAWPRIMITIVES2                   0x00002000u
#define D3DDEVCAPS_DRAWPRIMTLVERTEX                  0x00000400u
#define D3DDEVCAPS_EXECUTESYSTEMMEMORY               0x00000010u
#define D3DDEVCAPS_FLOATTLVERTEX                     0x00000001u
#define D3DDEVCAPS_HWRASTERIZATION                   0x00080000u
#define D3DDEVCAPS_TEXTUREVIDEOMEMORY                0x00000200u
#define D3DDEVCAPS_TLVERTEXSYSTEMMEMORY              0x00000040u
#define D3DENUMRET_CANCEL                            0x00000000u
#define D3DFOG_NONE                                  0x00000000u
#define D3DFVF_DIFFUSE                               0x00000040u
#define D3DFVF_SPECULAR                              0x00000080u
#define D3DFVF_TEX1                                  0x00000100u
#define D3DFVF_XYZRHW                                0x00000004u
#define D3DPMISCCAPS_CULLCCW                         0x00000040u
#define D3DPMISCCAPS_CULLCW                          0x00000020u
#define D3DPMISCCAPS_CULLNONE                        0x00000010u
#define D3DPRASTERCAPS_DITHER                        0x00000001u
#define D3DPRASTERCAPS_FOGTABLE                      0x00000100u
#define D3DPRASTERCAPS_FOGVERTEX                     0x00000080u
#define D3DPRASTERCAPS_SUBPIXEL                      0x00000020u
#define D3DPRASTERCAPS_ZBIAS                         0x00004000u
#define D3DPRASTERCAPS_ZTEST                         0x00000010u
#define D3DPSHADECAPS_ALPHAFLATBLEND                 0x00001000u
#define D3DPSHADECAPS_ALPHAGOURAUDBLEND              0x00004000u
#define D3DPSHADECAPS_COLORFLATRGB                   0x00000002u
#define D3DPSHADECAPS_COLORGOURAUDRGB                0x00000008u
#define D3DPSHADECAPS_FOGGOURAUD                     0x00080000u
#define D3DPSHADECAPS_SPECULARGOURAUDRGB             0x00000200u
#define D3DPTADDRESSCAPS_CLAMP                       0x00000004u
#define D3DPTADDRESSCAPS_INDEPENDENTUV               0x00000010u
#define D3DPTADDRESSCAPS_WRAP                        0x00000001u
#define D3DPTBLENDCAPS_COPY                          0x00000040u
#define D3DPTBLENDCAPS_DECAL                         0x00000001u
#define D3DPTBLENDCAPS_MODULATE                      0x00000002u
#define D3DPTBLENDCAPS_MODULATEALPHA                 0x00000008u
#define D3DPTEXTURECAPS_ALPHA                        0x00000004u
#define D3DPTEXTURECAPS_PERSPECTIVE                  0x00000001u
#define D3DPTEXTURECAPS_POW2                         0x00000002u
#define D3DPTEXTURECAPS_TRANSPARENCY                 0x00000008u
#define D3DPTFILTERCAPS_LINEAR                       0x00000002u
#define D3DPTFILTERCAPS_MAGFLINEAR                   0x02000000u
#define D3DPTFILTERCAPS_MAGFPOINT                    0x01000000u
#define D3DPTFILTERCAPS_MINFLINEAR                   0x00000200u
#define D3DPTFILTERCAPS_MINFPOINT                    0x00000100u
#define D3DPTFILTERCAPS_NEAREST                      0x00000001u
#define D3DPT_TRIANGLELIST                           0x00000004u
#define D3DPT_TRIANGLESTRIP                          0x00000005u
#define D3DRENDERSTATE_ALPHABLENDENABLE              0x0000001Bu
#define D3DRENDERSTATE_ALPHAFUNC                     0x00000019u
#define D3DRENDERSTATE_ALPHAREF                      0x00000018u
#define D3DRENDERSTATE_ALPHATESTENABLE               0x0000000Fu
#define D3DRENDERSTATE_DESTBLEND                     0x00000014u
#define D3DRENDERSTATE_FOGCOLOR                      0x00000022u
#define D3DRENDERSTATE_FOGENABLE                     0x0000001Cu
#define D3DRENDERSTATE_FOGTABLEEND                   0x00000025u
#define D3DRENDERSTATE_FOGTABLEMODE                  0x00000023u
#define D3DRENDERSTATE_FOGTABLESTART                 0x00000024u
#define D3DRENDERSTATE_SHADEMODE                     0x00000009u
#define D3DRENDERSTATE_SRCBLEND                      0x00000013u
#define D3DRENDERSTATE_ZENABLE                       0x00000007u
#define D3DRENDERSTATE_ZFUNC                         0x00000017u
#define D3DRENDERSTATE_ZWRITEENABLE                  0x0000000Eu
#define D3DSHADE_FLAT                                0x00000001u
#define D3DSHADE_GOURAUD                             0x00000002u
#define D3DTFG_LINEAR                                0x00000002u
#define D3DTSS_MAGFILTER                             0x00000010u
#define D3D_OK                                       ((HRESULT)0x00000000u)
#define DDBD_16                                      0x00000400u
#define DDBD_24                                      0x00000200u
#define DDBD_32                                      0x00000100u
#define DDBLT_COLORFILL                              0x00000400u
#define DDBLT_DEPTHFILL                              0x02000000u
#define DDCAPS2_NOPAGELOCKREQUIRED                   0x00000800u
#define DDCAPS2_WIDESURFACES                         0x00001000u
#define DDCAPS_3D                                    0x00000001u
#define DDCAPS_BLT                                   0x00000040u
#define DDCAPS_BLTCOLORFILL                          0x04000000u
#define DDCAPS_BLTDEPTHFILL                          0x10000000u
#define DDCAPS_BLTSTRETCH                            0x00000200u
#define DDCAPS_COLORKEY                              0x00400000u
#define DDCKEYCAPS_SRCBLT                            0x00000200u
#define DDCKEY_SRCBLT                                0x00000008u
#define DDERR_INVALIDCAPS                            ((HRESULT)0x88760064u)
#define DDERR_INVALIDPARAMS                          ((HRESULT)0x80070057u)
#define DDERR_INVALIDPIXELFORMAT                     ((HRESULT)0x88760091u)
#define DDERR_NOCOLORKEY                             ((HRESULT)0x887600D7u)
#define DDERR_NOPALETTEATTACHED                      ((HRESULT)0x8876023Cu)
#define DDERR_NOTFOUND                               ((HRESULT)0x887600FFu)
#define DDERR_OUTOFMEMORY                            ((HRESULT)0x8007000Eu)
#define DDERR_UNSUPPORTED                            ((HRESULT)0x80004001u)
#define DDPF_ALPHAPIXELS                             0x00000001u
#define DDPF_RGB                                     0x00000040u
#define DDPF_ZBUFFER                                 0x00000400u
#define DDSCAPS_3DDEVICE                             0x00002000u
#define DDSCAPS_BACKBUFFER                           0x00000004u
#define DDSCAPS_FLIP                                 0x00000010u
#define DDSCAPS_FRONTBUFFER                          0x00000020u
#define DDSCAPS_OFFSCREENPLAIN                       0x00000040u
#define DDSCAPS_PRIMARYSURFACE                       0x00000200u
#define DDSCAPS_SYSTEMMEMORY                         0x00000800u
#define DDSCAPS_TEXTURE                              0x00001000u
#define DDSCAPS_VIDEOMEMORY                          0x00004000u
#define DDSCAPS_ZBUFFER                              0x00020000u
#define DDSCL_FPUSETUP                               0x00000800u
#define DDSD_BACKBUFFERCOUNT                         0x00000020u
#define DDSD_CAPS                                    0x00000001u
#define DDSD_CKSRCBLT                                0x00010000u
#define DDSD_HEIGHT                                  0x00000002u
#define DDSD_LPSURFACE                               0x00000800u
#define DDSD_PITCH                                   0x00000008u
#define DDSD_PIXELFORMAT                             0x00001000u
#define DDSD_WIDTH                                   0x00000004u
#define DD_OK                                        ((HRESULT)0x00000000u)
#define MCIERR_DEVICE_NOT_INSTALLED                  0x00000132u

DEFINE_GUID(IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(IID_IDirect3D3, 0xBB223240, 0xE72B, 0x11D0, 0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E);
DEFINE_GUID(IID_IDirect3DHALDevice, 0x84E63DE0, 0x46AA, 0x11CF, 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E);
DEFINE_GUID(IID_IDirect3DTexture, 0x2CDCD9E0, 0x25A0, 0x11CF, 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56);
DEFINE_GUID(IID_IDirect3DTexture2, 0x93281502, 0x8CF8, 0x11D0, 0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29);
DEFINE_GUID(IID_IDirectDraw, 0x6C14DB80, 0xA733, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
DEFINE_GUID(IID_IDirectDraw4, 0x9C59509A, 0x39BD, 0x11D1, 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5);
DEFINE_GUID(IID_IDirectDrawSurface, 0x6C14DB81, 0xA733, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
DEFINE_GUID(IID_IDirectDrawSurface2, 0x57805885, 0x6EEC, 0x11CF, 0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27);
DEFINE_GUID(IID_IDirectDrawSurface3, 0xDA044E00, 0x69B2, 0x11D0, 0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB);
DEFINE_GUID(IID_IDirectDrawSurface4, 0x0B2B8630, 0xAD35, 0x11D0, 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B);
