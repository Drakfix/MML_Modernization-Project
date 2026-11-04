#pragma once
/* Rendering backend interface.
 *
 * The game draws only screen-space ("transformed and lit") triangles with one
 * vertex layout, 16-bit textures, a handful of blend modes, alpha test, depth
 * test and table fog (see the API census notes in native/README.md). That's
 * the whole contract a backend has to meet; ddshim.c turns the game's
 * DirectDraw/Direct3D calls into these. gfx.c hands them to the backend
 * chosen with [video] backend= (gfx_backend.h): Direct3D 11 (gfx_d3d11.c),
 * the first; Vulkan next. Nothing here depends on the platform: the window is
 * the platform's own handle (an HWND on Windows). */
#include <stdint.h>

/* matches the game's FVF 0x1C4: XYZRHW | DIFFUSE | SPECULAR | TEX1 (32 bytes) */
typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse, specular;   /* D3DCOLOR, 0xAARRGGBB */
    float u, v;
} gfx_vertex;

typedef struct gfx_tex gfx_tex;

typedef struct {
    int32_t left, top, right, bottom;   /* the game's coordinates, right / bottom exclusive */
} gfx_rect;

enum { GFX_TRILIST = 4, GFX_TRISTRIP = 5, GFX_TRIFAN = 6 };   /* D3DPRIMITIVETYPE values */

typedef struct {
    uint8_t blend;                /* alpha blending on */
    uint8_t src_blend, dst_blend; /* D3DBLEND values */
    uint8_t z_enable, z_write;
    uint8_t z_func;               /* D3DCMPFUNC */
    uint8_t alpha_test, alpha_func, alpha_ref;
    uint8_t flat;                 /* flat shading: colour of the first vertex */
    uint8_t linear;               /* bilinear texture filtering (else point) */
    uint8_t fog;                  /* table fog on */
    uint8_t scene_depth;          /* 3D world geometry: its depth counts for effects (ReShade) */
    uint32_t fog_color;           /* 0x00RRGGBB */
    float fog_start, fog_end;
} gfx_state;

typedef struct {
    int windowed;                 /* 0 = borderless window covering the monitor */
    int exclusive;                /* exclusive fullscreen while the game is in front (not windowed) */
    int window_w, window_h;       /* client size when windowed (0 = 75% of the monitor) */
    int render_scale;             /* 0 = match the window height, N = N x the game's mode */
    int msaa;                     /* 1, 2, 4 or 8 samples */
    int anisotropy;               /* 0/1 = off, else max anisotropy for linear filtering */
    int vsync;
    int nearest_upscale;          /* present the image with point instead of linear filtering */
    int stretch;                  /* fill the window instead of keeping the game's aspect */
    int no_fog;                   /* ignore the game's fog */
    int fog_from_w;               /* fog depth = 1/rhw instead of z */
    int gpu_display;              /* render on the GPU driving the monitor, not the fastest one */
    int show_depth;               /* debug: show the effects depth instead of the picture */
    int perf_log;                 /* debug ([debug] perf_log): the renderer's per-frame counts too */
    int render_thread;            /* [video] render_thread: the backend's work on a thread of its own */
    char shader[260];             /* RetroArch preset (.slangp, full path) for the final picture, "" = none */
    char game_dir[260];           /* where librashader.dll lives */
    char backend[16];             /* [video] backend: "d3d11" (default), "vulkan" */
} gfx_options;

int gfx_init(void *game_window, int app_w, int app_h, const gfx_options *opt);
/* place + black-fill the window before there is a device (intro video) */
void gfx_prepare_window(void *game_window, int app_w, int app_h, const gfx_options *opt);
int gfx_ready(void);
void gfx_set_mode(int app_w, int app_h);   /* the game's display mode (coordinate space) */

gfx_tex *gfx_tex_create(int w, int h);
void gfx_tex_upload(gfx_tex *t, const uint32_t *rgba, int w, int h);   /* RGBA8, tightly packed */
void gfx_tex_destroy(gfx_tex *t);
void gfx_tex_size(const gfx_tex *t, int *w, int *h);

void gfx_set_texture(gfx_tex *t);           /* NULL = untextured */
void gfx_set_state(const gfx_state *s);
void gfx_draw(int prim, const gfx_vertex *v, int count);
/* clear in the game's coordinates; rect NULL = everything */
void gfx_clear(const gfx_rect *rect, int color_too, uint32_t color, int depth_too, float depth);
void gfx_present(void);
/* frame interpolation: 0 = the game's frame, 1 = a second scene target for the
 * in-between frame. Draws and clears go to the target in use. */
void gfx_use_target(int t);
void gfx_present_target(int t);
/* the same in two halves: draw scene target t into the back buffer now, and
 * show it at due_ms (QPC milliseconds) from a timer thread, calling on_flip
 * there once it's up; gfx_wait_flip blocks until a pending one has shown */
void gfx_compose_target(int t);
void gfx_flip_at(double due_ms, void (*on_flip)(void));
void gfx_wait_flip(void);

/* settings changed while the game runs (in-game menu) */
const gfx_options *gfx_get_options(void);
void gfx_set_vsync(int on);
void gfx_set_nearest_upscale(int on);
void gfx_set_anisotropy(int n);
void gfx_set_scene(int render_scale, int msaa);
int gfx_set_shader(const char *path);   /* full path or "" */
/* end of frame for target t: every visible world surface's depth goes into its
 * depth buffer, for effects tools (ReShade); done once per frame, on compose
 * at the latest */
void gfx_finish_depth(int t);
/* fn(arg) in order with the drawing calls before and after it: on the render thread when there
   is one, else now. fn may present, compose and flip (gfx_present_target, gfx_compose_target,
   gfx_flip_at, gfx_use_target): frame_rec.c times its presents there, where they happen. */
void gfx_run(void (*fn)(double), double arg);
/* the frame's calls are all made (with a render thread: it may start the next frame meanwhile,
   one frame ahead at most; the UI is taken here) */
void gfx_end_frame(void);
