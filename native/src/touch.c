/* On-screen touch controls (Android): a controller drawn over the game.
 *
 * Layout, mirroring a standard controller and kept clear of the HUD (the minimap in the top-left
 * corner, the life and weapon gauges along the bottom corners):
 *   - the triggers and shoulder buttons in a row along the top edge: LT LB at the left, just
 *     right of the minimap, RB RT in the right corner (outermost = the triggers, as an index
 *     finger finds them);
 *   - the left stick: touching anywhere in the left part of the screen puts the stick there
 *     (a floating stick, so the thumb never has to find it); it rests drawn bottom left;
 *   - the camera: the rest of the right part works as a floating right stick, its centre
 *     following the finger past the edge so a turn reverses at once. A rate stick and not a
 *     swipe: the modern camera is built around the right stick's analog speed (camera speed,
 *     aim sensitivity, aim assist slowing it over enemies, the lock-on target flick), so all of
 *     that works unchanged. Double-tap and hold there for R3 (shoulder swap), and on the
 *     stick's side for L3 (modern controls on and off), as clicking a stick in;
 *   - the face buttons A B X Y in a diamond on the right;
 *   - Back and Start small, under the outer ends of the top rows: Back under LT, Start under RT
 *     (the bottom centre is the subtitles', the top centre the dialogue boxes'). Back + Start
 *     together opens the settings menu, as on a controller.
 * No D-pad: the game turns the left stick into D-pad presses in menus, and controls.c and our
 * menus take either. Labels follow the button prompts (padprompt.c; Xbox for Auto).
 *
 * Threads: SDL's finger events arrive on the game thread (plat_pump), the overlay is drawn there
 * too (gfx_ui_snapshot); input.c's SDL thread reads the resulting pad state under g_lock. A tap
 * shorter than a game poll still counts: a button stays down at least MIN_HOLD_MS. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "touch.h"

enum { MODE_AUTO, MODE_ON, MODE_OFF };
static volatile int g_mode = MODE_AUTO;
static volatile int g_physical;   /* a controller is connected (input.c's SDL thread) */

int touch_visible(void)
{
#ifdef __ANDROID__
    return g_mode == MODE_ON || (g_mode == MODE_AUTO && !g_physical);
#else
    (void)g_mode;
    return 0;
#endif
}

#ifdef __ANDROID__
#include <math.h>
#include <SDL3/SDL.h>

#include "gfx_ui.h"
#include "padprompt.h"
#include "plat.h"
#include "plat_sdl.h"
#include "ui_style.h"

#define MAX_FINGERS 10
#define MIN_HOLD_MS 80        /* a tap's press lasts at least this long (the game polls every 33 ms) */
#define DOUBLE_TAP_MS 300     /* stick / camera area: a second touch this soon after a tap holds L3 / R3 */
#define TAP_MS 220            /* (a touch this short, that barely moved, is a tap) */

enum { W_NONE, W_STICK, W_CAMERA, W_BUTTON };

/* the buttons drawn (PB_* from padprompt.h); L3 and R3 have none */
static const int DRAWN[] = { PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_BACK, PB_START };
#define NDRAWN ((int)(sizeof DRAWN / sizeof DRAWN[0]))

static const WORD BIT[PB_COUNT] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, 0, 0, BTN_L3, BTN_R3, BTN_BACK,
                                    BTN_START };

typedef struct {
    int used, what, btn, click;   /* click: the double-tap's L3 / R3 is held */
    SDL_FingerID id;
    float ax, ay, x, y, moved;   /* stick centre, finger, farthest from where it landed */
    float x0, y0;
    uint32_t t0, btn_t0;         /* when it landed; when it took its button */
} finger;

/* the layout for a w x h screen, in pixels (U = 1% of the height) */
static struct {
    int w, h;
    float u, sx;
    float zone_y, split_x;       /* the stick and camera areas: below zone_y, left / right of split_x */
    float stick_x, stick_y, stick_r, knob_r;
    float cam_x, cam_y, cam_r;
    float bx[PB_COUNT], by[PB_COUNT], bw[PB_COUNT], bh[PB_COUNT];   /* face buttons: centre, bw = radius */
} L;

static finger g_f[MAX_FINGERS];
static uint32_t g_tap_t[2];       /* the last tap on the stick's side [0], the camera's [1] */
static float g_tap_x[2], g_tap_y[2];
static int g_installed, g_cs_ready;
static float g_opacity = 0.4f;    /* [controls] touch_opacity: idle; pressed is stronger */
static float g_aspect;            /* [video] aspect: the picture's width / height, 0 = the whole screen */
static CRITICAL_SECTION g_lock;   /* over g_pub and g_until */
static pad_t g_pub;               /* fingers down now: buttons (triggers as BTN bits below), sticks */
static uint32_t g_until[PB_COUNT];   /* a press shorter than MIN_HOLD_MS lasts until then */
static int g_pub_lt, g_pub_rt;
static int g_was_visible;

static int is_face(int b)
{
    return b >= PB_A && b <= PB_Y;
}

static void layout(int w, int h)
{
    float u, sx, top, bw, bh, hud = 0;
    int i;
    if (w == L.w && h == L.h)
        return;
    memset(&L, 0, sizeof L);
    L.w = w;
    L.h = h;
    L.u = u = (float)h / 100.0f;
    /* the screen's rounded corners and camera cut-out: a margin at the sides */
    L.sx = sx = (float)w * 0.035f > 4 * u ? (float)w * 0.035f : 4 * u;
    top = 3.5f * u;
    bw = 17 * u;
    bh = 10 * u;
    /* the HUD's left edge: the screen's, or with bars at the sides the picture's */
    if (g_aspect > 0 && (float)w > (float)h * g_aspect)
        hud = ((float)w - (float)h * g_aspect) / 2;
    /* the top row: triggers outside, shoulders inside; the left pair right of the minimap (the
       corner's first quarter of the height, at full HUD size), clear of the boss gauges' middle */
    L.bx[PB_LT] = hud + 27 * u > sx ? hud + 27 * u : sx;
    L.bx[PB_LB] = L.bx[PB_LT] + bw + 1.5f * u;
    L.bx[PB_RT] = (float)w - sx - bw;
    L.bx[PB_RB] = L.bx[PB_RT] - bw - 1.5f * u;
    for (i = PB_LB; i <= PB_RT; i++) {
        L.by[i] = top;
        L.bw[i] = bw;
        L.bh[i] = bh;
    }
    /* Back and Start, small under the top rows' outer ends (the subtitles have the bottom centre,
       the dialogue boxes the top centre, the gauges the bottom corners) */
    L.bw[PB_BACK] = L.bw[PB_START] = 15 * u;
    L.bh[PB_BACK] = L.bh[PB_START] = 7 * u;
    L.by[PB_BACK] = L.by[PB_START] = top + bh + 1.5f * u;
    L.bx[PB_BACK] = L.bx[PB_LT];
    L.bx[PB_START] = L.bx[PB_RT] + bw - L.bw[PB_START];
    /* the face buttons: a diamond, above the weapon gauge's corner */
    {
        float cx = (float)w - sx - 24 * u, cy = 56 * u, d = 14.5f * u, r = 7.5f * u;
        L.bx[PB_A] = cx;     L.by[PB_A] = cy + d;
        L.bx[PB_B] = cx + d; L.by[PB_B] = cy;
        L.bx[PB_X] = cx - d; L.by[PB_X] = cy;
        L.bx[PB_Y] = cx;     L.by[PB_Y] = cy - d;
        for (i = PB_A; i <= PB_Y; i++)
            L.bw[i] = L.bh[i] = r;
        L.cam_r = 11 * u;
        L.cam_x = cx - d - r - 6 * u - L.cam_r;
        L.cam_y = 68 * u;
    }
    L.stick_r = 14 * u;
    L.knob_r = 6.5f * u;
    /* (clear of the life gauge in the corner below) */
    L.stick_x = (hud + 17 * u > sx + 4 * u ? hud + 17 * u : sx + 4 * u) + L.stick_r;
    L.stick_y = 64 * u;
    L.zone_y = L.by[PB_BACK] + L.bh[PB_BACK] + 1.5f * u;
    L.split_x = (float)w * 0.45f;
    log_printf("touch: layout for %dx%d", w, h);
}

/* how close (x, y) is to button b, as a share of its hit size (< 1 is on it) */
static float btn_dist(int b, float x, float y)
{
    float pad = 1.5f * L.u;
    if (is_face(b)) {
        float dx = x - L.bx[b], dy = y - L.by[b];
        return sqrtf(dx * dx + dy * dy) / (L.bw[b] * 1.3f);   /* (a little larger than drawn) */
    } else {
        float cx = L.bx[b] + L.bw[b] / 2, cy = L.by[b] + L.bh[b] / 2;
        float ex = fabsf(x - cx) / (L.bw[b] / 2 + pad), ey = fabsf(y - cy) / (L.bh[b] / 2 + pad);
        return ex > ey ? ex : ey;
    }
}

/* the button under (x, y), the nearest if several; -1 for none */
static int hit_button(float x, float y)
{
    int i, best = -1;
    float bd = 1.0f;
    for (i = 0; i < NDRAWN; i++) {
        float d = btn_dist(DRAWN[i], x, y);
        if (d < bd) {
            bd = d;
            best = DRAWN[i];
        }
    }
    return best;
}

/* a touch on the stick's side (0) or the camera's (1): the second of a double tap? */
static int double_tap(int side, float x, float y, uint32_t now)
{
    float dx = x - g_tap_x[side], dy = y - g_tap_y[side];
    int yes = g_tap_t[side] && now - g_tap_t[side] < DOUBLE_TAP_MS && sqrtf(dx * dx + dy * dy) < 10 * L.u;
    g_tap_t[side] = 0;
    return yes;
}

static int holder(int what)
{
    int i;
    for (i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].what == what)
            return 1;
    return 0;
}

static SHORT axis(float v)
{
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    return (SHORT)(v * 32767.0f);
}

/* a stick's deflection from its centre (the camera's starts past a small dead spot, at a third,
   so a short drag already turns past the game's own dead zone) */
static void deflect(const finger *f, float r, int camera, SHORT *ox, SHORT *oy)
{
    float dx = (f->x - f->ax) / r, dy = (f->ay - f->y) / r, m = sqrtf(dx * dx + dy * dy);
    if (m > 1) {
        dx /= m;
        dy /= m;
        m = 1;
    }
    if (camera && m > 0) {
        float m2 = m < 0.06f ? 0 : 0.33f + 0.67f * (m - 0.06f) / 0.94f;
        dx *= m2 / m;
        dy *= m2 / m;
    }
    *ox = axis(dx);
    *oy = axis(dy);
}

/* the fingers' pad, for the SDL thread */
static void publish(void)
{
    pad_t p;
    int i, lt = 0, rt = 0;
    memset(&p, 0, sizeof p);
    for (i = 0; i < MAX_FINGERS; i++) {
        const finger *f = &g_f[i];
        if (!f->used)
            continue;
        if (f->what == W_BUTTON && f->btn >= 0) {
            p.wButtons |= BIT[f->btn];
            lt |= f->btn == PB_LT;
            rt |= f->btn == PB_RT;
        } else if (f->what == W_STICK) {
            deflect(f, L.stick_r, 0, &p.sThumbLX, &p.sThumbLY);
            if (f->click)
                p.wButtons |= BTN_L3;
        } else if (f->what == W_CAMERA) {
            deflect(f, L.cam_r, 1, &p.sThumbRX, &p.sThumbRY);
            if (f->click)
                p.wButtons |= BTN_R3;
        }
    }
    EnterCriticalSection(&g_lock);
    g_pub = p;
    g_pub_lt = lt;
    g_pub_rt = rt;
    LeaveCriticalSection(&g_lock);
}

/* button b let go by a finger that took it at t0: a short press still lasts MIN_HOLD_MS */
static void let_go(int b, uint32_t t0)
{
    if (b < 0)
        return;
    EnterCriticalSection(&g_lock);
    g_until[b] = t0 + MIN_HOLD_MS;
    LeaveCriticalSection(&g_lock);
}

static void release_all(void)
{
    int i;
    for (i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].what == W_BUTTON)
            let_go(g_f[i].btn, g_f[i].btn_t0);
    memset(g_f, 0, sizeof g_f);
    if (g_cs_ready)
        publish();
}

void touch_release_all(void)
{
    release_all();
}

void touch_finger(int kind, uint64_t id, float nx, float ny)
{
    finger *f = NULL;
    int i, w = 0, h = 0;
    float x, y;
    uint32_t now = plat_ticks_ms();
    if (!g_cs_ready)
        return;
    if (!touch_visible()) {
        if (g_was_visible)
            release_all();
        g_was_visible = 0;
        return;
    }
    g_was_visible = 1;
    if (plat_sdl_window())
        SDL_GetWindowSizeInPixels(plat_sdl_window(), &w, &h);
    if (w <= 0 || h <= 0)
        return;
    layout(w, h);
    x = nx * (float)w;
    y = ny * (float)h;
    for (i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].id == (SDL_FingerID)id) {
            f = &g_f[i];
            break;
        }
    if (kind == TOUCH_DOWN) {
        int b;
        if (f)   /* (a lost up: start over) */
            f->used = 0;
        for (i = 0; i < MAX_FINGERS && g_f[i].used; i++)
            ;
        if (i == MAX_FINGERS)
            return;
        f = &g_f[i];
        memset(f, 0, sizeof *f);
        f->used = 1;
        f->id = (SDL_FingerID)id;
        f->x = f->x0 = f->ax = x;
        f->y = f->y0 = f->ay = y;
        f->t0 = f->btn_t0 = now;
        f->btn = -1;
        b = hit_button(x, y);
        if (b >= 0) {
            f->what = W_BUTTON;
            f->btn = b;
        } else if (y >= L.zone_y && x < L.split_x && !holder(W_STICK)) {
            f->what = W_STICK;
            f->click = double_tap(0, x, y, now);
            /* (the stick's ring stays on the screen) */
            if (f->ax < L.sx + L.stick_r * 0.5f)
                f->ax = L.sx + L.stick_r * 0.5f;
            if (f->ay > (float)h - L.stick_r * 0.5f)
                f->ay = (float)h - L.stick_r * 0.5f;
        } else if (y >= L.zone_y && x >= L.split_x && !holder(W_CAMERA)) {
            f->what = W_CAMERA;
            f->click = double_tap(1, x, y, now);
        } else
            f->what = W_NONE;
    } else if (!f) {
        return;
    } else if (kind == TOUCH_MOVE) {
        float dx = x - f->x0, dy = y - f->y0, d = sqrtf(dx * dx + dy * dy);
        f->x = x;
        f->y = y;
        if (d > f->moved)
            f->moved = d;
        if (f->what == W_BUTTON) {   /* sliding across the buttons: the one under the finger */
            int b = f->btn >= 0 && btn_dist(f->btn, x, y) < 1.4f ? f->btn : hit_button(x, y);
            if (b != f->btn) {
                let_go(f->btn, f->btn_t0);
                f->btn = b;
                f->btn_t0 = now;
            }
        } else if (f->what == W_CAMERA) {   /* past the edge the centre follows */
            float cx = x - f->ax, cy = y - f->ay, m = sqrtf(cx * cx + cy * cy);
            if (m > L.cam_r) {
                f->ax = x - cx / m * L.cam_r;
                f->ay = y - cy / m * L.cam_r;
            }
        }
    } else {   /* up or cancelled */
        if (f->what == W_BUTTON)
            let_go(f->btn, f->btn_t0);
        else if (f->what == W_STICK || f->what == W_CAMERA) {
            int side = f->what == W_CAMERA;
            if (f->click)   /* (a quick second tap still clicks) */
                let_go(side ? PB_R3 : PB_L3, f->t0);
            else if (now - f->t0 < TAP_MS && f->moved < 4 * L.u) {
                g_tap_t[side] = now ? now : 1;
                g_tap_x[side] = f->x0;
                g_tap_y[side] = f->y0;
            }
        }
        f->used = 0;
    }
    publish();
}

void touch_merge(pad_t *p, int *connected)
{
    pad_t t;
    uint32_t now, until[PB_COUNT];
    int b, lt, rt;
    g_physical = *connected;
    if (!g_cs_ready || !touch_visible())
        return;
    EnterCriticalSection(&g_lock);
    t = g_pub;
    lt = g_pub_lt;
    rt = g_pub_rt;
    memcpy(until, g_until, sizeof until);
    LeaveCriticalSection(&g_lock);
    now = plat_ticks_ms();
    for (b = 0; b < PB_COUNT; b++)
        if (until[b] && (int32_t)(until[b] - now) > 0) {
            t.wButtons |= BIT[b];
            lt |= b == PB_LT;
            rt |= b == PB_RT;
        }
    p->wButtons |= t.wButtons;
    if (t.sThumbLX || t.sThumbLY) {
        p->sThumbLX = t.sThumbLX;
        p->sThumbLY = t.sThumbLY;
    }
    if (t.sThumbRX || t.sThumbRY) {
        p->sThumbRX = t.sThumbRX;
        p->sThumbRY = t.sThumbRY;
    }
    if (lt)
        p->bLeftTrigger = 255;
    if (rt)
        p->bRightTrigger = 255;
    *connected = 1;
}

/* ---- drawing ---- */

#define A_IDLE g_opacity
#define A_DOWN (g_opacity + 0.45f > 1.0f ? 1.0f : g_opacity + 0.45f)

static int shown_down(int b)
{
    int i;
    for (i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].what == W_BUTTON && g_f[i].btn == b)
            return 1;
    return 0;
}

static void label(float cx, float cy, float maxw, float maxh, const char *s)
{
    float lw = gfx_ui_font_width(UI_FONT_DISPLAY, s, 1.0f), lh = gfx_ui_font_line(UI_FONT_DISPLAY, 1.0f), sc;
    if (lw <= 0 || lh <= 0)
        return;
    sc = maxh / lh;
    if (lw * sc > maxw)
        sc = maxw / lw;
    gfx_ui_font_text(UI_FONT_DISPLAY, cx - lw * sc / 2, cy - lh * sc / 2, s, ui_col(0xFFFFFFFF), ui_col(0xFFCFE6FF),
                     sc);
}

static void draw_rect_button(int b)
{
    float x = L.bx[b], y = L.by[b], w = L.bw[b], h = L.bh[b], cut = h * 0.3f;
    int down = shown_down(b);
    ui_alpha = down ? A_DOWN : A_IDLE;
    ui_panel(x, y, w, h, cut, down ? COL_BLUE : COL_PANEL_TOP, down ? COL_BLUE_DARK : COL_PANEL_BOT);
    ui_panel_edge(x, y, w, h, cut, L.u * 0.35f, down ? COL_YELLOW : COL_EDGE);
    /* (a DualSense's and a Switch controller's Back and Start are icons) */
    if ((b == PB_BACK || b == PB_START) && ui_pad_icon(x + w / 2, y + h / 2, h * 0.38f, b))
        return;
    label(x + w / 2, y + h / 2, w * 0.8f, h * 0.62f, pad_cap(b));
}

static void draw_stick(float cx, float cy, float r, float kx, float ky, float kr, int active, const char *name)
{
    ui_alpha = active ? A_DOWN : A_IDLE;
    gfx_ui_circle(cx, cy, r, ui_col(0x60081322));
    gfx_ui_ring(cx, cy, r, L.u * 0.45f, ui_col(active ? COL_YELLOW : COL_EDGE));
    gfx_ui_circle(kx, ky, kr, ui_col(active ? COL_BLUE : 0xFF2B3A50));
    gfx_ui_ring(kx, ky, kr, L.u * 0.35f, ui_col(0x90FFFFFF));
    if (name && !active)
        label(kx, ky, kr * 1.5f, kr * 0.55f, name);
}

static void draw(void *user, int w, int h)
{
    float save = ui_alpha;
    const finger *stick = NULL, *cam = NULL;
    int i;
    (void)user;
    if (!touch_visible()) {
        if (g_was_visible)
            release_all();
        g_was_visible = 0;
        return;
    }
    layout(w, h);
    for (i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].what == W_STICK)
            stick = &g_f[i];
        else if (g_f[i].used && g_f[i].what == W_CAMERA)
            cam = &g_f[i];
    /* the sticks */
    if (stick) {
        float dx = stick->x - stick->ax, dy = stick->y - stick->ay, m = sqrtf(dx * dx + dy * dy);
        if (m > L.stick_r) {
            dx *= L.stick_r / m;
            dy *= L.stick_r / m;
        }
        draw_stick(stick->ax, stick->ay, L.stick_r, stick->ax + dx, stick->ay + dy, L.knob_r, 1, NULL);
    } else
        draw_stick(L.stick_x, L.stick_y, L.stick_r, L.stick_x, L.stick_y, L.knob_r, 0, "MOVE");
    if (cam) {
        float dx = cam->x - cam->ax, dy = cam->y - cam->ay;
        draw_stick(cam->ax, cam->ay, L.cam_r, cam->ax + dx, cam->ay + dy, L.knob_r * 0.85f, 1, NULL);
    } else {
        float save2;
        draw_stick(L.cam_x, L.cam_y, L.cam_r, L.cam_x, L.cam_y, L.knob_r * 0.85f, 0, "CAM");
        save2 = ui_alpha;
        ui_alpha = A_IDLE * 0.7f;
        for (i = 0; i < 4; i++) {   /* arrows around it: drag anywhere on this side */
            static const float DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
            float d = L.cam_r + 2.4f * L.u, s = 1.6f * L.u, ax = L.cam_x + DX[i] * d, ay = L.cam_y + DY[i] * d;
            gfx_ui_tri(ax + DX[i] * s * 0.6f, ay + DY[i] * s * 0.6f, ax - DX[i] * s * 0.4f - DY[i] * s * 0.7f,
                       ay - DY[i] * s * 0.4f - DX[i] * s * 0.7f, ax - DX[i] * s * 0.4f + DY[i] * s * 0.7f,
                       ay - DY[i] * s * 0.4f + DX[i] * s * 0.7f, ui_col(COL_EDGE));
        }
        ui_alpha = save2;
    }
    /* the buttons */
    for (i = PB_A; i <= PB_Y; i++) {
        float r = L.bw[i];
        int down = shown_down(i);
        ui_alpha = down ? A_DOWN : A_IDLE;
        ui_pad_face_ex(L.bx[i] - r, L.by[i], r, i, r * 1.05f / gfx_ui_font_line(UI_FONT_BODY, 1.0f));
        if (down)
            gfx_ui_ring(L.bx[i], L.by[i], r, L.u * 0.5f, ui_col(COL_YELLOW));
    }
    for (i = 0; i < NDRAWN; i++)
        if (!is_face(DRAWN[i]))
            draw_rect_button(DRAWN[i]);
    ui_alpha = save;
}

void touch_load(const char *ini)
{
    char v[16];
    int m;
    if (!ini || !ini[0])
        return;
    GetPrivateProfileStringA("controls", "touch_controls", "auto", v, sizeof v, ini);
    m = _stricmp(v, "on") == 0 || strcmp(v, "1") == 0 ? MODE_ON : _stricmp(v, "off") == 0 || strcmp(v, "0") == 0
        ? MODE_OFF : MODE_AUTO;
    {   /* (percent: 10 to 100) */
        int op = GetPrivateProfileIntA("controls", "touch_opacity", 40, ini);
        g_opacity = (float)(op < 10 ? 10 : op > 100 ? 100 : op) / 100.0f;
    }
    GetPrivateProfileStringA("video", "aspect", "fill", v, sizeof v, ini);
    g_aspect = strcmp(v, "16:9") == 0 ? 16.0f / 9.0f : strcmp(v, "4:3") == 0 ? 4.0f / 3.0f : 0;
    L.w = L.h = 0;   /* (laid out again: the aspect may have changed) */
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_lock);
        g_cs_ready = 1;
    }
    if (m != g_mode || !g_installed)
        log_printf("touch: touch controls %s", m == MODE_ON ? "on" : m == MODE_OFF ? "off" : "auto (shown while no "
                   "controller is connected)");
    g_mode = m;
    if (!g_installed) {
        g_installed = 1;
        gfx_ui_set_top_draw(draw, NULL);
    }
}
#else
void touch_load(const char *ini)
{
    (void)ini;
}

void touch_finger(int kind, uint64_t id, float x, float y)
{
    (void)kind; (void)id; (void)x; (void)y;
}

void touch_release_all(void)
{
}

void touch_merge(pad_t *p, int *connected)
{
    (void)p;
    g_physical = *connected;
}
#endif
