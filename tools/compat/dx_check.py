"""Checks native/compat's DirectX and PE definitions against the Windows SDK's headers.

    python tools/compat/dx_check.py

A program built with MSVC (x86) against the SDK prints, for every struct and member listed below,
_Static_assert lines with the SDK's sizes and offsets; they are then compiled against
native/compat with the Android NDK's clang for 32-bit x86, the layout the game itself uses.
Any difference fails that compile. Needs the Visual Studio Build Tools and the Android NDK
(ANDROID_SDK, default %LOCALAPPDATA%\\Android\\Sdk).
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
COMPAT = os.path.join(HERE, "..", "..", "native", "compat")

CHECKS = {
    "DDCOLORKEY": ["dwColorSpaceLowValue", "dwColorSpaceHighValue"],
    "DDSCAPS2": ["dwCaps", "dwCaps2", "dwCaps3", "dwCaps4"],
    "DDPIXELFORMAT": ["dwSize", "dwFlags", "dwFourCC", "dwRGBBitCount", "dwZBufferBitDepth", "dwRBitMask",
                      "dwGBitMask", "dwZBitMask", "dwBBitMask", "dwRGBAlphaBitMask"],
    "DDSURFACEDESC": ["dwSize", "dwFlags", "dwHeight", "dwWidth", "lPitch", "dwBackBufferCount", "dwMipMapCount",
                      "dwAlphaBitDepth", "lpSurface", "ddckCKDestOverlay", "ddckCKSrcBlt", "ddpfPixelFormat",
                      "ddsCaps"],
    "DDSURFACEDESC2": ["dwSize", "dwFlags", "dwHeight", "dwWidth", "lPitch", "dwBackBufferCount", "dwMipMapCount",
                       "dwAlphaBitDepth", "lpSurface", "ddckCKDestOverlay", "ddckCKSrcBlt", "ddpfPixelFormat",
                       "ddsCaps", "dwTextureStage"],
    "DDBLTFX": ["dwSize", "dwZDestConst", "dwZSrcConst", "dwAlphaDestConst", "dwAlphaSrcConst", "dwFillColor",
                "ddckDestColorkey", "ddckSrcColorkey"],
    "DDCAPS": ["dwSize", "dwCaps", "dwCaps2", "dwCKeyCaps", "dwZBufferBitDepths", "dwVidMemTotal", "dwVidMemFree",
               "dwRops", "ddsOldCaps", "dwSVBCaps", "dwNLVBRops", "ddsCaps"],
    "DDDEVICEIDENTIFIER": ["szDriver", "szDescription", "liDriverVersion", "dwVendorId", "dwRevision",
                           "guidDeviceIdentifier"],
    "D3DCOLORVALUE": ["r", "g", "b", "a"],
    "D3DMATERIAL": ["dwSize", "diffuse", "ambient", "specular", "emissive", "power", "hTexture", "dwRampSize"],
    "D3DVIEWPORT2": ["dwSize", "dwX", "dwWidth", "dvClipX", "dvClipHeight", "dvMinZ", "dvMaxZ"],
    "D3DRECT": ["x1", "y1", "x2", "y2"],
    "D3DPRIMCAPS": ["dwSize", "dwMiscCaps", "dwShadeCaps", "dwTextureAddressCaps", "dwStippleHeight"],
    "D3DDEVICEDESC": ["dwSize", "dwFlags", "dcmColorModel", "dwDevCaps", "dtcTransformCaps", "bClipping",
                      "dlcLightingCaps", "dpcLineCaps", "dpcTriCaps", "dwDeviceRenderBitDepth",
                      "dwDeviceZBufferBitDepth", "dwMaxBufferSize", "dwMaxVertexCount", "dwMinTextureWidth",
                      "dwMaxTextureHeight", "dwMaxStippleHeight", "dwMaxTextureRepeat", "dwMaxTextureAspectRatio",
                      "dwMaxAnisotropy", "dvGuardBandLeft", "dvExtentsAdjust", "dwStencilCaps", "dwTextureOpCaps",
                      "wMaxTextureBlendStages", "wMaxSimultaneousTextures"],
    "IMAGE_DOS_HEADER": ["e_magic", "e_lfanew"],
    "IMAGE_FILE_HEADER": ["Machine", "NumberOfSections", "SizeOfOptionalHeader", "Characteristics"],
    "IMAGE_OPTIONAL_HEADER32": ["Magic", "AddressOfEntryPoint", "ImageBase", "SizeOfImage", "SizeOfHeaders",
                                "NumberOfRvaAndSizes", "DataDirectory"],
    "IMAGE_NT_HEADERS32": ["Signature", "FileHeader", "OptionalHeader"],
    "IMAGE_SECTION_HEADER": ["Name", "Misc", "VirtualAddress", "SizeOfRawData", "PointerToRawData",
                             "Characteristics"],
    "IMAGE_IMPORT_DESCRIPTOR": ["OriginalFirstThunk", "TimeDateStamp", "Name", "FirstThunk"],
    "WIN32_FIND_DATAA": ["dwFileAttributes", "nFileSizeHigh", "nFileSizeLow", "cFileName", "cAlternateFileName"],
}


def vcvars():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    vs = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                        capture_output=True, text=True).stdout.strip()
    return os.path.join(vs, "VC", "Auxiliary", "Build", "vcvarsall.bat")


def main():
    work = tempfile.mkdtemp(prefix="dx_check_")
    nl = chr(92) + "n"
    src = ["#include <windows.h>", "#include <ddraw.h>", "#include <d3d.h>", "#include <stddef.h>",
           "#include <stdio.h>", "int main(void)", "{"]
    for t, members in CHECKS.items():
        src.append('    printf("_Static_assert(sizeof(%s) == %%u, \\"sizeof %s\\");%s", (unsigned)sizeof(%s));'
                   % (t, t, nl, t))
        for m in members:
            src.append('    printf("_Static_assert(offsetof(%s, %s) == %%u, \\"%s.%s\\");%s", (unsigned)offsetof(%s, %s));'
                       % (t, m, t, m, nl, t, m))
    src += ["    return 0;", "}"]
    with open(os.path.join(work, "sdk.c"), "w") as f:
        f.write("\n".join(src) + "\n")
    subprocess.run('call "%s" x86 >nul 2>&1 && cl /nologo /W1 sdk.c >nul && .\\sdk.exe > asserts.h' % vcvars(),
                   shell=True, cwd=work, check=True)
    with open(os.path.join(work, "ours.c"), "w") as f:
        f.write('#include <windows.h>\n#include <ddraw.h>\n#include <stddef.h>\n#include "asserts.h"\n')
    sdk = os.environ.get("ANDROID_SDK") or os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk")
    ndk = os.path.join(sdk, "ndk", sorted(os.listdir(os.path.join(sdk, "ndk")))[-1])
    clang = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin", "clang.exe")
    r = subprocess.run([clang, "--target=i686-linux-android28", "-fsyntax-only", "-I", COMPAT, "-I", work,
                        os.path.join(work, "ours.c")], capture_output=True, text=True)
    n = sum(1 for _ in open(os.path.join(work, "asserts.h")))
    if r.returncode:
        print(r.stderr)
        print("MISMATCH (of %d checks)" % n)
        sys.exit(1)
    print("%d sizes and offsets match the Windows SDK" % n)


if __name__ == "__main__":
    main()
