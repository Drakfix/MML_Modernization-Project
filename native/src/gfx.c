/* gfx.h: the calls go to the backend [video] backend= names (gfx_options.backend), chosen when
 * the window is first prepared or the device made. Direct3D 11 until then and by default. */
#include <string.h>

#include "gfx_backend.h"
#include "log.h"

static const gfx_backend *const g_backends[] = { &gfx_backend_d3d11 };
static const gfx_backend *g_be = &gfx_backend_d3d11;
static int g_chosen;

static void choose(const gfx_options *opt)
{
    size_t k;
    if (g_chosen)
        return;
    g_chosen = 1;
    if (!opt || !opt->backend[0])
        return;
    for (k = 0; k < sizeof g_backends / sizeof g_backends[0]; k++)
        if (_stricmp(opt->backend, g_backends[k]->name) == 0) {
            g_be = g_backends[k];
            return;
        }
    log_printf("gfx: no backend \"%s\", using %s", opt->backend, g_be->name);
}

int gfx_init(void *game_window, int app_w, int app_h, const gfx_options *opt)
{
    choose(opt);
    return g_be->init(game_window, app_w, app_h, opt);
}

void gfx_prepare_window(void *game_window, int app_w, int app_h, const gfx_options *opt)
{
    choose(opt);
    g_be->prepare_window(game_window, app_w, app_h, opt);
}

int gfx_ready(void) { return g_be->ready(); }
void gfx_set_mode(int app_w, int app_h) { g_be->set_mode(app_w, app_h); }
gfx_tex *gfx_tex_create(int w, int h) { return g_be->tex_create(w, h); }
void gfx_tex_upload(gfx_tex *t, const uint32_t *rgba, int w, int h) { g_be->tex_upload(t, rgba, w, h); }
void gfx_tex_destroy(gfx_tex *t) { g_be->tex_destroy(t); }
void gfx_tex_size(const gfx_tex *t, int *w, int *h) { g_be->tex_size(t, w, h); }
void gfx_set_texture(gfx_tex *t) { g_be->set_texture(t); }
void gfx_set_state(const gfx_state *s) { g_be->set_state(s); }
void gfx_draw(int prim, const gfx_vertex *v, int count) { g_be->draw(prim, v, count); }
void gfx_clear(const gfx_rect *rect, int color_too, uint32_t color, int depth_too, float depth)
{
    g_be->clear(rect, color_too, color, depth_too, depth);
}
void gfx_present(void) { g_be->present(); }
void gfx_use_target(int t) { g_be->use_target(t); }
void gfx_present_target(int t) { g_be->present_target(t); }
void gfx_compose_target(int t) { g_be->compose_target(t); }
void gfx_flip_at(double due_ms, void (*on_flip)(void)) { g_be->flip_at(due_ms, on_flip); }
void gfx_wait_flip(void) { g_be->wait_flip(); }
const gfx_options *gfx_get_options(void) { return g_be->get_options(); }
void gfx_set_vsync(int on) { g_be->set_vsync(on); }
void gfx_set_nearest_upscale(int on) { g_be->set_nearest_upscale(on); }
void gfx_set_anisotropy(int n) { g_be->set_anisotropy(n); }
void gfx_set_scene(int render_scale, int msaa) { g_be->set_scene(render_scale, msaa); }
int gfx_set_shader(const char *path) { return g_be->set_shader(path); }
void gfx_finish_depth(int t) { g_be->finish_depth(t); }
