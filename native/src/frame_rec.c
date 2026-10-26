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
static gfx_vertex g_out[MAX_VERTS > 65536 ? 65536 : MAX_VERTS];
static gfx_tex *g_defer[MAX_DEFER];
static int g_ncmds, g_nstates, g_nverts, g_ndefer, g_overflow;
static int g_on;
static int g_mid_only, g_timing;   /* debug */
static double g_last_present, g_gap_sum, g_gap_max;
static unsigned g_presents, g_late;

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
            int n = c->u.draw.count;
            if (!c->u.draw.moves || n > (int)(sizeof g_out / sizeof g_out[0])) {
                gfx_draw(c->u.draw.prim, v, n);
                break;
            }
            for (k = 0; k < n; k++) {
                g_out[k] = v[k];
                if (p[k].valid) {
                    g_out[k].x = p[k].x + (v[k].x - p[k].x) * t;
                    g_out[k].y = p[k].y + (v[k].y - p[k].y) * t;
                    g_out[k].z = p[k].z + (v[k].z - p[k].z) * t;
                    if (g_out[k].z > 0.0f)
                        g_out[k].rhw = v[k].rhw * v[k].z / g_out[k].z;   /* rhw = 0.1 / z */
                }
            }
            gfx_draw(c->u.draw.prim, g_out, n);
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
            log_printf("interp timing: %u presents, average gap %.2f ms, longest %.2f ms, %u over 20 ms",
                       g_presents, g_gap_sum / g_presents, g_gap_max, g_late);
            g_presents = g_late = 0;
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
        wait_until(t0 + tick * (steps - 1));
        gfx_present_target(0);
        note_present();
    }
    reset();
}
