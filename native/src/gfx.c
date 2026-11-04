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
 * runs on our own big stacks already.
 *
 * The render thread ([video] render_thread, on by default): the drawing calls go into a queue
 * instead, in order, and a thread of ours runs them on the backend, so the game thread gets on
 * with the next step meanwhile (on the Retroid the backend took about a fifth of the game
 * thread). Each call's data is copied (vertices, pixels, states); a texture is a handle of ours
 * whose backend texture the thread makes when it gets there. The calls that place or remake the
 * window, its swapchain or the scene targets (init, the mode, the scene, the shader preset) wait
 * for the queue to empty and run on the calling thread: on Windows that is the window's own
 * thread, which must not be waiting for a thread that sends its window messages. The UI's
 * callbacks run at each queued frame end on the calling thread (gfx_ui_snapshot). */
#include <stdlib.h>
#include <string.h>
#include <windows.h>   /* (native/compat elsewhere) */
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

#include "gfx_backend.h"
#include "log.h"

static const gfx_backend *const g_backends[] = {
#ifdef _WIN32
    &gfx_backend_d3d11,
#endif
#ifdef MML_VULKAN
    &gfx_backend_vulkan,
#endif
};
#ifdef _WIN32
static const gfx_backend *g_be = &gfx_backend_d3d11;   /* unless [video] backend= names another */
#else
static const gfx_backend *g_be = &gfx_backend_vulkan;
#endif
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


/* ---------------------------------------------------------------- the render thread */

#define CHUNK_BYTES (256u << 10)      /* small: the thread works alongside, not after the frame */
#define MAX_CHUNKS 256                 /* 64 MB queued at most: then the game thread waits */
#define INLINE_MAX (CHUNK_BYTES / 4)   /* bigger payloads go in a block of their own */

typedef struct chunk {
    struct chunk *next;
    size_t used;
    int frame_end;                     /* the last chunk of a frame (counts for the frame limit) */
    double data[CHUNK_BYTES / sizeof(double)];
} chunk;

typedef struct {
    gfx_tex *real;                     /* the backend's (made on the render thread) */
    int w, h;
} tex_proxy;

enum { Q_TEX_CREATE = 1, Q_TEX_UPLOAD, Q_TEX_DESTROY, Q_SET_TEXTURE, Q_SET_STATE, Q_DRAW, Q_CLEAR, Q_PRESENT,
       Q_USE_TARGET, Q_PRESENT_TARGET, Q_COMPOSE_TARGET, Q_FLIP_AT, Q_RUN, Q_SET_VSYNC, Q_SET_NEAREST, Q_SET_ANISO,
       Q_FINISH_DEPTH };

typedef struct { uint32_t op, size; } qhead;   /* size: the whole command, a multiple of 8 */
typedef struct { tex_proxy *t; } q_tex;
typedef struct { tex_proxy *t; int w, h; uint32_t *ext; } q_upload;            /* pixels follow, or in ext */
typedef struct { int prim, count; gfx_vertex *ext; } q_draw;                   /* vertices follow, or in ext */
typedef struct { gfx_rect rect; int has_rect, color_too, depth_too; uint32_t color; float depth; } q_clear;
typedef struct { int v; } q_int;
typedef struct { double due; void (*cb)(void); } q_flip;
typedef struct { void (*fn)(double); double arg; } q_run;

#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif
static int g_rt;                       /* the render thread runs */
static THREAD_LOCAL int t_rt;          /* this is the render thread: calls go straight to the backend */
#define QUEUED (g_rt && !t_rt)
/* a texture as the backend knows it (callbacks on the render thread pass ours) */
#define REAL_TEX(t) (g_rt && (t) ? ((tex_proxy *)(t))->real : (t))
static CRITICAL_SECTION g_qlock;       /* over the lists and counts below */
static HANDLE g_work, g_progress;      /* auto-reset: chunks to run / the thread got on */
static chunk *g_cur;                   /* being filled, the producing thread's alone */
static chunk *g_full_head, *g_full_tail, *g_free;
static int g_nchunks, g_busy;
static unsigned g_frames_queued, g_frames_done;
static int g_early_tex_logged;

/* The producing thread waits for the render thread to get on. On Windows that is the window's
   own thread, and what the render thread does may send the window messages (placing it,
   DXGI and the drivers while presenting) that wait for this thread in turn: they are handled
   here meanwhile, as Windows does in its own blocking calls. Only sent messages run, not the
   posted ones the game's loop takes. */
static void q_sleep(void)
{
#ifdef _WIN32
    if (MsgWaitForMultipleObjects(1, &g_progress, FALSE, 10, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
        MSG m;
        PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
#else
    WaitForSingleObject(g_progress, 10);
#endif
}

static chunk *get_chunk(void)
{
    for (;;) {
        chunk *c = NULL;
        int make = 0;
        EnterCriticalSection(&g_qlock);
        if (g_free) {
            c = g_free;
            g_free = c->next;
        } else if (g_nchunks < MAX_CHUNKS) {
            g_nchunks++;
            make = 1;
        }
        LeaveCriticalSection(&g_qlock);
        if (make && !(c = (chunk *)malloc(sizeof *c))) {
            EnterCriticalSection(&g_qlock);
            g_nchunks--;
            LeaveCriticalSection(&g_qlock);
        }
        if (c) {
            c->used = 0;
            c->frame_end = 0;
            return c;
        }
        q_sleep();
    }
}

/* the chunk being filled goes to the render thread (an empty one too when it ends a frame) */
static void q_publish(int frame_end)
{
    chunk *c = g_cur;
    if (!c) {
        if (!frame_end)
            return;
        c = get_chunk();
    }
    g_cur = NULL;
    c->next = NULL;
    c->frame_end = frame_end;
    EnterCriticalSection(&g_qlock);
    if (g_full_tail)
        g_full_tail->next = c;
    else
        g_full_head = c;
    g_full_tail = c;
    if (frame_end)
        g_frames_queued++;
    LeaveCriticalSection(&g_qlock);
    SetEvent(g_work);
}

/* room for a command with `payload` bytes after its head, in the chunk being filled */
static void *q_add(uint32_t op, size_t payload)
{
    size_t size = (sizeof(qhead) + payload + 7) & ~(size_t)7;
    qhead *h;
    if (g_cur && g_cur->used + size > CHUNK_BYTES)
        q_publish(0);
    if (!g_cur)
        g_cur = get_chunk();
    h = (qhead *)((uint8_t *)g_cur->data + g_cur->used);
    h->op = op;
    h->size = (uint32_t)size;
    g_cur->used += size;
    return h + 1;
}

static int q_wait(int frames_ahead)   /* until at most frames_ahead frames are queued; -1: until idle */
{
    for (;;) {
        int done;
        EnterCriticalSection(&g_qlock);
        done = frames_ahead < 0 ? !g_full_head && !g_busy : (int)(g_frames_queued - g_frames_done) <= frames_ahead;
        LeaveCriticalSection(&g_qlock);
        if (done)
            return 1;
        q_sleep();
    }
}

/* everything queued done: for the calls that run on the calling thread */
static void q_drain(void)
{
    if (!QUEUED)
        return;
    q_publish(0);
    q_wait(-1);
}

/* the end of a frame: the render thread may work on it while the game makes the next one, but
   no further ahead than that */
static void q_frame_end(void)
{
    q_publish(1);
    q_wait(1);
}

static void run_cmd(uint32_t op, void *p)
{
    switch (op) {
    case Q_TEX_CREATE: {
        tex_proxy *t = ((q_tex *)p)->t;
        t->real = g_be->tex_create(t->w, t->h);
        break;
    }
    case Q_TEX_UPLOAD: {
        q_upload *u = (q_upload *)p;
        const uint32_t *px = u->ext ? u->ext : (const uint32_t *)(u + 1);
        if (u->t && u->t->real)
            g_be->tex_upload(u->t->real, px, u->w, u->h);
        free(u->ext);
        break;
    }
    case Q_TEX_DESTROY: {
        tex_proxy *t = ((q_tex *)p)->t;
        if (t->real)
            g_be->tex_destroy(t->real);
        free(t);
        break;
    }
    case Q_SET_TEXTURE: {
        tex_proxy *t = ((q_tex *)p)->t;
        g_be->set_texture(t ? t->real : NULL);
        break;
    }
    case Q_SET_STATE:
        g_be->set_state((const gfx_state *)p);
        break;
    case Q_DRAW: {
        q_draw *d = (q_draw *)p;
        g_be->draw(d->prim, d->ext ? d->ext : (const gfx_vertex *)(d + 1), d->count);
        free(d->ext);
        break;
    }
    case Q_CLEAR: {
        q_clear *c = (q_clear *)p;
        g_be->clear(c->has_rect ? &c->rect : NULL, c->color_too, c->color, c->depth_too, c->depth);
        break;
    }
    case Q_PRESENT: g_be->present(); break;
    case Q_USE_TARGET: g_be->use_target(((q_int *)p)->v); break;
    case Q_PRESENT_TARGET: g_be->present_target(((q_int *)p)->v); break;
    case Q_COMPOSE_TARGET: g_be->compose_target(((q_int *)p)->v); break;
    case Q_FLIP_AT: g_be->flip_at(((q_flip *)p)->due, ((q_flip *)p)->cb); break;
    case Q_RUN: ((q_run *)p)->fn(((q_run *)p)->arg); break;
    case Q_SET_VSYNC: g_be->set_vsync(((q_int *)p)->v); break;
    case Q_SET_NEAREST: g_be->set_nearest_upscale(((q_int *)p)->v); break;
    case Q_SET_ANISO: g_be->set_anisotropy(((q_int *)p)->v); break;
    case Q_FINISH_DEPTH: g_be->finish_depth(((q_int *)p)->v); break;
    default: break;
    }
}

static DWORD WINAPI render_thread(void *arg)
{
    (void)arg;
    t_rt = 1;
    for (;;) {
        chunk *c;
        size_t at;
        EnterCriticalSection(&g_qlock);
        c = g_full_head;
        if (c) {
            g_full_head = c->next;
            if (!g_full_head)
                g_full_tail = NULL;
        }
        g_busy = c != NULL;
        LeaveCriticalSection(&g_qlock);
        if (!c) {
            SetEvent(g_progress);
            WaitForSingleObject(g_work, INFINITE);
            continue;
        }
        for (at = 0; at < c->used;) {
            qhead *h = (qhead *)((uint8_t *)c->data + at);
            run_cmd(h->op, h + 1);
            at += h->size;
        }
        EnterCriticalSection(&g_qlock);
        c->next = g_free;
        g_free = c;
        if (c->frame_end)
            g_frames_done++;
        LeaveCriticalSection(&g_qlock);
        SetEvent(g_progress);
    }
}

static void start_render_thread(void)
{
    HANDLE t;
    InitializeCriticalSection(&g_qlock);
    g_work = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_progress = CreateEventW(NULL, FALSE, FALSE, NULL);
    t = g_work && g_progress ? CreateThread(NULL, 8u << 20, render_thread, NULL, 0, NULL) : NULL;
    if (!t) {
        log_printf("gfx: the render thread did not start, drawing on the game thread");
        return;
    }
    SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
    gfx_ui_threaded(1);
    g_rt = 1;
    log_printf("gfx: drawing on a render thread");
}

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
    if (opt && opt->render_thread)
        start_render_thread();
}

int gfx_init(void *game_window, int app_w, int app_h, const gfx_options *opt)
{
    choose(opt);
    OWN_R(int, gfx_init, game_window, 16);
    q_drain();
    return g_be->init(game_window, app_w, app_h, opt);
}

void gfx_prepare_window(void *game_window, int app_w, int app_h, const gfx_options *opt)
{
    choose(opt);
    OWN(gfx_prepare_window, game_window, 16);
    q_drain();
    g_be->prepare_window(game_window, app_w, app_h, opt);
}

int gfx_ready(void)
{
    return g_be->ready();   /* (a flag) */
}

void gfx_set_mode(int app_w, int app_h)
{
    OWN(gfx_set_mode, app_w, 8);
    q_drain();
    g_be->set_mode(app_w, app_h);
}

gfx_tex *gfx_tex_create(int w, int h)
{
    if (QUEUED) {
        tex_proxy *t = (tex_proxy *)calloc(1, sizeof *t);
        if (!t)
            return NULL;
        t->w = w;
        t->h = h;
        ((q_tex *)q_add(Q_TEX_CREATE, sizeof(q_tex)))->t = t;
        return (gfx_tex *)t;
    }
    if (!g_chosen && !g_early_tex_logged++)
        log_printf("gfx: a texture before the renderer was set up");
    OWN_R(gfx_tex *, gfx_tex_create, w, 8);
    return g_be->tex_create(w, h);
}

void gfx_tex_upload(gfx_tex *t, const uint32_t *rgba, int w, int h)
{
    if (QUEUED) {
        size_t bytes = (size_t)w * h * 4;
        q_upload *u;
        if (!t || !rgba || w <= 0 || h <= 0)
            return;
        u = (q_upload *)q_add(Q_TEX_UPLOAD, sizeof *u + (bytes <= INLINE_MAX ? bytes : 0));
        u->t = (tex_proxy *)t;
        u->w = w;
        u->h = h;
        u->ext = NULL;
        if (bytes <= INLINE_MAX)
            memcpy(u + 1, rgba, bytes);
        else if ((u->ext = (uint32_t *)malloc(bytes)) != NULL)
            memcpy(u->ext, rgba, bytes);
        else
            u->t = NULL;   /* (no memory: skipped) */
        return;
    }
    OWN(gfx_tex_upload, t, 16);
    g_be->tex_upload(REAL_TEX(t), rgba, w, h);
}

void gfx_tex_destroy(gfx_tex *t)
{
    if (QUEUED) {
        if (t)
            ((q_tex *)q_add(Q_TEX_DESTROY, sizeof(q_tex)))->t = (tex_proxy *)t;
        return;
    }
    OWN(gfx_tex_destroy, t, 4);
    g_be->tex_destroy(t);
}

void gfx_tex_size(const gfx_tex *t, int *w, int *h)
{
    if (QUEUED) {
        *w = t ? ((const tex_proxy *)t)->w : 0;
        *h = t ? ((const tex_proxy *)t)->h : 0;
        return;
    }
    g_be->tex_size(REAL_TEX(t), w, h);   /* (reads two fields) */
}

void gfx_set_texture(gfx_tex *t)
{
    if (QUEUED) {
        ((q_tex *)q_add(Q_SET_TEXTURE, sizeof(q_tex)))->t = (tex_proxy *)t;
        return;
    }
    OWN(gfx_set_texture, t, 4);
    g_be->set_texture(REAL_TEX(t));
}

void gfx_set_state(const gfx_state *s)
{
    if (QUEUED) {
        *(gfx_state *)q_add(Q_SET_STATE, sizeof *s) = *s;
        return;
    }
    OWN(gfx_set_state, s, 4);
    g_be->set_state(s);
}

void gfx_draw(int prim, const gfx_vertex *v, int count)
{
    if (QUEUED) {
        size_t bytes = sizeof *v * (size_t)(count > 0 ? count : 0);
        q_draw *d;
        if (count <= 0 || !v)
            return;
        d = (q_draw *)q_add(Q_DRAW, sizeof *d + (bytes <= INLINE_MAX ? bytes : 0));
        d->prim = prim;
        d->count = count;
        d->ext = NULL;
        if (bytes <= INLINE_MAX)
            memcpy(d + 1, v, bytes);
        else if ((d->ext = (gfx_vertex *)malloc(bytes)) != NULL)
            memcpy(d->ext, v, bytes);
        else
            d->count = 0;
        return;
    }
    OWN(gfx_draw, prim, 12);
    g_be->draw(prim, v, count);
}

void gfx_clear(const gfx_rect *rect, int color_too, uint32_t color, int depth_too, float depth)
{
    if (QUEUED) {
        q_clear *c = (q_clear *)q_add(Q_CLEAR, sizeof(q_clear));
        c->has_rect = rect != NULL;
        if (rect)
            c->rect = *rect;
        c->color_too = color_too;
        c->color = color;
        c->depth_too = depth_too;
        c->depth = depth;
        return;
    }
    OWN(gfx_clear, rect, 20);
    g_be->clear(rect, color_too, color, depth_too, depth);
}

static void q_int_cmd(uint32_t op, int v)
{
    ((q_int *)q_add(op, sizeof(q_int)))->v = v;
}

void gfx_present(void)
{
    if (QUEUED) {
        gfx_ui_snapshot();
        q_add(Q_PRESENT, 0);
        q_frame_end();
        return;
    }
    OWN0(gfx_present);
    g_be->present();
}

void gfx_use_target(int t)
{
    if (QUEUED) {
        q_int_cmd(Q_USE_TARGET, t);
        return;
    }
    OWN(gfx_use_target, t, 4);
    g_be->use_target(t);
}

void gfx_present_target(int t)
{
    if (QUEUED) {
        gfx_ui_snapshot();
        q_int_cmd(Q_PRESENT_TARGET, t);
        q_publish(0);   /* (the thread can start on it) */
        return;
    }
    OWN(gfx_present_target, t, 4);
    g_be->present_target(t);
}

void gfx_compose_target(int t)
{
    if (QUEUED) {
        gfx_ui_snapshot();
        q_int_cmd(Q_COMPOSE_TARGET, t);
        return;
    }
    OWN(gfx_compose_target, t, 4);
    g_be->compose_target(t);
}

void gfx_flip_at(double due_ms, void (*on_flip)(void))
{
    if (QUEUED) {
        q_flip *f = (q_flip *)q_add(Q_FLIP_AT, sizeof(q_flip));
        f->due = due_ms;
        f->cb = on_flip;
        return;
    }
    OWN(gfx_flip_at, due_ms, 12);
    g_be->flip_at(due_ms, on_flip);
}

void gfx_wait_flip(void)
{
    q_drain();
    g_be->wait_flip();   /* (waits for an event) */
}

const gfx_options *gfx_get_options(void)
{
    return g_be->get_options();
}

void gfx_set_vsync(int on)
{
    if (QUEUED) {
        q_int_cmd(Q_SET_VSYNC, on);
        return;
    }
    OWN(gfx_set_vsync, on, 4);
    g_be->set_vsync(on);
}

void gfx_set_nearest_upscale(int on)
{
    if (QUEUED) {
        q_int_cmd(Q_SET_NEAREST, on);
        return;
    }
    OWN(gfx_set_nearest_upscale, on, 4);
    g_be->set_nearest_upscale(on);
}

void gfx_set_anisotropy(int n)
{
    if (QUEUED) {
        q_int_cmd(Q_SET_ANISO, n);
        return;
    }
    OWN(gfx_set_anisotropy, n, 4);
    g_be->set_anisotropy(n);
}

void gfx_set_scene(int render_scale, int msaa)
{
    OWN(gfx_set_scene, render_scale, 8);
    q_drain();
    g_be->set_scene(render_scale, msaa);
}

int gfx_set_shader(const char *path)
{
    OWN_R(int, gfx_set_shader, path, 4);
    q_drain();
    return g_be->set_shader(path);
}

void gfx_finish_depth(int t)
{
    if (QUEUED) {
        q_int_cmd(Q_FINISH_DEPTH, t);
        return;
    }
    OWN(gfx_finish_depth, t, 4);
    g_be->finish_depth(t);
}

void gfx_end_frame(void)
{
    if (QUEUED) {
        gfx_ui_snapshot();
        q_frame_end();
    }
}

void gfx_run(void (*fn)(double), double arg)
{
    if (QUEUED) {
        q_run *r = (q_run *)q_add(Q_RUN, sizeof(q_run));
        r->fn = fn;
        r->arg = arg;
        return;
    }
    fn(arg);
}
