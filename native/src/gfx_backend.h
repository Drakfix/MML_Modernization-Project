#pragma once
/* A rendering backend: the functions of gfx.h, which gfx.c hands to the backend chosen at
 * gfx_init ([video] backend=). Each backend file defines one of these tables. */
#include "gfx.h"
#include "gfx_ui.h"

typedef struct {
    const char *name;
    int (*init)(void *game_window, int app_w, int app_h, const gfx_options *opt);
    void (*prepare_window)(void *game_window, int app_w, int app_h, const gfx_options *opt);
    int (*ready)(void);
    void (*set_mode)(int app_w, int app_h);
    gfx_tex *(*tex_create)(int w, int h);
    void (*tex_upload)(gfx_tex *t, const uint32_t *rgba, int w, int h);
    void (*tex_destroy)(gfx_tex *t);
    void (*tex_size)(const gfx_tex *t, int *w, int *h);
    void (*set_texture)(gfx_tex *t);
    void (*set_state)(const gfx_state *s);
    void (*draw)(int prim, const gfx_vertex *v, int count);
    void (*clear)(const gfx_rect *rect, int color_too, uint32_t color, int depth_too, float depth);
    void (*present)(void);
    void (*use_target)(int t);
    void (*present_target)(int t);
    void (*compose_target)(int t);
    void (*flip_at)(double due_ms, void (*on_flip)(void));
    void (*wait_flip)(void);
    const gfx_options *(*get_options)(void);
    void (*set_vsync)(int on);
    void (*set_nearest_upscale)(int on);
    void (*set_anisotropy)(int n);
    void (*set_scene)(int render_scale, int msaa);
    int (*set_shader)(const char *path);
    void (*finish_depth)(int t);
    /* mml.dll: called on a stack of gfx.c's own, not the game's (the game keeps its threads'
       16 KB stacks just below the main loop's frames; a deep driver call overwrote them) */
    int own_stack;
} gfx_backend;

extern const gfx_backend gfx_backend_d3d11;   /* gfx_d3d11.c */
#ifdef MML_VULKAN
extern const gfx_backend gfx_backend_vulkan;  /* gfx_vk.c */
#endif
