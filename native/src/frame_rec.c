/* Frame recorder for interpolation: see frame_rec.h. */
#include <stdlib.h>
#include <string.h>

#include "frame_rec.h"
#include "log.h"

#ifdef _WIN32
#include <windows.h>
static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
void mml_sleep_ms(double ms);   /* mml.c: high-resolution timer sleep */

static void wait_until(double t)
{
    double left = t - now_ms();
    if (left > 0.6)
        mml_sleep_ms(left - 0.5);
    while (now_ms() < t)
        YieldProcessor();
}
#endif

enum { C_STATE, C_TEX, C_CLEAR, C_DRAW };

typedef struct {
    float x, y, z;
} interp_cam;

typedef struct {
    int kind;
    union {
        struct { int index; } state;              /* into g_states */
        gfx_tex *tex;
        struct { RECT rect; int has_rect, color_too, depth_too; uint32_t color; float depth; } clear;
        struct { int prim, first, count, moves; } draw;  /* into g_verts / g_prevs */
    } u;
} cmd;

#define MAX_CMDS   65536
#define MAX_STATES 32768
#define MAX_VERTS  (1 << 19)
#define MAX_DEFER  1024

static cmd g_cmds[MAX_CMDS];
static gfx_state g_states[MAX_STATES];
static gfx_vertex g_verts[MAX_VERTS];
static rec_prev g_prevs[MAX_VERTS];
static gfx_vertex g_out[65536];
static gfx_tex *g_defer[MAX_DEFER];
static int g_ncmds, g_nstates, g_nverts, g_ndefer, g_overflow;
static int g_on;
static int g_mid_only, g_timing;   /* debug */
static double g_last_present, g_gap_sum, g_gap_max;
static unsigned g_presents, g_late, g_cut_draws;

void rec_enable(int on) { g_on = on; }
void rec_debug(int mid_only, int timing) { g_mid_only = mid_only; g_timing = timing; }
int rec_enabled(void) { return g_on; }

static cmd *add(int kind)
{
    if (g_ncmds >= MAX_CMDS) {
        g_overflow = 1;
        return NULL;
    }
    g_cmds[g_ncmds].kind = kind;
    return &g_cmds[g_ncmds++];
}

void rec_state(const gfx_state *s)
{
    cmd *c;
    gfx_set_state(s);
    if (!g_on)
        return;
    if (g_nstates >= MAX_STATES || !(c = add(C_STATE))) {
        g_overflow = 1;
        return;
    }
    g_states[g_nstates] = *s;
    c->u.state.index = g_nstates++;
}

void rec_texture(gfx_tex *t)
{
    cmd *c;
    gfx_set_texture(t);
    if (g_on && (c = add(C_TEX)))
        c->u.tex = t;
}

void rec_clear(const RECT *rect, int color_too, uint32_t color, int depth_too, float depth)
{
    cmd *c;
    gfx_clear(rect, color_too, color, depth_too, depth);
    if (!g_on || !(c = add(C_CLEAR)))
        return;
    c->u.clear.has_rect = rect != NULL;
    if (rect)
        c->u.clear.rect = *rect;
    c->u.clear.color_too = color_too;
    c->u.clear.depth_too = depth_too;
    c->u.clear.color = color;
    c->u.clear.depth = depth;
}

void rec_draw(int prim, const gfx_vertex *v, const rec_prev *prev, int count)
{
    cmd *c;
    gfx_draw(prim, v, count);
    if (!g_on || count <= 0)
        return;
    if (g_nverts + count > MAX_VERTS || !(c = add(C_DRAW))) {
        g_overflow = 1;
        return;
    }
    memcpy(&g_verts[g_nverts], v, sizeof *v * (size_t)count);
    if (prev)
        memcpy(&g_prevs[g_nverts], prev, sizeof *prev * (size_t)count);
    c->u.draw.prim = prim;
    c->u.draw.first = g_nverts;
    c->u.draw.count = count;
    c->u.draw.moves = prev != NULL;
    g_nverts += count;
}

void rec_tex_destroy(gfx_tex *t)
{
    /* a texture the recording still points at has to live until the replay */
    if (g_on && g_ncmds && g_ndefer < MAX_DEFER) {
        g_defer[g_ndefer++] = t;
        return;
    }
    gfx_tex_destroy(t);
}

/* ---- near-plane cut for in-between frames ----
 * A 3D vertex blended part of the way back can land behind the near plane
 * (polygons very close to the camera, which the game itself cut there). Its
 * triangles are cut again at that depth, in camera space, the way the game
 * does: the visible part stays exact and shared edges stay shared. */
static interp_cam g_cam[65536];          /* blended camera-space position per vertex */
static gfx_vertex g_tris[3 * 256];
static int g_ntris;

static void tri_flush(void)
{
    if (g_ntris)
        gfx_draw(GFX_TRILIST, g_tris, g_ntris);
    g_ntris = 0;
}

static void tri_add(const gfx_vertex *a, const gfx_vertex *b, const gfx_vertex *c)
{
    if (g_ntris + 3 > (int)(sizeof g_tris / sizeof g_tris[0]))
        tri_flush();
    g_tris[g_ntris++] = *a;
    g_tris[g_ntris++] = *b;
    g_tris[g_ntris++] = *c;
}

static uint32_t lerp_color(uint32_t a, uint32_t b, float s)
{
    uint32_t out = 0;
    int sh;
    for (sh = 0; sh < 32; sh += 8) {
        float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * s + 0.5f) << sh;
    }
    return out;
}

/* the point at fraction s from vertex i to vertex j (camera space), projected */
static gfx_vertex cut_point(const gfx_vertex *v, const rec_prev *p, int i, int j, float s)
{
    gfx_vertex o;
    const interp_cam *a = &g_cam[i], *b = &g_cam[j];
    float x = a->x + (b->x - a->x) * s, y = a->y + (b->y - a->y) * s, z = a->z + (b->z - a->z) * s;
    /* per-vertex screen origin and depth / rhw scales (the same for a whole draw in practice) */
    float oxi = v[i].x - p[i].kx * p[i].cx / p[i].cz, oxj = v[j].x - p[j].kx * p[j].cx / p[j].cz;
    float oyi = v[i].y - p[i].ky * p[i].cy / p[i].cz, oyj = v[j].y - p[j].ky * p[j].cy / p[j].cz;
    float dzi = v[i].z / p[i].cz, dzj = v[j].z / p[j].cz;
    float rwi = v[i].rhw * p[i].cz, rwj = v[j].rhw * p[j].cz;
    o = v[i];
    o.x = oxi + (oxj - oxi) * s + p[i].kx * x / z;
    o.y = oyi + (oyj - oyi) * s + p[i].ky * y / z;
    o.z = (dzi + (dzj - dzi) * s) * z;
    o.rhw = (rwi + (rwj - rwi) * s) / z;
    o.u = v[i].u + (v[j].u - v[i].u) * s;
    o.v = v[i].v + (v[j].v - v[i].v) * s;
    o.diffuse = lerp_color(v[i].diffuse, v[j].diffuse, s);
    o.specular = lerp_color(v[i].specular, v[j].specular, s);
    return o;
}

static void clip_tri(const gfx_vertex *v, const rec_prev *p, const gfx_vertex *out, int i0, int i1, int i2)
{
    int idx[3] = { i0, i1, i2 }, k, n = 0;
    gfx_vertex poly[4];
    float near_z = p[i0].near_z;
    if (p[i0].valid != REC_PERSP || p[i1].valid != REC_PERSP || p[i2].valid != REC_PERSP) {
        /* can't cut what has no 3D position: draw this one as the game drew it */
        if ((p[i0].valid == REC_PERSP && g_cam[i0].z < near_z) || (p[i1].valid == REC_PERSP && g_cam[i1].z < near_z) ||
            (p[i2].valid == REC_PERSP && g_cam[i2].z < near_z))
            tri_add(&v[i0], &v[i1], &v[i2]);
        else
            tri_add(&out[i0], &out[i1], &out[i2]);
        return;
    }
    for (k = 0; k < 3; k++) {
        int a = idx[k], b = idx[(k + 1) % 3];
        int in_a = g_cam[a].z >= near_z, in_b = g_cam[b].z >= near_z;
        if (in_a)
            poly[n++] = out[a];
        if (in_a != in_b)
            poly[n++] = cut_point(v, p, a, b, (g_cam[a].z - near_z) / (g_cam[a].z - g_cam[b].z));
    }
    if (n >= 3)
        tri_add(&poly[0], &poly[1], &poly[2]);
    if (n == 4)
        tri_add(&poly[0], &poly[2], &poly[3]);
}

static void draw_cut(int prim, const gfx_vertex *v, const rec_prev *p, const gfx_vertex *out, int n)
{
    int i;
    g_ntris = 0;
    if (prim == GFX_TRILIST) {
        for (i = 0; i + 2 < n; i += 3)
            clip_tri(v, p, out, i, i + 1, i + 2);
    } else if (prim == GFX_TRISTRIP) {
        for (i = 0; i + 2 < n; i++)
            clip_tri(v, p, out, i, i + 1, i + 2);
    } else if (prim == GFX_TRIFAN) {
        for (i = 1; i + 1 < n; i++)
            clip_tri(v, p, out, 0, i, i + 1);
    }
    tri_flush();
}

/* replay the recording with vertices at fraction t of the way from where they
 * were one tick ago (t = 0) to where they are now (t = 1) */
static void replay(float t)
{
    int i, k;
    for (i = 0; i < g_ncmds; i++) {
        const cmd *c = &g_cmds[i];
        switch (c->kind) {
        case C_STATE:
            gfx_set_state(&g_states[c->u.state.index]);
            break;
        case C_TEX:
            gfx_set_texture(c->u.tex);
            break;
        case C_CLEAR:
            gfx_clear(c->u.clear.has_rect ? &c->u.clear.rect : NULL, c->u.clear.color_too, c->u.clear.color,
                      c->u.clear.depth_too, c->u.clear.depth);
            break;
        case C_DRAW: {
            const gfx_vertex *v = &g_verts[c->u.draw.first];
            const rec_prev *p = &g_prevs[c->u.draw.first];
            int n = c->u.draw.count, cut = 0;
            float u = 1.0f - t;
            if (!c->u.draw.moves || n > (int)(sizeof g_out / sizeof g_out[0])) {
                gfx_draw(c->u.draw.prim, v, n);
                break;
            }
            for (k = 0; k < n; k++) {
                g_out[k] = v[k];
                if (p[k].valid == REC_PERSP) {
                    float x = p[k].px * u + p[k].cx * t, y = p[k].py * u + p[k].cy * t;
                    float z = p[k].pz * u + p[k].cz * t;
                    g_cam[k].x = x;
                    g_cam[k].y = y;
                    g_cam[k].z = z;
                    if (z < p[k].near_z) {
                        cut = 1;
                        continue;
                    }
                    g_out[k].x = v[k].x + p[k].kx * (x / z - p[k].cx / p[k].cz);
                    g_out[k].y = v[k].y + p[k].ky * (y / z - p[k].cy / p[k].cz);
                    g_out[k].z = v[k].z * (z / p[k].cz);
                    g_out[k].rhw = v[k].rhw * p[k].cz / z;
                } else if (p[k].valid == REC_SCREEN) {
                    g_out[k].x = p[k].x + (v[k].x - p[k].x) * t;
                    g_out[k].y = p[k].y + (v[k].y - p[k].y) * t;
                    g_out[k].z = p[k].z + (v[k].z - p[k].z) * t;
                    if (g_out[k].z > 0.0f)
                        g_out[k].rhw = v[k].rhw * v[k].z / g_out[k].z;   /* rhw = 0.1 / z */
                }
            }
            if (cut) {
                draw_cut(c->u.draw.prim, v, p, g_out, n);
                g_cut_draws++;
            } else {
                gfx_draw(c->u.draw.prim, g_out, n);
            }
            break;
        }
        }
    }
}

/* debug: how evenly frames reach the screen */
static void note_present(void)
{
    double t;
    if (!g_timing)
        return;
    t = now_ms();
    if (g_last_present > 0.0) {
        double gap = t - g_last_present;
        g_gap_sum += gap;
        if (gap > g_gap_max)
            g_gap_max = gap;
        if (gap > 20.0)
            g_late++;
        if (++g_presents == 300) {
            log_printf("interp timing: %u presents, average gap %.2f ms, longest %.2f ms, %u over 20 ms, "
                       "%u draws cut at the near plane", g_presents, g_gap_sum / g_presents, g_gap_max, g_late, g_cut_draws);
            g_presents = g_late = g_cut_draws = 0;
            g_gap_sum = g_gap_max = 0.0;
        }
    }
    g_last_present = t;
}

static void reset(void)
{
    int i;
    g_ncmds = g_nstates = g_nverts = 0;
    for (i = 0; i < g_ndefer; i++)
        gfx_tex_destroy(g_defer[i]);
    g_ndefer = 0;
}

void rec_present(int steps)
{
    static int overflow_logged;
    double t0, tick;
    int s;

    if (!g_on || !g_ncmds || steps < 2) {
        gfx_present();
        reset();
        return;
    }
    if (g_overflow && !overflow_logged) {
        overflow_logged = 1;
        log_printf("interp: frame recording overflowed (%d commands, %d vertices), some draws not interpolated",
                   g_ncmds, g_nverts);
    }
    g_overflow = 0;
    t0 = now_ms();
    tick = 1000.0 / 60.0;
    for (s = 1; s < steps; s++) {
        gfx_use_target(1);
        replay((float)s / (float)steps);
        if (s > 1)
            wait_until(t0 + tick * (s - 1));
        gfx_present_target(1);
        note_present();
    }
    gfx_use_target(0);
    if (!g_mid_only) {
        /* the game's frame goes up half a tick later from the timer thread, so
         * the game thread can get on with the next frame meanwhile */
        gfx_compose_target(0);
        gfx_flip_at(t0 + tick * (steps - 1), note_present);
    }
    reset();
}
