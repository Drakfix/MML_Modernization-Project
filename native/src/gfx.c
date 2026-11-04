/* gfx.h: the calls go to the backend [video] backend= names (gfx_options.backend), chosen when
 * the window is first prepared or the device made. Direct3D 11 until then and by default.
 *
 * In mml.dll the game's machine code calls us on its own stacks. It keeps its two threads' 16 KB
 * stacks just below the main loop's frames, so whatever the main loop calls must stay shallow:
 * a Vulkan driver compiling a pipeline went deep enough to overwrite a sleeping thread's stack,
 * which then returned into zeros. A backend that asks for it (own_stack) is called on a 2 MB
 * stack of ours instead: each forwarder below calls itself again there, with its arguments
 * copied over (32-bit x86 passes them on the stack, contiguous). The thread block's stack bounds
 * follow, for exception dispatch inside the driver. The runner has no machine code: its game
 * runs on our own big stacks already. */
#include <string.h>
#if defined(_M_IX86) && !defined(MML_RUNNER)
#include <windows.h>
#endif

#include "gfx_backend.h"
#include "log.h"

static const gfx_backend *const g_backends[] = {
    &gfx_backend_d3d11,
#ifdef MML_VULKAN
    &gfx_backend_vulkan,
#endif
};
static const gfx_backend *g_be = &gfx_backend_d3d11;
static int g_chosen;

#if defined(_M_IX86) && !defined(MML_RUNNER)
#define OWN_STACK (2u << 20)
static uint8_t *g_own;            /* the stack */
static DWORD g_own_thread;        /* the game thread, which made it */
static volatile int g_on_own;     /* running on it now (calls nest: the inner ones stay) */

static void own_stack_make(void)
{
    if (g_own || !g_be->own_stack)
        return;
    g_own = (uint8_t *)VirtualAlloc(NULL, OWN_STACK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_own_thread = GetCurrentThreadId();
    if (!g_own)
        log_printf("gfx: no memory for the renderer's stack, it runs on the game's");
}

static int want_own_stack(void)
{
    return g_own && !g_on_own && GetCurrentThreadId() == g_own_thread;
}

/* called as own_jump_fn (fn, args, nbytes, top): fn with the nbytes of arguments at args copied below
   top (cdecl); returns its EAX */
typedef uint32_t(__cdecl *own_jump_fn)(void *fn, const void *args, unsigned nbytes, void *top);
static __declspec(naked) void own_stack_jump(void)
{
    __asm {
        push ebp
        mov ebp, esp
        push esi
        push edi
        mov eax, [ebp + 20]   ; top
        mov ecx, [ebp + 16]   ; nbytes
        sub eax, ecx
        and eax, 0FFFFFFF0h
        mov esi, [ebp + 12]   ; args
        mov edi, eax
        mov esp, eax
        cld
        rep movsb
        call dword ptr [ebp + 8]
        lea esp, [ebp - 8]
        pop edi
        pop esi
        pop ebp
        ret
    }
}

static uint32_t own_stack_call(void *fn, const void *args, unsigned nbytes)
{
    NT_TIB *t = (NT_TIB *)NtCurrentTeb();
    void *base = t->StackBase, *limit = t->StackLimit;
    uint32_t r;
    g_on_own = 1;
    t->StackBase = g_own + OWN_STACK;
    t->StackLimit = g_own;
    r = ((own_jump_fn)(void *)own_stack_jump)(fn, args, nbytes, g_own + OWN_STACK - 64);
    t->StackBase = base;
    t->StackLimit = limit;
    g_on_own = 0;
    return r;
}

/* the forwarder fn, called again on our stack with its own arguments (first: the first one) */
#define OWN(fn, first, bytes)                                                                       \
    do {                                                                                            \
        if (want_own_stack()) {                                                                     \
            own_stack_call((void *)(fn), &(first), (bytes));                                        \
            return;                                                                                 \
        }                                                                                           \
    } while (0)
#define OWN_R(type, fn, first, bytes)                                                               \
    do {                                                                                            \
        if (want_own_stack())                                                                       \
            return (type)own_stack_call((void *)(fn), &(first), (bytes));                           \
    } while (0)
#define OWN0(fn)                                                                                    \
    do {                                                                                            \
        if (want_own_stack()) {                                                                     \
            own_stack_call((void *)(fn), NULL, 0);                                                  \
            return;                                                                                 \
        }                                                                                           \
    } while (0)
#define OWN0_R(type, fn)                                                                            \
    do {                                                                                            \
        if (want_own_stack())                                                                       \
            return (type)own_stack_call((void *)(fn), NULL, 0);                                     \
    } while (0)
#else
static void own_stack_make(void) {}
#define OWN(fn, first, bytes) ((void)0)
#define OWN_R(type, fn, first, bytes) ((void)0)
#define OWN0(fn) ((void)0)
#define OWN0_R(type, fn) ((void)0)
#endif

static void choose(const gfx_options *opt)
{
    size_t k;
    if (g_chosen)
        return;
    g_chosen = 1;
    if (opt && opt->backend[0]) {
        for (k = 0; k < sizeof g_backends / sizeof g_backends[0]; k++)
            if (_stricmp(opt->backend, g_backends[k]->name) == 0)
                break;
        if (k < sizeof g_backends / sizeof g_backends[0])
            g_be = g_backends[k];
        else
            log_printf("gfx: no backend \"%s\", using %s", opt->backend, g_be->name);
    }
    own_stack_make();
}

int gfx_init(void *game_window, int app_w, int app_h, const gfx_options *opt)
{
    choose(opt);
    OWN_R(int, gfx_init, game_window, 16);
    return g_be->init(game_window, app_w, app_h, opt);
}

void gfx_prepare_window(void *game_window, int app_w, int app_h, const gfx_options *opt)
{
    choose(opt);
    OWN(gfx_prepare_window, game_window, 16);
    g_be->prepare_window(game_window, app_w, app_h, opt);
}

int gfx_ready(void)
{
    return g_be->ready();   /* (a flag) */
}

void gfx_set_mode(int app_w, int app_h)
{
    OWN(gfx_set_mode, app_w, 8);
    g_be->set_mode(app_w, app_h);
}

gfx_tex *gfx_tex_create(int w, int h)
{
    OWN_R(gfx_tex *, gfx_tex_create, w, 8);
    return g_be->tex_create(w, h);
}

void gfx_tex_upload(gfx_tex *t, const uint32_t *rgba, int w, int h)
{
    OWN(gfx_tex_upload, t, 16);
    g_be->tex_upload(t, rgba, w, h);
}

void gfx_tex_destroy(gfx_tex *t)
{
    OWN(gfx_tex_destroy, t, 4);
    g_be->tex_destroy(t);
}

void gfx_tex_size(const gfx_tex *t, int *w, int *h)
{
    g_be->tex_size(t, w, h);   /* (reads two fields) */
}

void gfx_set_texture(gfx_tex *t)
{
    OWN(gfx_set_texture, t, 4);
    g_be->set_texture(t);
}

void gfx_set_state(const gfx_state *s)
{
    OWN(gfx_set_state, s, 4);
    g_be->set_state(s);
}

void gfx_draw(int prim, const gfx_vertex *v, int count)
{
    OWN(gfx_draw, prim, 12);
    g_be->draw(prim, v, count);
}

void gfx_clear(const gfx_rect *rect, int color_too, uint32_t color, int depth_too, float depth)
{
    OWN(gfx_clear, rect, 20);
    g_be->clear(rect, color_too, color, depth_too, depth);
}

void gfx_present(void)
{
    OWN0(gfx_present);
    g_be->present();
}

void gfx_use_target(int t)
{
    OWN(gfx_use_target, t, 4);
    g_be->use_target(t);
}

void gfx_present_target(int t)
{
    OWN(gfx_present_target, t, 4);
    g_be->present_target(t);
}

void gfx_compose_target(int t)
{
    OWN(gfx_compose_target, t, 4);
    g_be->compose_target(t);
}

void gfx_flip_at(double due_ms, void (*on_flip)(void))
{
    OWN(gfx_flip_at, due_ms, 12);
    g_be->flip_at(due_ms, on_flip);
}

void gfx_wait_flip(void)
{
    g_be->wait_flip();   /* (waits for an event) */
}

const gfx_options *gfx_get_options(void)
{
    return g_be->get_options();
}

void gfx_set_vsync(int on)
{
    OWN(gfx_set_vsync, on, 4);
    g_be->set_vsync(on);
}

void gfx_set_nearest_upscale(int on)
{
    OWN(gfx_set_nearest_upscale, on, 4);
    g_be->set_nearest_upscale(on);
}

void gfx_set_anisotropy(int n)
{
    OWN(gfx_set_anisotropy, n, 4);
    g_be->set_anisotropy(n);
}

void gfx_set_scene(int render_scale, int msaa)
{
    OWN(gfx_set_scene, render_scale, 8);
    g_be->set_scene(render_scale, msaa);
}

int gfx_set_shader(const char *path)
{
    OWN_R(int, gfx_set_shader, path, 4);
    return g_be->set_shader(path);
}

void gfx_finish_depth(int t)
{
    OWN(gfx_finish_depth, t, 4);
    g_be->finish_depth(t);
}
