/* Always full-detail models ([video] lod=full, the default).
 *
 * Characters and props carry three versions of their model (pointers at
 * +0x70 full, +0x74 medium, +0x78 low in their model table). Six draw paths
 * pick one by the object's screen depth with the same two thresholds:
 *     cmp depth, 0x708 ; jle .near    (> 1800: low)
 *     ...
 *   .near:
 *     cmp depth, 0x2BC ; jle .full    (> 700: medium, <= 700: full)
 * Raising both thresholds out of reach makes every path take the full model,
 * so nothing swaps as it moves away. With the wide view and high resolutions
 * the swaps were very visible, and the extra polygons cost nothing today.
 *
 * Sites: 0x4EAF76 (actors, alternate path), 0x4EB5D4 (actors), 0x4EC000,
 * and three more model drawers at 0x4EC93E / 0x4ECC76 / 0x4ED26F.
 *
 * Full models far away bring a second problem: their small triangles (heads,
 * hands) cover less than one PS1 pixel. The GTE's NCLIP (0x4EFD2E) computes
 * the winding cross product in 320x240 space and truncates it to an integer,
 * so those triangles come out as 0 = "back-facing" and get dropped, and the
 * character loses parts. The low models had big enough triangles to survive.
 * keep_small_triangles() swaps the truncation for one that gives +-1 when a
 * nonzero cross product would truncate to 0. Every caller only tests the sign,
 * and exactly degenerate triangles still come out 0.
 *
 * The ground does the same per terrain chunk (FUN_004345B3, which feeds the
 * cell emitter 0x4346C6). By the chunk's depth:
 *     <= 0x400   near: the subdivided cell renderer (always full detail)
 *     <= 0xAF0   mesh +0x18 (full)
 *     <= 0x1C20  mesh +0x1C (reduced)
 *     beyond     mesh +0x20 (reduced further)
 * so the far ground was coarser and switched to the full mesh as you came
 * closer, which read as "low-res textures until close". Making both JLEs
 * unconditional sends every chunk past the near range down the +0x18 path,
 * the same one the closest chunks already take. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "hook.h"
#include "log.h"
#include "lod.h"

/* address of each CMP reg,imm32 instruction; the immediate sits at +2 */
static const struct { uintptr_t at; uint8_t modrm; uint32_t imm; } g_sites[] = {
    { 0x4EB048, 0xFF, 0x708 }, { 0x4EB05B, 0xFF, 0x2BC },   /* cmp edi */
    { 0x4EB6D6, 0xFF, 0x708 }, { 0x4EB6E9, 0xFF, 0x2BC },   /* cmp edi */
    { 0x4EC184, 0xFE, 0x708 }, { 0x4EC194, 0xFE, 0x2BC },   /* cmp esi */
    { 0x4EC93E, 0xFA, 0x708 }, { 0x4EC94D, 0xFA, 0x2BC },   /* cmp edx */
    { 0x4ECC76, 0xFA, 0x708 }, { 0x4ECC85, 0xFA, 0x2BC },
    { 0x4ED26F, 0xFA, 0x708 }, { 0x4ED27E, 0xFA, 0x2BC },
};

/* terrain chunk LOD: jle (7E rel8) -> jmp (EB rel8) */
static const struct { uintptr_t at; uint8_t rel; } g_terrain[] = {
    { 0x434644, 0x07 },   /* cmp edi,0x1C20 ; jle -> skip the +0x20 mesh */
    { 0x434653, 0x05 },   /* cmp edi,0xAF0  ; jle -> skip the +0x1C mesh */
};

#define G_NCLIP_TRUNC_CALL 0x4EFD6Bu   /* in gte_nclip: call 0x4F915C (round toward zero), then fistp */
#define G_FPU_TRUNC        0x4F915Cu

static const uintptr_t g_fpu_trunc = G_FPU_TRUNC;

/* st(0) = cross product in, rounded value out; preserves every register */
static __declspec(naked) void nclip_round(void)
{
    __asm {
        push eax
        fld st(0)                    ; x x
        call dword ptr [g_fpu_trunc] ; t x
        ftst
        fnstsw ax
        sahf
        jne keep_t                   ; truncated to something nonzero: as before
        fstp st(0)                   ; x
        ftst
        fnstsw ax
        sahf
        je done                      ; exactly 0 (or NaN): as before
        ja positive
        fstp st(0)
        fld1
        fchs                         ; -1
        jmp done
    positive:
        fstp st(0)
        fld1                         ; +1
        jmp done
    keep_t:
        fstp st(1)                   ; t
    done:
        pop eax
        ret
    }
}

static int full_terrain(void)
{
    size_t i, n = sizeof g_terrain / sizeof g_terrain[0];
    for (i = 0; i < n; i++) {
        uint8_t expect[2] = { 0x7E, g_terrain[i].rel };
        if (!mem_expect(g_terrain[i].at, expect, sizeof expect)) {
            log_printf("lod: terrain site %08X unexpected, ground LODs left alone", (unsigned)g_terrain[i].at);
            return 0;
        }
    }
    for (i = 0; i < n; i++) {
        static const uint8_t jmp = 0xEB;
        mem_write(g_terrain[i].at, &jmp, 1);
    }
    log_printf("lod: always full-detail ground");
    return 1;
}

/* The nearest chunks (depth <= 0x400) went to a renderer that splits each
 * cell into a 3x3 grid, a PS1 trick against affine texture warping. Our
 * renderer is perspective-correct, so it only added trouble: the midpoints'
 * texture coordinates are halved as whole texels (walls and floors swam
 * close to the camera) and the split cells take their colour from another
 * path (asphalt changed shade as you came near). They now go down the same
 * full-detail path as every other chunk (cmp edi,0x400 ; jg -> jmp); terrain.c
 * clips them at the near plane. [debug] near_subdiv=1 brings the split back. */
int lod_no_subdiv(void)
{
    static const uint8_t expect[2] = { 0x7F, 0x34 }, jmp = 0xEB;
    if (!mem_expect(0x434660, expect, sizeof expect)) {
        log_printf("lod: near branch unexpected");
        return 0;
    }
    mem_write(0x434660, &jmp, 1);
    log_printf("lod: near ground cells not subdivided");
    return 1;
}

static int keep_small_triangles(void)
{
    uint8_t expect[5] = { 0xE8 }, call[5] = { 0xE8 };
    int32_t rel = (int32_t)(G_FPU_TRUNC - (G_NCLIP_TRUNC_CALL + 5));
    memcpy(expect + 1, &rel, 4);
    if (!mem_expect(G_NCLIP_TRUNC_CALL, expect, sizeof expect)) {
        log_printf("lod: NCLIP code unexpected, small triangles left alone");
        return 0;
    }
    rel = (int32_t)((uintptr_t)nclip_round - (G_NCLIP_TRUNC_CALL + 5));
    memcpy(call + 1, &rel, 4);
    mem_write(G_NCLIP_TRUNC_CALL, call, sizeof call);
    log_printf("lod: sub-pixel triangles kept (NCLIP rounds away from 0)");
    return 1;
}

int lod_install(int small_tris)
{
    static const uint32_t never = 0x7FFFFFFF;
    size_t i, n = sizeof g_sites / sizeof g_sites[0];

    for (i = 0; i < n; i++) {
        uint8_t expect[6] = { 0x81, g_sites[i].modrm };
        memcpy(expect + 2, &g_sites[i].imm, 4);
        if (!mem_expect(g_sites[i].at, expect, sizeof expect)) {
            log_printf("lod: site %08X unexpected, model LODs left alone", (unsigned)g_sites[i].at);
            return 0;
        }
    }
    for (i = 0; i < n; i++)
        mem_write(g_sites[i].at + 2, &never, 4);
    log_printf("lod: always full-detail models (%u thresholds raised)", (unsigned)n);
    full_terrain();
    if (small_tris)
        keep_small_triangles();
    return 1;
}
