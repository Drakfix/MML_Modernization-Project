/* Exact texture coordinates where the near plane cuts a model polygon.
 *
 * The object drawers send polygons that reach behind the camera through
 * gfx_near_clip_poly (0x4F04FB), a real Sutherland-Hodgman clipper in camera
 * space. Its output records are {x, y, z floats, u, v bytes}: the cut points'
 * positions are exact, but their texture coordinates are interpolated and then
 * ROUNDED to whole texels. Right next to the camera one texel covers dozens of
 * pixels, and the cut point slides along the edge as anything moves, so walls
 * and props close to the camera visibly swam.
 *
 * After the game's clipper returns we redo its walk with the same inputs, get
 * the unrounded coordinates of every cut point, and remember how far the
 * rounding moved them, keyed by the point's exact screen position. At draw
 * time those vertices get the difference back (the handlers turn a byte UV
 * into byte / 256). The game's own data is untouched.
 *
 * The clipper also projects every vertex it keeps (x * H / z), and the big map
 * models (buildings, cars, props) go through it whether they reach the camera
 * or not, so this is also where 60 fps interpolation learns about them: each
 * kept vertex is reported like the GTE's own projections (clipfix_report). */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "clipfix.h"
#include "hook.h"
#include "interp.h"
#include "log.h"

#define G_FN_NEAR_CLIP 0x4F04FBu   /* watcall: EAX SVECTOR* verts, EDX u8[2]* uvs, EBX out records,
                                    * ECX unused here, [esp+4] count (3 or 4); ret 4; returns count out */
#define G_GTE_ROT      0x77EAF8u   /* float[9] row-major */
#define G_GTE_TRANS    0x77EB1Cu   /* s32[3] */
#define G_GTE_NEAR     0x77EBC0u   /* float: the clipper's near distance */

typedef struct {
    float x, y, z;
    uint8_t u, v, pad[2];
} clip_rec;

/* screen position -> how far rounding moved the UV, per game frame */
#define TABLE_BITS 12
#define TABLE_SIZE (1u << TABLE_BITS)
typedef struct {
    uint64_t key;
    uint32_t gen;
    float du, dv;
} slot;
static slot g_table[TABLE_SIZE];
static uint32_t g_build_gen = 2, g_draw_gen = 1;
static unsigned g_fixed, g_applied;

static uintptr_t g_tramp;
static uint32_t g_a_verts, g_a_uvs, g_a_out, g_a_count, g_a_site;
static int g_report;

static uint64_t key_of(float x, float y)
{
    uint32_t a, b;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    return ((uint64_t)a << 32) | b;
}

static slot *probe(uint64_t key, uint32_t gen)
{
    uint32_t h = (uint32_t)key * 0x9E3779B1u ^ (uint32_t)(key >> 32) * 0x85EBCA77u, i, n;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    i = (h ^ (h >> 15)) & (TABLE_SIZE - 1);
    for (n = 0; n < TABLE_SIZE; n++) {
        slot *s = &g_table[i];
        if (s->gen != gen || s->key == key)
            return s;
        i = (i + 1) & (TABLE_SIZE - 1);
    }
    return NULL;
}

static void camera_space(const int16_t *sv, float out[3])
{
    const float *m = (const float *)G_GTE_ROT;
    const int32_t *t = (const int32_t *)G_GTE_TRANS;
    float x = sv[0], y = sv[1], z = sv[2];
    out[0] = x * m[0] + y * m[1] + z * m[2] + (float)t[0];
    out[1] = x * m[3] + y * m[4] + z * m[5] + (float)t[1];
    out[2] = x * m[6] + y * m[7] + z * m[8] + (float)t[2];
}

/* the clipper's walk again, without the rounding; called right after it */
static void __cdecl clip_post(int produced)
{
    static const int quad_order[5] = { 0, 1, 3, 2, 0 }, tri_order[4] = { 0, 1, 2, 0 };
    const int16_t *verts = (const int16_t *)(uintptr_t)g_a_verts;
    const uint8_t *uvs = (const uint8_t *)(uintptr_t)g_a_uvs;
    const clip_rec *out = (const clip_rec *)(uintptr_t)g_a_out;
    const int *order;
    float near_z = *(const float *)G_GTE_NEAR, c[5][3];
    int n = (int)g_a_count, i, k = 0;

    if ((n != 3 && n != 4) || produced <= 0 || produced > 8)
        return;
    order = n == 4 ? quad_order : tri_order;
    for (i = 0; i < n; i++)
        camera_space(verts + 4 * i, c[i]);
    for (i = 0; i < n; i++) {
        int a = order[i], b = order[i + 1];
        int out_a = c[a][2] < near_z, out_b = c[b][2] < near_z;
        if (!out_a) {
            if (g_report && k < produced)     /* vertex a itself: exact, and tracked for 60 fps */
                interp_vertex(g_a_site, verts + 4 * a, c[a], out[k].x, out[k].y);
            k++;
            if (!out_b)
                continue;
        } else if (out_b) {
            continue;
        }
        /* the cut point on edge a-b */
        if (k < produced) {
            float t = (c[a][2] - near_z) / (c[a][2] - c[b][2]);
            float u = uvs[2 * a] - ((float)uvs[2 * a] - (float)uvs[2 * b]) * t;
            float v = uvs[2 * a + 1] - ((float)uvs[2 * a + 1] - (float)uvs[2 * b + 1]) * t;
            float du = u - out[k].u, dv = v - out[k].v;
            if (du != 0.0f || dv != 0.0f) {
                slot *s = probe(key_of(out[k].x, out[k].y), g_build_gen);
                if (s) {
                    s->key = key_of(out[k].x, out[k].y);
                    s->gen = g_build_gen;
                    s->du = du;
                    s->dv = dv;
                    g_fixed++;
                }
            }
        }
        k++;
    }
}

static __declspec(naked) void clip_detour(void)
{
    __asm {
        mov g_a_verts, eax
        mov g_a_uvs, edx
        push eax
        mov eax, [esp + 4]                    ; our caller: which drawer this is
        mov g_a_site, eax
        pop eax
        mov g_a_out, ebx
        push eax
        mov eax, [esp + 8]                    ; the count, above our push and the return address
        mov g_a_count, eax
        pop eax
        push dword ptr [g_a_count]
        call dword ptr [g_tramp]              ; the game's clipper (pops its argument)
        pushad
        push eax
        call clip_post
        add esp, 4
        popad
        ret 4
    }
}

int clipfix_install(void)
{
    static const uint8_t prologue[8] = { 0x56, 0x57, 0x55, 0x89, 0xE5, 0x83, 0xEC, 0x74 };
    uint8_t jmp[5] = { 0xE9 }, *tr;
    int32_t rel;
    if (!mem_expect(G_FN_NEAR_CLIP, prologue, sizeof prologue)) {
        log_printf("clipfix: near clipper unexpected, left alone");
        return 0;
    }
    tr = (uint8_t *)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr)
        return 0;
    memcpy(tr, prologue, sizeof prologue);
    tr[8] = 0xE9;
    rel = (int32_t)((G_FN_NEAR_CLIP + sizeof prologue) - ((uintptr_t)tr + 13));
    memcpy(tr + 9, &rel, 4);
    g_tramp = (uintptr_t)tr;
    rel = (int32_t)((uintptr_t)clip_detour - (G_FN_NEAR_CLIP + 5));
    memcpy(jmp + 1, &rel, 4);
    mem_write(G_FN_NEAR_CLIP, jmp, sizeof jmp);
    log_printf("clipfix: exact texture coordinates where the near plane cuts models");
    return 1;
}

void clipfix_report(int on)
{
    g_report = on;
}

void clipfix_frame(void)
{
    static unsigned frames;
    g_draw_gen = g_build_gen;   /* what the game built this frame is about to be drawn */
    g_build_gen++;
    if (g_build_gen < 2)
        g_build_gen = 2;
    if (++frames % 300 == 0 && g_fixed) {
        log_printf("clipfix: %u cut points made exact, %u vertices corrected (300 frames)", g_fixed, g_applied);
        g_fixed = g_applied = 0;
    }
}

const gfx_vertex *clipfix_apply(const gfx_vertex *v, const gfx_vertex *raw, unsigned n)
{
    static gfx_vertex fixed[4096];
    unsigned k;
    int copied = 0;
    if (!g_tramp || n > 4096)
        return v;
    for (k = 0; k < n; k++) {
        slot *s = probe(key_of(raw[k].x * 0.5f, raw[k].y * 0.5f), g_draw_gen);
        if (!s || s->gen != g_draw_gen)
            continue;
        if (!copied) {
            memcpy(fixed, v, sizeof *v * n);
            copied = 1;
        }
        fixed[k].u += s->du * (1.0f / 256.0f);
        fixed[k].v += s->dv * (1.0f / 256.0f);
        g_applied++;
    }
    return copied ? fixed : v;
}
