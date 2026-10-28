/* In-game options menu (F1, or Back + Start on a controller), and the pad
 * chords (Back + Start, Back + RB for the quick save).
 *
 * Drawn by our renderer over the final picture (gfx_ui), so the game keeps
 * running underneath and every change shows at once: good for comparing
 * settings side by side. While it's open the game gets no input (pad, game
 * keys, mouse look). Each change is applied live and written to mml.ini right
 * away; the launcher reads those keys back, so both stay in step.
 *
 * Three pages (LB/RB or Q/E), one setting per row, with a line of help for the
 * selected one. The look borrows from the game's own HUD: Mega Man blue
 * panels with cut corners, yellow accents and green energy-cell gauges.
 *
 * Runs on the game thread: menu_frame() once per game frame (input),
 * menu_filter_buttons() from the pad poll, and the draw callback while each
 * presented frame is composed. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controls.h"
#include "ddshim.h"
#include "fog.h"
#include "frame_rec.h"
#include "gfx.h"
#include "gfx_ui.h"
#include "input.h"
#include "kbm.h"
#include "lockon.h"
#include "lod.h"
#include "log.h"
#include "menu.h"
#include "texreplace.h"
#include "ui_style.h"

#define MAX_PRESETS 4096

enum {
    IT_FPS, IT_VSYNC, IT_RES, IT_MSAA, IT_TEXFILTER, IT_UPSCALE,
    IT_SHADER, IT_SHADER_RES, IT_FOG, IT_LOD, IT_HDTEX,
    IT_CAMSPEED, IT_INVERT_X, IT_INVERT_Y, IT_MOUSE, IT_RETICLE,
};

typedef struct {
    int page, kind;
    const char *label, *help;
} item;

static const char *const PAGES[] = { "VIDEO", "PICTURE", "CONTROLS" };
#define NPAGES 3

static const item g_items[] = {
    { 0, IT_FPS, "Frame rate", "60 fps adds in-between frames for smooth motion. The game itself still runs at 30." },
    { 0, IT_VSYNC, "Vsync", "Locks frames to your monitor's refresh so the picture never tears. Adds a little lag." },
    { 0, IT_RES, "Resolution", "Display renders at your monitor's resolution. 1x to 4x are multiples of the game's 480 lines." },
    { 0, IT_MSAA, "Antialiasing", "Smooths jagged polygon edges. ReShade depth effects stop working while it's on." },
    { 0, IT_TEXFILTER, "Texture filter", "Anisotropic keeps floors and walls sharp when you look along them." },
    { 0, IT_UPSCALE, "Upscaling", "How a lower resolution is stretched to the screen. Nearest keeps the pixels crisp." },
    { 1, IT_SHADER, "Shader", "RetroArch shader presets (CRT looks and more) from the Modernization\\shaders folder." },
    { 1, IT_SHADER_RES, "Shader input", "What the shader works on. CRT presets look right at the game's own resolution." },
    { 1, IT_FOG, "Distance fog", "The original fog, a lighter one that lets you see further, or none at all." },
    { 1, IT_LOD, "Model detail", "Full detail keeps faraway models at their best instead of swapping in low-poly ones." },
    { 1, IT_HDTEX, "HD textures", "Replacement textures from the Modernization\\textures folder." },
    { 2, IT_CAMSPEED, "Camera speed", "How fast the right stick turns the camera." },
    { 2, IT_INVERT_X, "Invert camera X", "Flips left and right on the camera stick." },
    { 2, IT_INVERT_Y, "Invert camera Y", "Flips up and down on the camera stick and the mouse." },
    { 2, IT_MOUSE, "Mouse & keyboard", "Mouse look speed. Off turns the mouse and keyboard controls off." },
    { 2, IT_RETICLE, "Lock-on reticle", "Colour of the marker on the enemy you're locked on to." },
};
#define NITEMS ((int)(sizeof g_items / sizeof g_items[0]))

static const int SPEEDS[] = { 50, 75, 100, 125, 150, 200 };
static const int MOUSE[] = { 0, 50, 75, 100, 150, 200 };
static const struct { const char *name; unsigned rgb; } RETICLES[] = {
    { "Yellow", 0xFFD840 }, { "Orange", 0xFF9A2E }, { "Red", 0xFF4040 }, { "Pink", 0xFF6EC7 },
    { "Purple", 0xB47CFF }, { "Blue", 0x4AA3FF }, { "Cyan", 0x3EE0E0 }, { "Green", 0x5BE36B },
    { "White", 0xFFFFFF },
};

static struct {
    int ready, open, page, cursor, block;   /* block: input held back until everything is released */
    int pad_ui;                             /* button prompts for the pad (else the keyboard) */
    char ini[MAX_PATH], game_dir[MAX_PATH];
    /* values */
    int fps60, vsync, res, msaa, shader_game_res, nearest, aniso, fog, lod_full, hdtex;
    int cam_speed, inv_x, inv_y, mouse, reticle;
    int shader;                         /* index into presets, -1 = off */
    char shader_custom[MAX_PATH];       /* a preset from the ini outside the shaders folder */
    char **presets;
    int npresets;
    char note[160];
    double note_until;
    /* input */
    int prev_keys;
    int held_dir;
    double held_since, last_repeat;
    /* animation (draw side) */
    double last_draw, page_since, kick_at;
    float shown, sel_y;
    int kick_dir, sel_page;
    LARGE_INTEGER qpf;
} M;

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)M.qpf.QuadPart;
}

static void note(const char *msg)
{
    strcpy_s(M.note, sizeof M.note, msg);
    M.note_until = now_s() + 4.0;
}

static void ini_int(const char *sec, const char *key, int v)
{
    char b[16];
    _snprintf_s(b, sizeof b, _TRUNCATE, "%d", v);
    WritePrivateProfileStringA(sec, key, b, M.ini);
}

static void ini_str(const char *sec, const char *key, const char *v)
{
    WritePrivateProfileStringA(sec, key, v, M.ini);
}

/* ---- shader presets under Modernization\shaders ---- */

static int by_name(const void *a, const void *b)
{
    return _stricmp(*(char *const *)a, *(char *const *)b);
}

static void scan_presets(const char *dir, int depth)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", dir);
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        char path[MAX_PATH];
        size_t n = strlen(fd.cFileName);
        if (fd.cFileName[0] == '.')
            continue;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* slang-shaders keeps its internal passes (and helper presets that need
               shaders we may not have) in folders called "shaders"; only list the
               user-facing presets */
            if (depth < 6 && _stricmp(fd.cFileName, "shaders") != 0)
                scan_presets(path, depth + 1);
        } else if (n > 7 && _stricmp(fd.cFileName + n - 7, ".slangp") == 0 && M.npresets < MAX_PRESETS) {
            M.presets[M.npresets++] = _strdup(path);
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static void full_path(const char *in, char *out, size_t n)
{
    if (!in[0])
        out[0] = 0;
    else if (in[1] == ':' || (in[0] == '\\' && in[1] == '\\'))
        strcpy_s(out, n, in);
    else
        _snprintf_s(out, n, _TRUNCATE, "%s\\%s", M.game_dir, in);
}

static const char *shader_path(void)
{
    if (M.shader >= 0 && M.shader < M.npresets)
        return M.presets[M.shader];
    return M.shader_custom;
}

/* ---- applying ---- */

static int effective_scale(void)
{
    /* CRT presets want the game's own resolution (the launcher's rule too) */
    if (shader_path()[0] && M.res == 0)
        return M.shader_game_res ? 1 : 0;
    return M.res;
}

static void apply(int kind)
{
    switch (kind) {
    case IT_FPS:
        rec_enable(M.fps60);
        ini_int("video", "interp", M.fps60);
        break;
    case IT_VSYNC:
        gfx_set_vsync(M.vsync);
        ini_int("video", "vsync", M.vsync);
        break;
    case IT_RES:
    case IT_MSAA:
    case IT_SHADER_RES:
        gfx_set_scene(effective_scale(), M.msaa);
        ini_int("video", "render_scale", M.res);
        ini_int("video", "msaa", M.msaa);
        ini_int("video", "shader_scale", M.shader_game_res);
        if (M.msaa > 1)
            note("Antialiasing hides depth from ReShade effects");
        break;
    case IT_SHADER: {
        const char *p = shader_path();
        char rel[MAX_PATH];
        size_t gl = strlen(M.game_dir);
        if (!gfx_set_shader(p) && p[0])
            note("That preset failed to load (see mml.log)");
        gfx_set_scene(effective_scale(), M.msaa);
        if (p[0] && _strnicmp(p, M.game_dir, gl) == 0 && p[gl] == '\\')
            strcpy_s(rel, sizeof rel, p + gl + 1);   /* inside the game folder: relative, like the launcher */
        else
            strcpy_s(rel, sizeof rel, p);
        ini_str("video", "shader", rel);
        break;
    }
    case IT_UPSCALE:
        gfx_set_nearest_upscale(M.nearest);
        ini_int("video", "nearest_upscale", M.nearest);
        break;
    case IT_TEXFILTER:
        gfx_set_anisotropy(M.aniso ? 16 : 0);
        ini_int("video", "anisotropy", M.aniso ? 16 : 0);
        break;
    case IT_FOG:
        fog_set_mode(M.fog);
        ini_str("video", "fog", M.fog == FOG_OFF ? "off" : M.fog == FOG_LIGHT ? "light" : "original");
        break;
    case IT_LOD:
        lod_set(M.lod_full, !GetPrivateProfileIntA("debug", "near_subdiv", 0, M.ini));
        ini_str("video", "lod", M.lod_full ? "full" : "original");
        break;
    case IT_HDTEX:
        texreplace_set_replace(M.hdtex);
        ddshim_reupload_textures();
        ini_int("textures", "replace", M.hdtex);
        break;
    case IT_CAMSPEED:
    case IT_INVERT_X:
    case IT_INVERT_Y:
        controls_camera(SPEEDS[M.cam_speed], SPEEDS[M.cam_speed], M.inv_x, M.inv_y, 0, 0);
        ini_int("controls", "camera_speed_x", SPEEDS[M.cam_speed]);
        ini_int("controls", "camera_speed_y", SPEEDS[M.cam_speed]);
        ini_int("controls", "invert_x", M.inv_x);
        ini_int("controls", "invert_y", M.inv_y);
        if (kind != IT_INVERT_Y)
            break;
        /* like the launcher, invert Y covers the mouse too */
        ini_int("controls", "mouse_invert_y", M.inv_y);
        /* fall through */
    case IT_MOUSE:
        controls_mouse(MOUSE[M.mouse] > 0, MOUSE[M.mouse] > 0 ? MOUSE[M.mouse] : 100, M.inv_y);
        ini_int("controls", "mouse_keyboard", MOUSE[M.mouse] > 0);
        if (MOUSE[M.mouse] > 0)
            ini_int("controls", "mouse_speed", MOUSE[M.mouse]);
        break;
    case IT_RETICLE: {
        char hex[8];
        lockon_set_color(RETICLES[M.reticle].rgb);
        _snprintf_s(hex, sizeof hex, _TRUNCATE, "%06X", RETICLES[M.reticle].rgb);
        ini_str("controls", "reticle_color", hex);
        break;
    }
    default:
        break;
    }
}

static int cycle(int v, int n, int dir)
{
    return (v + dir + n) % n;
}

static int nearest_index(const int *list, int n, int v)
{
    int i, best = 0;
    for (i = 0; i < n; i++)
        if (abs(list[i] - v) < abs(list[best] - v))
            best = i;
    return best;
}

static void change(int kind, int dir)
{
    switch (kind) {
    case IT_FPS: M.fps60 = !M.fps60; break;
    case IT_VSYNC: M.vsync = !M.vsync; break;
    case IT_RES: M.res = cycle(M.res, 5, dir); break;
    case IT_MSAA: M.msaa = M.msaa == 1 ? (dir > 0 ? 4 : 8) : M.msaa == 4 ? (dir > 0 ? 8 : 1) : (dir > 0 ? 1 : 4); break;
    case IT_SHADER:
        /* -1 = off, then every preset found */
        M.shader = M.npresets ? cycle(M.shader + 1, M.npresets + 1, dir) - 1 : -1;
        M.shader_custom[0] = 0;
        break;
    case IT_SHADER_RES: M.shader_game_res = !M.shader_game_res; break;
    case IT_UPSCALE: M.nearest = !M.nearest; break;
    case IT_TEXFILTER: M.aniso = !M.aniso; break;
    case IT_FOG: M.fog = cycle(M.fog, 3, dir); break;
    case IT_LOD: M.lod_full = !M.lod_full; break;
    case IT_HDTEX: M.hdtex = !M.hdtex; break;
    case IT_CAMSPEED: M.cam_speed = cycle(M.cam_speed, 6, dir); break;
    case IT_INVERT_X: M.inv_x = !M.inv_x; break;
    case IT_INVERT_Y: M.inv_y = !M.inv_y; break;
    case IT_MOUSE: M.mouse = cycle(M.mouse, 6, dir); break;
    case IT_RETICLE: M.reticle = cycle(M.reticle, (int)(sizeof RETICLES / sizeof RETICLES[0]), dir); break;
    default: return;
    }
    M.kick_dir = dir;
    M.kick_at = now_s();
    apply(kind);
}

static void value_text(int kind, char *out, size_t n)
{
    static const char *RES[] = { "Display", "1x", "2x", "3x", "4x" };
    static const char *FOG[] = { "Original", "Light", "Off" };
    switch (kind) {
    case IT_FPS: strcpy_s(out, n, M.fps60 ? "60 fps" : "30 fps"); break;
    case IT_VSYNC: strcpy_s(out, n, M.vsync ? "On" : "Off"); break;
    case IT_RES: strcpy_s(out, n, RES[M.res]); break;
    case IT_MSAA: strcpy_s(out, n, M.msaa > 1 ? (M.msaa == 4 ? "4x" : "8x") : "Off"); break;
    case IT_SHADER: {
        const char *p = shader_path(), *b = strrchr(p, '\\');
        if (!p[0]) {
            strcpy_s(out, n, "Off");
            break;
        }
        strcpy_s(out, n, b ? b + 1 : p);
        if (strlen(out) > 7)
            out[strlen(out) - 7] = 0;   /* drop .slangp */
        break;
    }
    case IT_SHADER_RES: strcpy_s(out, n, M.shader_game_res ? "Game resolution" : "Display"); break;
    case IT_UPSCALE: strcpy_s(out, n, M.nearest ? "Nearest" : "Bilinear"); break;
    case IT_TEXFILTER: strcpy_s(out, n, M.aniso ? "Anisotropic 16x" : "Smooth"); break;
    case IT_FOG: strcpy_s(out, n, FOG[M.fog]); break;
    case IT_LOD: strcpy_s(out, n, M.lod_full ? "Full detail" : "Original"); break;
    case IT_HDTEX: strcpy_s(out, n, M.hdtex ? "On" : "Off"); break;
    case IT_CAMSPEED: _snprintf_s(out, n, _TRUNCATE, "%d%%", SPEEDS[M.cam_speed]); break;
    case IT_INVERT_X: strcpy_s(out, n, M.inv_x ? "On" : "Off"); break;
    case IT_INVERT_Y: strcpy_s(out, n, M.inv_y ? "On" : "Off"); break;
    case IT_MOUSE:
        if (MOUSE[M.mouse])
            _snprintf_s(out, n, _TRUNCATE, "%d%%", MOUSE[M.mouse]);
        else
            strcpy_s(out, n, "Off");
        break;
    case IT_RETICLE: strcpy_s(out, n, RETICLES[M.reticle].name); break;
    default: out[0] = 0; break;
    }
}

/* on/off rows get a status light; stepped ones an energy gauge */
static int toggle_state(int kind)
{
    switch (kind) {
    case IT_VSYNC: return M.vsync;
    case IT_HDTEX: return M.hdtex;
    case IT_INVERT_X: return M.inv_x;
    case IT_INVERT_Y: return M.inv_y;
    default: return -1;
    }
}

static int gauge(int kind, int *total)
{
    switch (kind) {
    case IT_CAMSPEED: *total = 6; return M.cam_speed + 1;
    case IT_MOUSE: *total = 5; return M.mouse;
    case IT_MSAA: *total = 2; return M.msaa == 1 ? 0 : M.msaa == 4 ? 1 : 2;
    default: return -1;
    }
}

/* ---- drawing ---- */

/* a pad button: round face with its letter */
static float pad_button(float x, float cy, float r, const char *label, uint32_t face)
{
    float tw = gfx_ui_font_width(UI_FONT_BODY, label, 0.62f);
    gfx_ui_circle(x + r, cy, r, ui_col(0xFF000000 | (face & 0xFFFFFF) ));
    gfx_ui_ring(x + r, cy, r, r * 0.16f, ui_col(0x60FFFFFF));
    ui_text(x + r - tw / 2, cy - gfx_ui_font_line(UI_FONT_BODY, 0.62f) / 2, label, 0xFFFFFFFF, 0.62f);
    return r * 2;
}

static float dpad_glyph(float x, float cy, float s)
{
    float t = s * 0.36f;
    gfx_ui_rect(x + (s - t) / 2, cy - s / 2, t, s, ui_col(0xFF2B3A50));
    gfx_ui_rect(x, cy - t / 2, s, t, ui_col(0xFF2B3A50));
    gfx_ui_rect(x + (s - t) / 2 + t * 0.25f, cy - s / 2 + t * 0.15f, t * 0.5f, t * 0.6f, ui_col(0xFF9FB3C8));
    gfx_ui_rect(x + (s - t) / 2 + t * 0.25f, cy + s / 2 - t * 0.75f, t * 0.5f, t * 0.6f, ui_col(0xFF9FB3C8));
    return s;
}

/* word-wrapped help text; returns the height used */
static float wrapped(float x, float y, float maxw, const char *s, uint32_t col, float scale)
{
    char buf[256];
    float lh = gfx_ui_font_line(UI_FONT_BODY, scale) * 1.05f, yy = y;
    while (*s) {
        int n = 0, last_space = -1;
        while (s[n]) {
            buf[n] = s[n];
            buf[n + 1] = 0;
            if (s[n] == ' ')
                last_space = n;
            if (gfx_ui_font_width(UI_FONT_BODY, buf, scale) > maxw && last_space > 0) {
                n = last_space;
                break;
            }
            if (++n >= (int)sizeof buf - 2)
                break;
        }
        buf[n] = 0;
        ui_text(x, yy, buf, col, scale);
        yy += lh;
        s += n;
        while (*s == ' ')
            s++;
    }
    return yy - y;
}

static int page_items(int page, int *first)
{
    int i, n = 0;
    *first = -1;
    for (i = 0; i < NITEMS; i++)
        if (g_items[i].page == page) {
            if (*first < 0)
                *first = i;
            n++;
        }
    return n;
}

static void draw_value(const item *it, float vx, float cy, float u, int selected, double t)
{
    char v[MAX_PATH];
    float sc = 0.86f, th = gfx_ui_font_line(UI_FONT_BODY, sc), x = vx, kick = 0, aw = u * 0.42f;
    int on = toggle_state(it->kind), total = 0, filled = gauge(it->kind, &total);
    value_text(it->kind, v, sizeof v);
    if (selected) {
        double since = t - M.kick_at;
        if (since < 0.18)
            kick = (float)(1 - since / 0.18) * u * 0.22f;
        ui_arrow(x - aw / 2 + (M.kick_dir > 0 ? kick : 0), cy, aw, 1, COL_YELLOW);
        x -= aw + u * 0.35f;
    }
    if (filled >= 0) {   /* energy cells, filled from the left */
        float cw = u * 0.36f, ch = u * 0.62f, gap = u * 0.1f, gx;
        int i;
        x -= total * (cw + gap) - gap;
        gx = x;
        for (i = 0; i < total; i++) {
            float sx = gx + i * (cw + gap);
            if (i < filled) {
                ui_slant(sx, cy - ch / 2, cw, ch, ch * 0.25f, COL_GREEN_HI, COL_GREEN_LO);
            } else {
                ui_slant(sx, cy - ch / 2, cw, ch, ch * 0.25f, 0xFF2A3E5A, 0xFF1C2C44);
            }
        }
        x -= u * 0.4f;
    }
    if (on >= 0) {   /* status light */
        float r = u * 0.2f;
        x -= r * 2;
        if (on) {
            gfx_ui_circle(x + r, cy, r * 2.0f, ui_col(0x3062F06E));
            gfx_ui_circle(x + r, cy, r, ui_col(0xFF62F06E));
            gfx_ui_circle(x + r * 0.75f, cy - r * 0.3f, r * 0.35f, ui_col(0xC0FFFFFF));
        } else {
            gfx_ui_circle(x + r, cy, r, ui_col(0xFF3A4656));
        }
        x -= u * 0.3f;
    }
    if (it->kind == IT_RETICLE) {   /* a little lock-on marker in that colour */
        float r = u * 0.3f;
        uint32_t c = 0xFF000000 | RETICLES[M.reticle].rgb;
        x -= r * 2;
        gfx_ui_ring(x + r, cy, r, r * 0.25f, ui_col(c));
        ui_line(x + r, cy - r * 1.5f, x + r, cy - r * 0.6f, r * 0.25f, c);
        ui_line(x + r, cy + r * 0.6f, x + r, cy + r * 1.5f, r * 0.25f, c);
        ui_line(x - r * 0.5f, cy, x + r * 0.4f, cy, r * 0.25f, c);
        ui_line(x + r * 1.6f, cy, x + r * 2.5f, cy, r * 0.25f, c);
        x -= u * 0.45f;
    }
    if (it->kind == IT_SHADER && M.shader >= 0) {   /* preset number, small */
        char cnt[32];
        _snprintf_s(cnt, sizeof cnt, _TRUNCATE, "%d/%d", M.shader + 1, M.npresets);
        x -= gfx_ui_font_width(UI_FONT_BODY, cnt, 0.6f);
        ui_text(x, cy - gfx_ui_font_line(UI_FONT_BODY, 0.6f) / 2 + u * 0.04f, cnt, COL_DIM, 0.6f);
        x -= u * 0.3f;
    }
    {
        /* long preset names: trim from the end so they fit */
        float maxw = u * 9;
        size_t n = strlen(v);
        while (n > 4 && gfx_ui_font_width(UI_FONT_BODY, v, sc) > maxw) {
            v[--n] = 0;
            v[n - 1] = v[n - 2] = '.';
        }
    }
    x -= gfx_ui_font_width(UI_FONT_BODY, v, sc);
    ui_text(x, cy - th / 2, v, selected ? 0xFFFFFFFF : (on == 0 ? COL_DIM : COL_TEXT), sc);
    if (selected)
        ui_arrow(x - u * 0.35f - aw / 2 - (M.kick_dir < 0 ? kick : 0), cy, aw, -1, COL_YELLOW);
}

static void draw(void *user, int w, int h)
{
    float u, pad, pw, ph, x, y, cut, rowh, head_h, tab_h, help_h, foot_h, rows_y, slide, dt;
    int first, n, i, maxrows = 0;
    double t = now_s();
    (void)user;
    dt = M.last_draw > 0 ? (float)(t - M.last_draw) : 0;
    if (dt > 0.1f)
        dt = 0.1f;
    M.last_draw = t;
    /* open/close animation */
    if (M.open)
        M.shown = M.shown + dt / 0.18f > 1 ? 1 : M.shown + dt / 0.18f;
    else
        M.shown = M.shown - dt / 0.12f < 0 ? 0 : M.shown - dt / 0.12f;
    if (M.shown <= 0)
        return;
    ui_alpha = ui_ease_out(M.shown);

    u = gfx_ui_line(1.0f);
    pad = u * 0.75f;
    cut = u * 0.9f;
    rowh = u * 1.3f;
    head_h = u * 2.3f;
    tab_h = u * 1.25f;
    help_h = u * 2.5f;
    foot_h = u * 1.4f;
    for (i = 0; i < NPAGES; i++) {
        n = page_items(i, &first);
        if (n > maxrows)
            maxrows = n;
    }
    pw = u * 19;
    if (pw > w - 2 * u)
        pw = w - 2 * u;
    ph = head_h + tab_h + u * 0.5f + maxrows * rowh + u * 0.4f + help_h + foot_h + pad;
    slide = (1 - ui_alpha) * u * 4;
    x = u * 1.2f - slide;
    y = (h - ph) / 2;
    if (y < u * 0.5f)
        y = u * 0.5f;

    /* dim the game behind the panel so the text reads, but keep the right side clear for comparing */
    gfx_ui_hgrad(0, 0, w * 0.62f, (float)h, ui_col(0x99030812), ui_col(0x00030812));

    /* body */
    ui_panel(x + u * 0.25f, y + u * 0.3f, pw, ph, cut, 0x60000000, 0x60000000);   /* drop shadow */
    ui_panel(x, y, pw, ph, cut, COL_PANEL_TOP, COL_PANEL_BOT);
    for (i = 0; i < (int)(ph / 3); i++)   /* faint scanlines */
        gfx_ui_rect(x + (i * 3 > ph - cut ? cut * (1 - (ph - i * 3) / cut) : 0), y + i * 3,
                    pw - (i * 3 < cut ? cut - i * 3 : 0) - (i * 3 > ph - cut ? cut * (1 - (ph - i * 3) / cut) : 0),
                    1, ui_col(0x0C000000));
    ui_panel_edge(x, y, pw, ph, cut, u * 0.08f, COL_EDGE);
    ui_panel_edge(x + u * 0.18f, y + u * 0.18f, pw - u * 0.36f, ph - u * 0.36f, cut * 0.85f, 1.0f, 0x405CC8FF);
    {   /* yellow brackets on the two square corners */
        float b = u * 0.7f, th = u * 0.12f, o = u * 0.02f;
        ui_line(x - o, y + b, x - o, y - o - th / 2, th, COL_YELLOW);
        ui_line(x - o - th / 2, y - o, x + b, y - o, th, COL_YELLOW);
        ui_line(x + pw + o, y + ph - b, x + pw + o, y + ph + o + th / 2, th, COL_YELLOW);
        ui_line(x + pw + o + th / 2, y + ph + o, x + pw - b, y + ph + o, th, COL_YELLOW);
    }

    /* header: a Mega Man blue band with the title, and a strip of hazard stripes */
    {
        float bx = x + u * 0.18f, by = y + u * 0.18f, bw = pw * 0.66f, bh = head_h - u * 0.55f;
        float ts = 0.62f, tx = x + pad, ty;
        float xy[8], sweep;
        uint32_t c[4];
        /* square on the left (inside the frame), cut on the right */
        xy[0] = bx; xy[1] = by;
        xy[2] = bx + bw; xy[3] = by;
        xy[4] = bx + bw - bh * 0.5f; xy[5] = by + bh;
        xy[6] = bx; xy[7] = by + bh;
        c[0] = ui_col(COL_BLUE); c[1] = ui_col(COL_BLUE_DARK); c[2] = ui_col(COL_BLUE_DARK); c[3] = ui_col(0xFF1854B8);
        gfx_ui_quad(xy, c);
        /* a glint that runs across the band every few seconds */
        sweep = (float)fmod(t, 3.5) / 0.6f;
        if (sweep < 1) {
            float gx = bx + sweep * (bw + bh), gw = bh * 0.5f;
            if (gx + gw < bx + bw - bh * 0.5f)
                ui_slant(gx, by, gw, bh, bh * 0.5f, 0x00FFFFFF, 0x50FFFFFF);
        }
        gfx_ui_rect(bx, by + bh - u * 0.08f, bw - bh * 0.5f, u * 0.08f, ui_col(COL_YELLOW));
        ty = by + (bh - gfx_ui_font_line(UI_FONT_DISPLAY, ts)) / 2;
        ui_text_outlined(UI_FONT_DISPLAY, tx, ty, "SETTINGS", 0xFFFFFFFF, 0xFF9EE2FF, ts, u * 0.06f, 0xFF061226);
        /* hazard stripes */
        {
            float sx0 = bx + bw + u * 0.2f, sx1 = x + pw - cut - u * 0.1f, sh = u * 0.32f, sy = by + bh - sh;
            float step = sh * 1.6f, sxx;
            for (sxx = sx0; sxx + step <= sx1; sxx += step)
                ui_slant(sxx, sy, step * 0.5f, sh, sh, COL_YELLOW, COL_YELLOW_LO);
            ui_text(sx0, by + u * 0.05f, "changes apply live", COL_DIM, 0.58f);
        }
    }

    /* page tabs, LB / RB on either side */
    {
        float ty = y + head_h, cy = ty + tab_h / 2, capw, slant = tab_h * 0.35f, tx, tw;
        float kh = u * 0.62f;
        capw = M.pad_ui ? ui_key_cap(x + pad, cy, kh, "LB") : ui_key_cap(x + pad, cy, kh, "Q");
        if (M.pad_ui)
            ui_key_cap(x + pw - pad - capw, cy, kh, "RB");
        else
            ui_key_cap(x + pw - pad - capw, cy, kh, "E");
        tx = x + pad + capw + u * 0.35f;
        tw = (pw - 2 * pad - 2 * capw - u * 0.7f - slant) / NPAGES;
        for (i = 0; i < NPAGES; i++) {
            float sx = tx + i * tw, lw = gfx_ui_font_width(UI_FONT_DISPLAY, PAGES[i], 0.42f);
            float lh = gfx_ui_font_line(UI_FONT_DISPLAY, 0.42f);
            if (i == M.page) {
                ui_slant(sx, ty + u * 0.12f, tw - u * 0.12f, tab_h - u * 0.24f, slant, COL_YELLOW, COL_YELLOW_LO);
                gfx_ui_font_text(UI_FONT_DISPLAY, sx + (tw + slant - lw) / 2 - u * 0.06f, cy - lh / 2, PAGES[i],
                                 ui_col(COL_NAVY), ui_col(COL_NAVY), 0.42f);
            } else {
                ui_slant(sx, ty + u * 0.12f, tw - u * 0.12f, tab_h - u * 0.24f, slant, 0xFF183052, 0xFF122440);
                gfx_ui_font_text(UI_FONT_DISPLAY, sx + (tw + slant - lw) / 2 - u * 0.06f, cy - lh / 2, PAGES[i],
                                 ui_col(COL_DIM), ui_col(COL_DIM), 0.42f);
            }
        }
    }

    /* rows */
    rows_y = y + head_h + tab_h + u * 0.5f;
    n = page_items(M.page, &first);
    {
        float target = rows_y + (M.cursor - first) * rowh, fade_in;
        if (M.sel_page != M.page || M.sel_y <= 0) {
            M.sel_y = target;
            M.sel_page = M.page;
        }
        M.sel_y += (target - M.sel_y) * (dt * 20 > 1 ? 1 : dt * 20);
        fade_in = ui_ease_out((float)((t - M.page_since) / 0.16));
        /* selection: a slanted light bar with the yellow pointer at its left */
        {
            float sy = M.sel_y, bob = sinf((float)t * 6.0f) * u * 0.08f;
            float pulse = 0.75f + 0.25f * sinf((float)t * 4.0f);
            ui_slant(x + pad * 0.6f, sy + u * 0.08f, pw - pad * 1.4f, rowh - u * 0.16f, u * 0.25f,
                      ui_with_alpha(0xFF2F8FE8, 0.85f * pulse), 0x302F8FE8);
            gfx_ui_rect(x + pad * 0.6f, sy + u * 0.08f, u * 0.09f, rowh - u * 0.16f, ui_col(COL_YELLOW));
            ui_arrow(x + pad * 0.35f + bob, sy + rowh / 2, u * 0.38f, 1, COL_YELLOW);
        }
        for (i = 0; i < n; i++) {
            const item *it = &g_items[first + i];
            float ry = rows_y + i * rowh + (1 - fade_in) * u * 0.3f * (i + 1), cy = ry + rowh / 2;
            float save = ui_alpha;
            int sel = first + i == M.cursor;
            ui_alpha *= fade_in;
            if (i > 0)
                gfx_ui_rect(x + pad * 1.2f, ry, pw - pad * 2.2f, 1, ui_col(0x14FFFFFF));
            ui_text(x + pad * 1.6f, cy - gfx_ui_font_line(UI_FONT_BODY, 0.9f) / 2, it->label,
                 sel ? 0xFFFFFFFF : 0xFFC4D3E2, 0.9f);
            draw_value(it, x + pw - pad * 1.2f, cy, u, sel, t);
            ui_alpha = save;
        }
    }

    /* help for the selected setting (or a warning) */
    {
        float hy = rows_y + maxrows * rowh + u * 0.4f, hx = x + pad * 1.2f, hw = pw - pad * 2.4f;
        gfx_ui_hgrad(hx, hy, hw, 1.5f, ui_col(0xA05CC8FF), ui_col(0x005CC8FF));
        hy += u * 0.35f;
        if (M.note[0] && t < M.note_until) {
            float s = u * 0.55f;
            gfx_ui_tri(hx + s / 2, hy + u * 0.05f, hx + s, hy + s + u * 0.05f, hx, hy + s + u * 0.05f, ui_col(COL_WARN));
            ui_text(hx + s * 0.38f, hy + s * 0.18f, "!", COL_NAVY, 0.5f);
            wrapped(hx + s + u * 0.3f, hy, hw - s - u * 0.3f, M.note, COL_WARN, 0.72f);
        } else if (M.cursor >= 0) {
            wrapped(hx, hy, hw, g_items[M.cursor].help, 0xFFB4C6D8, 0.72f);
        }
    }

    /* button prompts */
    {
        float fy = y + ph - foot_h / 2 - pad * 0.5f, fx = x + pad * 1.2f, r = u * 0.34f, sc = 0.68f;
        float lh = gfx_ui_font_line(UI_FONT_BODY, sc);
        gfx_ui_rect(x + pad * 1.2f, fy - foot_h / 2, pw - pad * 2.4f, 1, ui_col(0x14FFFFFF));
        if (M.pad_ui) {
            fx += dpad_glyph(fx, fy, r * 2) + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Choose", COL_DIM, sc) + u * 0.7f;
            fx += pad_button(fx, fy, r, "A", 0xFF3FA34D) + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Change", COL_DIM, sc) + u * 0.7f;
            fx += pad_button(fx, fy, r, "B", 0xFFD8443C) + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Close", COL_DIM, sc) + u * 0.7f;
        } else {
            fx += ui_key_cap(fx, fy, r * 1.9f, "Arrows") + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Choose", COL_DIM, sc) + u * 0.7f;
            fx += ui_key_cap(fx, fy, r * 1.9f, "Enter") + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Change", COL_DIM, sc) + u * 0.7f;
            fx += ui_key_cap(fx, fy, r * 1.9f, "Esc") + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Close", COL_DIM, sc) + u * 0.7f;
        }
    }
}

/* ---- input ---- */

enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_OK = 16, K_BACK = 32, K_TOGGLE = 64,
       K_PREV = 128, K_NEXT = 256 };

static int read_keys(WORD *buttons)
{
    pad_t p;
    int conn, k = 0, kb = 0;
    memset(&p, 0, sizeof p);
    conn = input_sdl_snapshot(&p);
    conn = controls_debug_overlay(&p, conn);
    *buttons = conn ? p.wButtons : 0;
    if (conn) {
        if ((p.wButtons & BTN_DPAD_UP) || p.sThumbLY > 16000) k |= K_UP;
        if ((p.wButtons & BTN_DPAD_DOWN) || p.sThumbLY < -16000) k |= K_DOWN;
        if ((p.wButtons & BTN_DPAD_LEFT) || p.sThumbLX < -16000) k |= K_LEFT;
        if ((p.wButtons & BTN_DPAD_RIGHT) || p.sThumbLX > 16000) k |= K_RIGHT;
        if (p.wButtons & BTN_A) k |= K_OK;
        if (p.wButtons & BTN_B) k |= K_BACK;
        if (p.wButtons & BTN_LB) k |= K_PREV;
        if (p.wButtons & BTN_RB) k |= K_NEXT;
        if ((p.wButtons & (BTN_BACK | BTN_START)) == (BTN_BACK | BTN_START)) k |= K_TOGGLE;
        if (k)
            M.pad_ui = 1;
    }
    if (kbm_foreground()) {
#define KEY(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
        if (KEY(VK_UP)) kb |= K_UP;
        if (KEY(VK_DOWN)) kb |= K_DOWN;
        if (KEY(VK_LEFT)) kb |= K_LEFT;
        if (KEY(VK_RIGHT)) kb |= K_RIGHT;
        if (KEY(VK_RETURN) || KEY(VK_SPACE)) kb |= K_OK;
        if (KEY(VK_ESCAPE) || KEY(VK_BACK)) kb |= K_BACK;
        if (KEY('Q') || KEY(VK_PRIOR)) kb |= K_PREV;
        if (KEY('E') || KEY(VK_NEXT)) kb |= K_NEXT;
        if (KEY(VK_F1)) kb |= K_TOGGLE;
#undef KEY
        if (kb)
            M.pad_ui = 0;
        k |= kb;
    }
    return k;
}

static void set_page(int page)
{
    int first;
    M.page = (page + NPAGES) % NPAGES;
    page_items(M.page, &first);
    M.cursor = first;
    M.page_since = now_s();
}

static void move_cursor(int dir)
{
    int first, n = page_items(M.page, &first);
    M.cursor = first + (M.cursor - first + dir + n) % n;
}

static void set_open(int open)
{
    M.open = open;
    M.block = 1;   /* until every key/button is let go: nothing leaks into the game */
    if (open) {
        M.page_since = now_s();
        M.sel_y = 0;
    }
    log_printf("menu: %s", open ? "open" : "closed");
}

void menu_frame(void)
{
    WORD buttons;
    int keys, pressed, dir = 0;
    double t;
    if (!M.ready)
        return;
    keys = read_keys(&buttons);
    pressed = keys & ~M.prev_keys;
    M.prev_keys = keys;
    if (M.block && !keys && !buttons)
        M.block = 0;
    if (pressed & K_TOGGLE) {
        set_open(!M.open);
        return;
    }
    if (!M.open)
        return;
    if (pressed & K_BACK) {
        set_open(0);
        return;
    }
    if (pressed & K_PREV)
        set_page(M.page - 1);
    if (pressed & K_NEXT)
        set_page(M.page + 1);
    /* direction with hold-to-repeat */
    t = now_s();
    if (keys & K_UP) dir = K_UP;
    else if (keys & K_DOWN) dir = K_DOWN;
    else if (keys & K_LEFT) dir = K_LEFT;
    else if (keys & K_RIGHT) dir = K_RIGHT;
    if (dir != M.held_dir) {
        M.held_dir = dir;
        M.held_since = M.last_repeat = t;
        if (!dir)
            return;
    } else if (!dir || t - M.held_since < 0.35 || t - M.last_repeat < 0.07) {
        dir = 0;
    } else {
        M.last_repeat = t;
    }
    if (dir == K_UP)
        move_cursor(-1);
    else if (dir == K_DOWN)
        move_cursor(1);
    else if (dir == K_LEFT)
        change(g_items[M.cursor].kind, -1);
    else if (dir == K_RIGHT)
        change(g_items[M.cursor].kind, 1);
    if (pressed & K_OK)
        change(g_items[M.cursor].kind, 1);
}

/* Pad chords with Back: Back + Start opens the menu, Back + RB quick saves.
 * Back and Start also mean something to the game (Start pauses), and people
 * never press two buttons on the same frame, so a lone Back or Start is held
 * back for a moment: if the partner follows, it was a chord and the game never
 * sees either; if not, the game gets it late (held) or as a short tap (let go
 * in time). RB isn't held back (nothing is bound to it by default). */
#define CHORD_S 0.12

static struct {
    WORD prev, pending, tap, swallow;
    int tap_polls;
    double since;
} F;
static volatile LONG g_save_req;

static void chord(unsigned short *buttons, WORD which)
{
    F.swallow = which;   /* keep these from the game until they're let go */
    F.pending = F.tap = 0;
    F.tap_polls = 0;
    *buttons &= (WORD)~which;
}

void menu_filter_buttons(unsigned short *buttons)
{
    const WORD HELD = BTN_BACK | BTN_START, MENU = BTN_BACK | BTN_START, SAVE = BTN_BACK | BTN_RB;
    WORD cur, newly;
    if (!M.ready)
        return;
    cur = (WORD)(*buttons & (HELD | BTN_RB));
    newly = (WORD)(cur & HELD & ~F.prev);
    F.prev = (WORD)(cur & HELD);
    if (F.swallow) {
        WORD sw = F.swallow;
        if (!(cur & sw))
            F.swallow = 0;
        *buttons &= (WORD)~sw;
        return;
    }
    if ((cur & MENU) == MENU) {   /* menu_frame sees it too and opens / closes the menu */
        chord(buttons, MENU);
        return;
    }
    if ((cur & SAVE) == SAVE) {
        chord(buttons, SAVE);
        if (!M.open)
            InterlockedExchange(&g_save_req, 1);
        return;
    }
    if (F.pending) {
        if (cur & F.pending) {
            if (now_s() - F.since < CHORD_S)
                *buttons &= (WORD)~F.pending;   /* still waiting for a partner */
            else
                F.pending = 0;                  /* a plain hold: pass it on */
        } else {
            F.tap = F.pending;                  /* let go in time: replay it as a tap */
            F.tap_polls = 2;
            F.pending = 0;
        }
    } else if (newly) {
        F.pending = newly;
        F.since = now_s();
        *buttons &= (WORD)~newly;
    }
    if (F.tap_polls > 0) {
        *buttons |= F.tap;
        F.tap_polls--;
    }
}

int menu_take_quicksave(void)
{
    return InterlockedExchange(&g_save_req, 0) != 0;
}

int menu_blocks_input(void)
{
    return M.open || M.block;
}

void menu_install(const char *ini, const char *game_dir)
{
    char s[MAX_PATH], full[MAX_PATH], dir[MAX_PATH];
    int i;
    QueryPerformanceFrequency(&M.qpf);
    strcpy_s(M.ini, sizeof M.ini, ini);
    strcpy_s(M.game_dir, sizeof M.game_dir, game_dir);
    M.fps60 = GetPrivateProfileIntA("video", "interp", 1, ini) != 0;
    M.vsync = GetPrivateProfileIntA("video", "vsync", 0, ini) != 0;
    M.res = GetPrivateProfileIntA("video", "render_scale", 0, ini);
    if (M.res < 0 || M.res > 4)
        M.res = 0;
    M.msaa = GetPrivateProfileIntA("video", "msaa", 1, ini);
    if (M.msaa != 4 && M.msaa != 8)
        M.msaa = 1;
    M.shader_game_res = GetPrivateProfileIntA("video", "shader_scale", 1, ini) != 0;
    M.nearest = GetPrivateProfileIntA("video", "nearest_upscale", 0, ini) != 0;
    M.aniso = GetPrivateProfileIntA("video", "anisotropy", 16, ini) > 1;
    GetPrivateProfileStringA("video", "fog", "original", s, sizeof s, ini);
    M.fog = _stricmp(s, "off") == 0 ? FOG_OFF : _stricmp(s, "light") == 0 ? FOG_LIGHT : FOG_ORIGINAL;
    GetPrivateProfileStringA("video", "lod", "full", s, sizeof s, ini);
    M.lod_full = _stricmp(s, "original") != 0;
    M.hdtex = GetPrivateProfileIntA("textures", "replace", 1, ini) != 0;
    M.cam_speed = nearest_index(SPEEDS, 6, GetPrivateProfileIntA("controls", "camera_speed_x", 100, ini));
    M.inv_x = GetPrivateProfileIntA("controls", "invert_x", 0, ini) != 0;
    M.inv_y = GetPrivateProfileIntA("controls", "invert_y", 0, ini) != 0;
    M.mouse = GetPrivateProfileIntA("controls", "mouse_keyboard", 1, ini)
                  ? nearest_index(MOUSE + 1, 5, GetPrivateProfileIntA("controls", "mouse_speed", 100, ini)) + 1 : 0;
    GetPrivateProfileStringA("controls", "reticle_color", "FFD840", s, sizeof s, ini);
    {
        unsigned rgb = (unsigned)strtoul(s, NULL, 16);
        M.reticle = 0;
        for (i = 0; i < (int)(sizeof RETICLES / sizeof RETICLES[0]); i++)
            if (RETICLES[i].rgb == rgb)
                M.reticle = i;
    }
    /* presets */
    M.presets = (char **)calloc(MAX_PRESETS, sizeof *M.presets);
    if (M.presets) {
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\Modernization\\shaders", game_dir);
        scan_presets(dir, 0);
        qsort(M.presets, (size_t)M.npresets, sizeof *M.presets, by_name);
    }
    GetPrivateProfileStringA("video", "shader", "", s, sizeof s, ini);
    full_path(s, full, sizeof full);
    M.shader = -1;
    M.shader_custom[0] = 0;
    for (i = 0; full[0] && i < M.npresets; i++)
        if (_stricmp(M.presets[i], full) == 0)
            M.shader = i;
    if (full[0] && M.shader < 0)
        strcpy_s(M.shader_custom, sizeof M.shader_custom, full);
    {
        pad_t p;
        M.pad_ui = input_sdl_snapshot(&p);
    }
    set_page(0);
    gfx_ui_add_draw(draw, NULL);
    M.ready = 1;
    log_printf("menu: F1 or Back + Start (%d shader presets found)", M.npresets);
}
