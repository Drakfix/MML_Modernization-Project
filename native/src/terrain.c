/* Terrain near-plane clipping (the "holes in the floor", RE-notes §7.7).
 *
 * The game's software GTE (perspective divide 0x4EEED0) doesn't clip: a
 * vertex closer than the near distance gets projected as if it sat ON the
 * near plane, and one behind the camera also gets depth SZ = 0, which makes
 * the packet depth writers (0x4F0242 quads, 0x4F0165 triangles) give the
 * whole polygon z = 1.0. Objects never reach that point because they go
 * through the real clipper (gfx_near_clip_poly 0x4F04FB) first; terrain never
 * does. The old driver threw those polygons away, which left the holes.
 *
 * Fix, in two halves:
 *  - Emit time: right after each terrain polygon is built, recompute its
 *    vertices in camera space from the cell's model data (the same SVECTORs
 *    and GTE matrix the game just used). If any vertex is nearer than the
 *    near distance, park the camera-space vertices in a ring and tag the
 *    packet: its z fields get a quiet NaN whose payload is the ring index.
 *  - Draw time: the packet's handler copies z into the staged vertices
 *    untouched, so a DrawPrimitive whose first z is a tag gets clipped
 *    against the near plane (Sutherland-Hodgman, float UVs from the staged
 *    vertices) and drawn as a triangle list. Untouched vertices keep the
 *    game's own screen position, so the result meets its neighbours exactly.
 *
 * The draw hook sits on the device vtable outside display.c's, so it sees
 * the game's 640-wide coordinates and works with either renderer. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "ddshim.h"
#include "game.h"
#include "guest.h"
#include "hook.h"
#include "log.h"
#include "mml.h"
#include "terrain.h"

/* software GTE state */
#define G_GTE_ROT     0x77EAF8u   /* float[9], row-major rotation */
#define G_GTE_TRANS   0x77EB1Cu   /* s32[3] translation */
#define G_GTE_SZ      0x77EBA0u   /* s32 depth fifo: [0] newest .. [3] oldest */
#define G_GTE_NEAR    0x77EBC0u   /* float: near distance */
#define G_GTE_OFX     0x77EBCCu   /* s32 screen offset (320-wide space) */
#define G_GTE_OFY     0x77EBD0u

/* terrain emitters */
#define G_CELL_SVEC   0x5598F4u   /* 4 SVECTORs {x, y, z, pad} of the cell being emitted */
#define G_CELL_W      0x559920u   /* s32[9]: SZ per grid slot of a subdivided cell */
#define G_EMIT_HEADER 0x559944u   /* u32: packet header being emitted, top byte = opcode */
#define G_CELL_TOPO   GA(0x511744u)   /* terrain_cell_topology: 4 quads then 4 triangles */
#define VT_DRAW_PRIMITIVE 28

/* hook sites: right after the depth writer returns */
#define SITE_CELL_A   GA(0x4349D5u)   /* emitter 0x4346C6, packet in EDI */
#define SITE_CELL_B   GA(0x43506Fu)   /* its twin 0x434D25, packet in EDI */
#define SITE_SUB_QUAD GA(0x4356EEu)   /* subdivided cell 0x43530E, quads, packet in EBP */
#define SITE_SUB_TRI  GA(0x43583Au)   /* same, triangles */

#define SITE_QUAD_FALLBACK GA(0x4F0266u)   /* mov [eax+8],1.0f in the quad depth writer */
#define SITE_TRI_FALLBACK  GA(0x4F0180u)   /* same in the triangle writer */

#define OP_TRI  0xA4
#define OP_QUAD 0xAC
#define TAG_BASE 0x7FC00000u          /* quiet NaN; payload 0 = drop the polygon */
#define TAG_MASK 0xFFC00000u
#define TAG_SITE 0x200000u              /* census: drop tags carry the emitter id */
#define RING 16384u
#define SZ_TO_Z (1.0f / 16384.0f)     /* packet z = SZ * 6.1035e-05 */
#define RHW_K 0.1f                    /* the handlers' rhw = 0.1 / z */
#define FAR_SZ_MAX 0xFFFF             /* bucket SZ >> 3 stays inside the 8192-entry ordering tables */

typedef struct {
    int n;                            /* 3 or 4, in packet order */
    float c[4][3];                    /* camera space */
    int sz[4];                        /* the game's own depth for each vertex */
    float near_z, hx, hy;
    float ofx, ofy;
    float x0;                         /* packet X of vertex 0, checks the match at draw time */
    int site;                         /* census: which emitter */
} clip_entry;

typedef struct {                      /* the game's FVF 0x1C4 */
    float x, y, z, rhw;
    uint32_t diffuse, specular;
    float u, v;
} tlvert;

typedef HRESULT(WINAPI *draw_prim_t)(void *dev, DWORD type, DWORD fvf, void *verts, DWORD count, DWORD flags);

static clip_entry g_ring[RING];
static unsigned g_next;
static int g_clip_on;
static float g_near_scale = 1.0f;   /* [debug] clip_near_scale: test the clipper visibly */
static int g_far_on;                  /* terrain_far_install: longer terrain range, buckets clamped */
static unsigned g_far_clamped, g_far_frames;
static int g_far_check;               /* dev (environment MML_FAR_CHECK=1): the clamp reads the right slots */
static unsigned g_far_match, g_far_mismatch;
static draw_prim_t g_next_draw;
static unsigned g_census_frames;
static unsigned g_tagged, g_dropped, g_clipped, g_stale, g_onscreen;
static float g_max_err;        /* census: reprojection error of kept vertices, device px */
static unsigned g_odd_shape;   /* census: a tag seen in a draw we don't clip */
/* census per emitter: 0/1 = cell emitters A/B, 2 = subdivided quads, 3 = subdivided tris */
static unsigned g_site_tagged[4], g_site_drawn[4], g_site_whole[4], g_site_whole_drawn[4];
static int g_cur_site;

/* ---- emit time ---- */

static void tag_packet(uint8_t *pkt, int n, const short (*v)[3], const int *sz)
{
    const float *m = (const float *)GUEST(G_GTE_ROT);
    const int32_t *t = (const int32_t *)GUEST(G_GTE_TRANS);
    float near_z = GVAR(float, G_GTE_NEAR) * g_near_scale;
    float c[4][3];
    uint32_t tag;
    int i, near_count = 0;

    /* SZ is the truncated camera depth, so SZ >= near means the vertex is fine */
    for (i = 0; i < n && (float)sz[i] >= near_z; i++)
        ;
    if (i == n)
        return;
    for (i = 0; i < n; i++) {
        float x = v[i][0], y = v[i][1], z = v[i][2];
        c[i][0] = m[0] * x + m[1] * y + m[2] * z + (float)t[0];
        c[i][1] = m[3] * x + m[4] * y + m[5] * z + (float)t[1];
        c[i][2] = m[6] * x + m[7] * y + m[8] * z + (float)t[2];
        if (c[i][2] < near_z)
            near_count++;
    }
    if (!near_count)
        return;
    if (near_count == n) {
        /* all of it nearer than the near plane: nothing to draw */
        tag = g_census_frames ? TAG_BASE | TAG_SITE | (uint32_t)g_cur_site : TAG_BASE;
        g_site_whole[g_cur_site]++;
    } else {
        unsigned idx = g_next++ % RING;
        clip_entry *e = &g_ring[idx];
        e->n = n;
        memcpy(e->c, c, sizeof c);
        for (i = 0; i < n; i++)
            e->sz[i] = sz[i];
        e->near_z = near_z;
        e->hx = mml_h_x();
        e->hy = GVAR(float, G_GTE_H_FLOAT);
        e->ofx = (float)GVAR(int32_t, G_GTE_OFX);
        e->ofy = (float)GVAR(int32_t, G_GTE_OFY);
        e->x0 = *(const float *)(pkt + (n == 4 ? 0x28 : 0x20));
        e->site = g_cur_site;
        g_site_tagged[g_cur_site]++;
        tag = TAG_BASE | (idx + 1);
    }
    for (i = 0; i < n; i++)
        *(uint32_t *)(pkt + 8 + 8 * i) = tag;
    g_tagged++;
}

/* emitters 0x4346C6 / 0x434D25: one quad or triangle from the cell's corners */
static void on_emit_cell(regs_t *r)
{
    const short *sv = (const short *)GUEST(G_CELL_SVEC);
    const int32_t *fifo = (const int32_t *)GUEST(G_GTE_SZ);
    int op = (int)((GVAR(uint32_t, G_EMIT_HEADER) >> 24) & 0xFC);
    short v[4][3];
    int sz[4], n, i;

    if (g_far_on) {
        /* the polygon's ordering-table bucket comes next: the largest of these four depths >> 3 */
        int32_t *depth = (int32_t *)GUEST(r->esp + 4 + 8);   /* the game's [esp+8] .. [esp+0x14] */
        if (g_far_check && op == OP_QUAD) {   /* dev: the slots must be this quad's four depths */
            int same = 1;
            for (i = 0; i < 4; i++)
                same &= depth[i] == fifo[3 - i] || depth[i] == fifo[i];
            if (same)
                g_far_match++;
            else
                g_far_mismatch++;
        }
        for (i = 0; i < 4; i++)
            if (depth[i] > FAR_SZ_MAX) {
                depth[i] = FAR_SZ_MAX;
                g_far_clamped++;
            }
    }
    if (op == OP_QUAD) {
        n = 4;
        sz[0] = fifo[3]; sz[1] = fifo[2]; sz[2] = fifo[1]; sz[3] = fifo[0];
    } else if (op == OP_TRI) {
        n = 3;
        sz[0] = fifo[2]; sz[1] = fifo[1]; sz[2] = fifo[0];
    } else {
        return;
    }
    for (i = 0; i < n; i++) {
        v[i][0] = sv[4 * i];
        v[i][1] = sv[4 * i + 1];
        v[i][2] = sv[4 * i + 2];
    }
    tag_packet((uint8_t *)GUEST(r->edi), n, (const short (*)[3])v, sz);
}

static void on_emit_cell_a(regs_t *r) { g_cur_site = 0; on_emit_cell(r); }
static void on_emit_cell_b(regs_t *r) { g_cur_site = 1; on_emit_cell(r); }

/* The subdivided cell is a 3x3 grid: slots 0-3 corners, 4 = mid(0,1),
 * 5 = mid(0,2), 6 = centre, 7 = mid(1,3), 8 = mid(2,3), built with the same
 * integer halving the game uses (0x43530E). */
static void grid_slot(int slot, short out[3])
{
    const short *sv = (const short *)GUEST(G_CELL_SVEC);
    const short *v0 = sv, *v1 = sv + 4, *v2 = sv + 8, *v3 = sv + 12;
    short b[3], c[3];
    int k;
#define MID(p, q, k) ((short)(((int)(p)[k] + (int)(q)[k]) >> 1))
    for (k = 0; k < 3; k++) {
        switch (slot) {
        case 0: out[k] = v0[k]; break;
        case 1: out[k] = v1[k]; break;
        case 2: out[k] = v2[k]; break;
        case 3: out[k] = v3[k]; break;
        case 4: out[k] = MID(v0, v1, k); break;
        case 5: out[k] = MID(v0, v2, k); break;
        case 7: out[k] = MID(v1, v3, k); break;
        case 8: out[k] = MID(v2, v3, k); break;
        default:
            b[k] = MID(v0, v2, k);
            c[k] = MID(v1, v3, k);
            out[k] = MID(b, c, k);
            break;
        }
    }
#undef MID
}

static void tag_grid_poly(uint8_t *pkt, int n, const int *slots)
{
    const int32_t *w = (const int32_t *)GUEST(G_CELL_W);
    short v[4][3];
    int sz[4], i;
    for (i = 0; i < n; i++) {
        if (slots[i] < 0 || slots[i] > 8)
            return;
        grid_slot(slots[i], v[i]);
        sz[i] = w[slots[i]];
    }
    tag_packet(pkt, n, (const short (*)[3])v, sz);
}

static void on_emit_sub_quad(regs_t *r)
{
    const uint8_t *esp = (const uint8_t *)GUEST(r->esp + 4);
    int next = *(const int *)(esp + 0x48);   /* topology index, already advanced by 4 */
    const uint8_t *topo = (const uint8_t *)GUEST(G_CELL_TOPO) + next - 4;
    int slots[4], i;
    if (next < 4 || next > 0x10)
        return;
    for (i = 0; i < 4; i++)
        slots[i] = topo[i];
    g_cur_site = 2;
    tag_grid_poly((uint8_t *)GUEST(r->ebp), 4, slots);
}

static void on_emit_sub_tri(regs_t *r)
{
    const uint8_t *esp = (const uint8_t *)GUEST(r->esp + 4);
    int slots[3];
    slots[0] = *(const int *)(esp + 0x40);
    slots[1] = *(const int *)(esp + 0x44);
    slots[2] = *(const int *)(esp + 0x38) >> 3;
    g_cur_site = 3;
    tag_grid_poly((uint8_t *)GUEST(r->ebp), 3, slots);
}

/* ---- draw time ---- */

static uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    int s;
    for (s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        out |= (uint32_t)(int)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

static HRESULT WINAPI clip_draw_prim(void *dev, DWORD type, DWORD fvf, void *verts, DWORD count, DWORD flags)
{
    static const int strip_order[4] = { 0, 1, 3, 2 }, fan_order[4] = { 0, 1, 2, 3 };
    const tlvert *in = (const tlvert *)verts;
    const clip_entry *e;
    const int *ord;
    tlvert poly[8], list[18];
    uint32_t tag;
    int k, m = 0, n = (int)count;

    if (fvf != 0x1C4 || !in || n < 3 || n > 4) {
        if (g_census_frames && fvf == 0x1C4 && in) {
            for (k = 0; k < n; k++)
                if ((*(const uint32_t *)&in[k].z & TAG_MASK) == TAG_BASE) {
                    g_odd_shape++;
                    break;
                }
        }
        return g_next_draw(dev, type, fvf, verts, count, flags);
    }
    tag = *(const uint32_t *)&in[0].z;
    if ((tag & TAG_MASK) != TAG_BASE)
        return g_next_draw(dev, type, fvf, verts, count, flags);
    if (tag == TAG_BASE || (tag & TAG_SITE)) {
        if (tag & TAG_SITE)
            g_site_whole_drawn[(tag & 3)]++;
        g_dropped++;
        return S_OK;
    }
    e = &g_ring[((tag & ~TAG_MASK) - 1) % RING];
    if (e->n != n || in[0].x != e->x0 * 2.0f || (n == 4 && type != 5 && type != 6)) {
        g_stale++;
        return S_OK;
    }
    ord = (n == 4 && type == 5) ? strip_order : fan_order;

    for (k = 0; k < n; k++) {
        int a = ord[k], b = ord[(k + 1) % n];
        int a_in = e->c[a][2] >= e->near_z, b_in = e->c[b][2] >= e->near_z;
        if (a_in) {
            tlvert *o = &poly[m++];
            if (g_census_frames) {
                float px = ((e->c[a][0] * e->hx) / e->c[a][2] + e->ofx) * 2.0f;
                float py = ((e->c[a][1] * e->hy) / e->c[a][2] + e->ofy) * 2.0f;
                float err = (float)fabs(px - in[a].x) + (float)fabs(py - in[a].y);
                if (err > g_max_err)
                    g_max_err = err;
            }
            *o = in[a];
            o->z = e->sz[a] > 0 ? (float)e->sz[a] * SZ_TO_Z : e->c[a][2] * SZ_TO_Z;
            o->rhw = RHW_K / o->z;
        }
        if (a_in != b_in) {
            tlvert *o = &poly[m++];
            float t = (e->near_z - e->c[a][2]) / (e->c[b][2] - e->c[a][2]);
            float cx = e->c[a][0] + (e->c[b][0] - e->c[a][0]) * t;
            float cy = e->c[a][1] + (e->c[b][1] - e->c[a][1]) * t;
            o->x = ((cx * e->hx) / e->near_z + e->ofx) * 2.0f;
            o->y = ((cy * e->hy) / e->near_z + e->ofy) * 2.0f;
            o->z = e->near_z * SZ_TO_Z;
            o->rhw = RHW_K / o->z;
            o->diffuse = lerp_color(in[a].diffuse, in[b].diffuse, t);
            o->specular = lerp_color(in[a].specular, in[b].specular, t);
            o->u = in[a].u + (in[b].u - in[a].u) * t;
            o->v = in[a].v + (in[b].v - in[a].v) * t;
        }
    }
    if (m < 3)
        return S_OK;
    if (g_census_frames) {
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        for (k = 0; k < m; k++) {
            if (poly[k].x < x0) x0 = poly[k].x;
            if (poly[k].x > x1) x1 = poly[k].x;
            if (poly[k].y < y0) y0 = poly[k].y;
            if (poly[k].y > y1) y1 = poly[k].y;
        }
        if (x1 > 0.0f && x0 < 640.0f && y1 > 0.0f && y0 < 480.0f)
            g_onscreen++;
    }
    for (k = 1; k + 1 < m; k++) {
        list[3 * (k - 1)] = poly[0];
        list[3 * (k - 1) + 1] = poly[k];
        list[3 * (k - 1) + 2] = poly[k + 1];
    }
    g_clipped++;
    g_site_drawn[e->site & 3]++;
    return g_next_draw(dev, 4 /* D3DPT_TRIANGLELIST */, fvf, list, (DWORD)(3 * (m - 2)), flags);
}

int terrain_clip_install(float near_scale)
{
    static const uint8_t cell_bytes[] = { 0xB8, 0x44, 0x99, 0x55, 0x00 };          /* mov eax,0x559944 */
    static const uint8_t sub_bytes[] = { 0x8B, 0x44, 0x24, 0x20, 0x89, 0x45, 0x00 }; /* mov eax,[esp+20]; mov [ebp],eax */

    if (!mem_expect(SITE_CELL_A, cell_bytes, sizeof cell_bytes) ||
        !mem_expect(SITE_CELL_B, cell_bytes, sizeof cell_bytes) ||
        !mem_expect(SITE_SUB_QUAD, sub_bytes, sizeof sub_bytes) ||
        !mem_expect(SITE_SUB_TRI, sub_bytes, sizeof sub_bytes)) {
        log_printf("terrain: emitter code unexpected, near clipping off");
        return 0;
    }
    if (!hook_mid(SITE_CELL_A, sizeof cell_bytes, cell_bytes, on_emit_cell_a) ||
        !hook_mid(SITE_CELL_B, sizeof cell_bytes, cell_bytes, on_emit_cell_b) ||
        !hook_mid(SITE_SUB_QUAD, sizeof sub_bytes, sub_bytes, on_emit_sub_quad) ||
        !hook_mid(SITE_SUB_TRI, sizeof sub_bytes, sub_bytes, on_emit_sub_tri)) {
        log_printf("terrain: hooks failed, near clipping off");
        return 0;
    }
    g_clip_on = 1;
    if (near_scale > 0.0f)
        g_near_scale = near_scale;
    log_printf("terrain: near-plane clipping ON (near x%.2f)", (double)g_near_scale);
    return 1;
}

/* ---- terrain range (draw distance) ----
 * Every frame FUN_00433C30 loads the terrain's depth cap from a table by the room's scale
 * (u8 0x77EABF: 0, 1, 2; the second table when 0x77EAC0 & 4) into 0x77EAA8, and the chunk test
 * (0x434537) drops every chunk whose centre has (z >> 2) >= the cap. Vanilla 0xE00 (14336 units);
 * the exe patch doubled the table (RE-notes 3.33), which the Lake's gate channel and Clozer Woods'
 * far side still pass. Doubled again here (57344 units, whole rooms).
 * What kept the cap down was the ordering table: a terrain polygon goes into bucket
 * (largest vertex depth >> 3) of the 8192-entry tables (.mmlo, 3.39) with no bounds check, and the
 * second table is the exe's last section, so a polygon at 65536 or more would write past it. The
 * emitter hooks above clamp those depths to 65535 first (bucket 8191, the farthest: drawn first,
 * the depth buffer still sorts it), so no cap can overflow it.
 *
 * [video] draw_distance=fog (the menu's Draw distance: Up to the fog; Full is the default): with
 * distance fog on, the range follows the fog (terrain_fog_end): it ends a little past where the fog
 * is full, never nearer than the 2001 release's own cap (which already drew a little past its fog:
 * Downtown fog full at 11469, cap 14336), so slower devices draw less. Past the fog's end the level
 * is drawn in the plain fog colour, which is invisible only against a fog-coloured sky: at Lake
 * Jyun the fogged far wall stood white against the blue sky, and there the sky shows instead. */
#define G_CAP_TABLE   GA(0x511760u)   /* u16 per room scale, stride 4 */
#define G_CAP_TABLE_B GA(0x51176Cu)   /* the same when 0x77EAC0 & 4 */
#define FOG_RANGE_MUL  1.15f          /* range past the fog's end: 15% ... */
#define FOG_RANGE_ADD  1024           /* ... plus two cells (a chunk's centre stands in for all of it) */

static const uint16_t k_cap_vanilla[2][3] = { { 0xE00, 0x680, 0x300 }, { 0xD80, 0x620, 0x2C0 } };
static const uint16_t k_cap_patched[2][3] = { { 0x1C00, 0xD00, 0x600 }, { 0x1B00, 0xC40, 0x580 } };
static uint16_t g_cap_cur[2][3];
static int g_cap_ok, g_fog_range, g_fog_units = -1;

static uintptr_t cap_addr(int t, int s)
{
    return (t ? G_CAP_TABLE_B : G_CAP_TABLE) + 4u * (uintptr_t)s;
}

/* [video] draw_distance (start-up and the menus): full, or fog = up to the fog */
void terrain_draw_distance_load(const char *ini)
{
    char v[16];
    GetPrivateProfileStringA("video", "draw_distance", "full", v, sizeof v, ini);
    g_fog_range = _stricmp(v, "fog") == 0;
    g_fog_units = -1;   /* the next frame sets the range again */
}

int terrain_range_install(int doubled)
{
    int t, s;
    for (t = 0; t < 2; t++)
        for (s = 0; s < 3; s++)
            if (!mem_expect(cap_addr(t, s), &k_cap_patched[t][s], 2)) {
                log_printf("terrain: depth cap table unexpected, terrain range left as patched");
                return 0;
            }
    memcpy(g_cap_cur, k_cap_patched, sizeof g_cap_cur);
    g_cap_ok = 1;
    if (!doubled)
        return 1;
    if (!g_clip_on) {
        log_printf("terrain: no emitter hooks, terrain range not doubled");
        return 1;
    }
    {
        char v[4] = "";
        GetEnvironmentVariableA("MML_FAR_CHECK", v, sizeof v);
        g_far_check = v[0] == '1';
    }
    g_far_on = 1;   /* the clamp first: the emitters may run on another thread */
    for (t = 0; t < 2; t++)
        for (s = 0; s < 3; s++) {
            g_cap_cur[t][s] = (uint16_t)(k_cap_patched[t][s] * 2);
            mem_write(cap_addr(t, s), &g_cap_cur[t][s], 2);
        }
    log_printf("terrain: range doubled (depth cap 0x%X = %d units), far buckets clamped", g_cap_cur[0][0],
               g_cap_cur[0][0] * 4);
    return 1;
}

/* fog.c, every frame: where the fog is full, in packet depth (camera z / 16384; 0 = no fog) */
void terrain_fog_end(float end_z)
{
    int units = 0, t, s;
    if (!g_cap_ok)
        return;
    if (g_fog_range && end_z > 0.0f)
        units = end_z >= 8.0f ? 0 : (int)(end_z * 16384.0f * FOG_RANGE_MUL) + FOG_RANGE_ADD;
    if (units == g_fog_units)
        return;
    g_fog_units = units;
    for (t = 0; t < 2; t++)
        for (s = 0; s < 3; s++) {
            int top = g_far_on ? k_cap_patched[t][s] * 2 : k_cap_patched[t][s], want = top;
            if (units) {
                want = units >> 2;
                if (want < k_cap_vanilla[t][s])
                    want = k_cap_vanilla[t][s];
                if (want > top)
                    want = top;
            }
            if (want != g_cap_cur[t][s]) {
                g_cap_cur[t][s] = (uint16_t)want;
                host_write(GUEST(cap_addr(t, s)), &g_cap_cur[t][s], 2);
            }
        }
    if (units)
        log_printf("terrain: fog full at %d units, terrain drawn to %d", (int)(end_z * 16384.0f), g_cap_cur[0][0] * 4);
    else
        log_printf("terrain: %s, terrain drawn to %d", g_fog_range ? "no fog" : "draw distance full",
                   g_cap_cur[0][0] * 4);
}

/* ---- census (dev) ---- */

#define MAX_SITES 32
static struct { uint32_t ret; unsigned quads, tris; } g_sites[MAX_SITES];

static void count(regs_t *r, int quad)
{
    /* the writers have no prologue: [esp] is the return address
     * (r->esp is taken after the stub's pushfd) */
    uint32_t ret = GVAR(uint32_t, r->esp + 4);
    int i;
    for (i = 0; i < MAX_SITES && g_sites[i].ret && g_sites[i].ret != ret; i++)
        ;
    if (i == MAX_SITES)
        return;
    g_sites[i].ret = ret;
    if (quad)
        g_sites[i].quads++;
    else
        g_sites[i].tris++;
}

static void on_quad_fallback(regs_t *r) { count(r, 1); }
static void on_tri_fallback(regs_t *r) { count(r, 0); }

void terrain_census_enable(void)
{
    static const uint8_t fallback_bytes[] = { 0xC7, 0x40, 0x08, 0x00, 0x00, 0x80, 0x3F };
    if (hook_mid(SITE_QUAD_FALLBACK, sizeof fallback_bytes, fallback_bytes, on_quad_fallback) &&
        hook_mid(SITE_TRI_FALLBACK, sizeof fallback_bytes, fallback_bytes, on_tri_fallback)) {
        g_census_frames = 1;
        log_printf("terrain: clip census ON");
    }
}

void terrain_frame(void)
{
    int i;
    if (g_clip_on && !g_next_draw) {
        void **vt = ddshim_device_vtable();
        if (vt) {
            /* display.c hooks the same slot earlier in the frame, so ours runs
             * first and hands it the game's 640-wide coordinates */
            void *mine = (void *)clip_draw_prim;
            g_next_draw = (draw_prim_t)vt[VT_DRAW_PRIMITIVE];
            host_write(&vt[VT_DRAW_PRIMITIVE], &mine, sizeof mine);
            log_printf("terrain: DrawPrimitive hooked (vtable %p)", (void *)vt);
        }
    }
    if (g_far_on && ++g_far_frames >= (g_far_check ? 300u : 1800u)) {
        if (g_far_clamped)
            log_printf("terrain: %u far polygon depths clamped to the last bucket (%u frames)", g_far_clamped,
                       g_far_frames);
        if (g_far_check)
            log_printf("terrain: far check: %u quads' depth slots match the GTE, %u don't", g_far_match,
                       g_far_mismatch);
        g_far_frames = g_far_clamped = g_far_match = g_far_mismatch = 0;
    }
    if (!g_census_frames || ++g_census_frames < 60)
        return;
    g_census_frames = 1;
    for (i = 0; i < MAX_SITES && g_sites[i].ret; i++) {
        if (g_sites[i].quads || g_sites[i].tris)
            log_printf("clip census: caller %08X  quads %u  tris %u (60 frames)",
                       g_sites[i].ret, g_sites[i].quads, g_sites[i].tris);
        g_sites[i].quads = g_sites[i].tris = 0;
    }
    log_printf("clip census: tagged %u, drawn clipped %u (on screen %u), dropped whole %u, stale %u, "
               "max reprojection error %.4f px, tags in other draws %u", g_tagged, g_clipped, g_onscreen,
               g_dropped, g_stale, (double)g_max_err, g_odd_shape);
    g_odd_shape = 0;
    for (i = 0; i < 4; i++) {
        if (g_site_tagged[i] || g_site_whole[i])
            log_printf("clip census: emitter %d: partial %u drawn %u, whole %u reached draw %u", i,
                       g_site_tagged[i], g_site_drawn[i], g_site_whole[i], g_site_whole_drawn[i]);
        g_site_tagged[i] = g_site_drawn[i] = g_site_whole[i] = g_site_whole_drawn[i] = 0;
    }
    g_tagged = g_clipped = g_dropped = g_stale = g_onscreen = 0;
    g_max_err = 0.0f;
}
