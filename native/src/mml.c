/* mml.dll: native runtime for the Mega Man Legends Modernization Project.
 *
 * Loaded by MEGAMAN.EXE through an extra import-table entry (see
 * tools/patch_add_import.py), so DllMain runs after the exe is mapped and its
 * imports are bound, but before its entry point. Everything here patches the
 * game in place and runs on the game's own thread (the only extra thread is
 * the short-lived gamepad intro-skip poller).
 *
 * Features ported off the Frida runtime so far:
 *   - anamorphic widescreen projection
 *   - GetTickCount rebase + frame-limiter float drift fix (audio sync)
 *   - intro bypass / gamepad intro skip
 *   - modern controls + orbit indicator (controls.c, toast.c)
 */
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apicensus.h"
#include "controls.h"
#include "input.h"
#include "lockon.h"
#include "ddshim.h"
#include "display.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "mml.h"
#include "packets.h"
#include "toast.h"
#include "terrain.h"
#include "fog.h"
#include "lod.h"
#include "gte_capture.h"
#include "frame_rec.h"
#include "bglayer.h"
#include "watch.h"

#define MML_NATIVE_BUILD "0.2.0-dev (native widescreen)"

static char g_game_dir[MAX_PATH];
static char g_ini[MAX_PATH];

/* ---- config ---- */

static float ini_float(const char *sec, const char *key, float def)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof buf, g_ini);
    return buf[0] ? (float)atof(buf) : def;
}

static void ini_string(const char *sec, const char *key, const char *def, char *out, DWORD n)
{
    GetPrivateProfileStringA(sec, key, def, out, n, g_ini);
}

/* ---- anamorphic widescreen ----
 * The game renders a fixed 4:3 frame that DDrawCompat stretches to the display.
 * Five projection instructions read the X scale from gte_h_float; we point them
 * at g_shadow_h_x = H * ratio so the 3D is pre-squeezed and the stretch cancels
 * out into a wider field of view. H itself is pulled in by g_closer so the
 * vertical framing matches the original. Recomputed whenever the game sets H. */

static float g_closer = 0.859375f;
static float g_ratio = 1.0f;
static int g_widescreen_on;
static int g_native_wide;   /* true 16:9 surface instead of the anamorphic squeeze */

float mml_hud_ratio(void)
{
    return (g_widescreen_on && !g_native_wide) ? g_ratio : 1.0f;
}
static volatile float g_shadow_h_x;
static int g_h_set_logs;

float mml_h_x(void)
{
    return g_widescreen_on ? g_shadow_h_x : *(float *)G_GTE_H_FLOAT;
}

static void on_h_set(regs_t *r)
{
    float *h = (float *)G_GTE_H_FLOAT;
    float adj = *h * g_closer;
    (void)r;
    *h = adj;
    g_shadow_h_x = adj * g_ratio;
    if (g_h_set_logs < 8) {
        g_h_set_logs++;
        log_printf("widescreen: game set H %.3f -> H %.3f, shadow X %.3f",
                   adj / g_closer, adj, (double)g_shadow_h_x);
    }
}

static void install_widescreen(void)
{
    static const uintptr_t x_sites[] = { 0x4EEEED, 0x4EEF21, 0x4EEF8D, 0x4F0685, 0x4F07EE };
    /* mov dword [gte_near], imm32: the immediate is 250.0 in vanilla and 31.25
     * after our near-plane exe patch, so only the opcode+address are checked. */
    static const uint8_t h_default_prefix[] = { 0xC7, 0x05, 0xC0, 0xEB, 0x77, 0x00 };
    static const uint8_t h_set_bytes[] = { 0xD8, 0x0D, 0x18, 0xA5, 0x50, 0x00 };
    uint8_t h_default_bytes[10];
    const uint32_t vanilla = G_GTE_H_FLOAT;
    uint32_t shadow = (uint32_t)(uintptr_t)&g_shadow_h_x;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    size_t i;

    if (sw <= 0 || sh <= 0) {
        log_printf("widescreen: no screen size, skipped");
        return;
    }
    /* native mode draws onto a real wide surface, so X isn't squeezed */
    g_ratio = g_native_wide ? 1.0f : (640.0f / 480.0f) / ((float)sw / (float)sh);

    /* All five sites must look right before we touch any of them. */
    for (i = 0; i < sizeof x_sites / sizeof x_sites[0]; i++) {
        if (!mem_expect(x_sites[i] + 2, &vanilla, 4)) {
            log_printf("widescreen: site %08X unexpected, skipped", (unsigned)x_sites[i]);
            return;
        }
    }
    if (!mem_expect(G_SITE_H_DEFAULT, h_default_prefix, sizeof h_default_prefix)) {
        log_printf("widescreen: H default site unexpected, skipped");
        return;
    }
    memcpy(h_default_bytes, (const void *)G_SITE_H_DEFAULT, sizeof h_default_bytes);
    if (!hook_mid(G_SITE_H_DEFAULT, sizeof h_default_bytes, h_default_bytes, on_h_set) ||
        !hook_mid(G_SITE_H_SET, sizeof h_set_bytes, h_set_bytes, on_h_set)) {
        log_printf("widescreen: H setter hooks failed");
        return;
    }
    for (i = 0; i < sizeof x_sites / sizeof x_sites[0]; i++)
        mem_write(x_sites[i] + 2, &shadow, 4);
    g_widescreen_on = 1;
    log_printf("widescreen: ON, %s (screen %dx%d, ratio %.4f, closer %.6f)",
               g_native_wide ? "native surface" : "anamorphic", sw, sh, g_ratio, g_closer);
}

/* ---- timing ----
 * The frame limiter accumulates +16.667 ms per frame into a float32 compared
 * against GetTickCount. At high system uptime the float's precision rounds the
 * step, so the game runs a few percent fast and cutscene audio drifts.
 * (1) GetTickCount returns ms since launch, keeping magnitudes small.
 * (2) Once per frame the threshold is rewritten from a float64 mirror, so it
 *     never drifts even past 2^23 ms of session time. */

static DWORD(WINAPI *g_real_gettickcount)(void);
static DWORD g_tick_base;

/* The game paces its frames with GetTickCount, which only advances every
 * ~15.6 ms, so its 33.3 ms frames really came out as 31 or 47 ms. A 1 ms
 * clock from the performance counter makes them even (and lets the 60 fps
 * interpolation space its frames evenly). Starting near 0 also keeps the
 * game's float frame-limiter threshold precise. */
static LARGE_INTEGER g_qpc_base, g_qpc_freq;

#define G_SPIN_RET 0x4A7132u   /* game_frame: do { t = GetTickCount(); } while (t < threshold); */
static HANDLE g_wait_timer;

/* Sleep for ms with sub-millisecond precision (no busy waiting). */
void mml_sleep_ms(double ms)
{
    LARGE_INTEGER due;
    if (ms <= 0.0)
        return;
    if (!g_wait_timer)
        g_wait_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!g_wait_timer) {
        Sleep((DWORD)ms);
        return;
    }
    due.QuadPart = -(LONGLONG)(ms * 10000.0);   /* relative, 100 ns units */
    if (SetWaitableTimer(g_wait_timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(g_wait_timer, INFINITE);
}

static DWORD WINAPI rebased_gettickcount(void)
{
    LARGE_INTEGER now;
    double ms;
    if (!g_qpc_freq.QuadPart)
        return g_real_gettickcount() - g_tick_base;
    QueryPerformanceCounter(&now);
    ms = (double)(now.QuadPart - g_qpc_base.QuadPart) * 1000.0 / (double)g_qpc_freq.QuadPart;
    /* the game waits for its next frame by spinning on this call: sleep instead */
    if ((uintptr_t)_ReturnAddress() == G_SPIN_RET) {
        double left = *(float *)G_FRAME_LIMITER_TH - ms;
        if (left > 1.5 && left < 200.0) {
            mml_sleep_ms(left - 1.0);
            QueryPerformanceCounter(&now);
            ms = (double)(now.QuadPart - g_qpc_base.QuadPart) * 1000.0 / (double)g_qpc_freq.QuadPart;
        }
    }
    return (DWORD)ms;
}

static double g_limiter_acc;
static int g_limiter_init;

static unsigned g_frames;
static int g_interp_on, g_bglayer_on, g_player_log;
extern int g_interp_census, g_smooth_bg, g_blend_census;

static void limiter_frame(void);

/* once per frame, right before the display list is walked */
static void on_frame(regs_t *r)
{
    (void)r;
    if (++g_frames == 300) {
        RECT rc = { 0 };
        HWND w = GetForegroundWindow();
        if (w)
            GetClientRect(w, &rc);
        log_printf("frame hook alive: 300 frames, limiter threshold %.1f ms, gfx path flags %08X, fg client %ldx%ld",
                   *(float *)G_FRAME_LIMITER_TH, *(unsigned *)0x88787Cu, rc.right, rc.bottom);
        {
            static const char *shims[] = { "apphelp.dll", "AcLayers.dll", "AcGenral.dll", "AcXtrnal.dll",
                                           "AcSpecfc.dll", "ddraw.dll", "dxgi.dll", "d3d9.dll" };
            char line[256] = "";
            size_t k;
            for (k = 0; k < sizeof shims / sizeof shims[0]; k++)
                if (GetModuleHandleA(shims[k])) {
                    strcat_s(line, sizeof line, shims[k]);
                    strcat_s(line, sizeof line, " ");
                }
            log_printf("loaded: %s", line);
        }
    }
    if (g_interp_on)
        gte_capture_frame_end();
    if (g_bglayer_on)
        bglayer_frame_end();
    watch_frame();
    if (g_player_log) {
        /* dev: log every player state change (player struct 0x5F8364) */
        static uint32_t last = 0xFFFFFFFF;
        const uint8_t *pl = (const uint8_t *)0x5F8364u;
        uint32_t now = pl[9] | pl[10] << 8 | (uint32_t)pl[0xAC] << 16 | (uint32_t)pl[0x174] << 24;
        if (now != last) {
            log_printf("player: f%u state %d sub %d var %d anim %02X facing %03X pos %d,%d held %04X press %04X",
                       g_frames, pl[9], pl[10], pl[11], pl[0xAC], (*(const uint32_t *)(pl + 0x54) >> 16) & 0xfff,
                       *(const int32_t *)(pl + 0x1C) >> 16, *(const int32_t *)(pl + 0x24) >> 16,
                       *(const uint16_t *)(pl + 0x11C), *(const uint16_t *)(pl + 0x11E));
            last = now;
        }
    }
    limiter_frame();
    apicensus_frame();
    ddshim_frame();
    display_frame();
    toast_frame();
    lockon_frame();
    terrain_frame();
    fog_frame();
}

static void limiter_frame(void)
{
    float *th = (float *)G_FRAME_LIMITER_TH;
    double expected, game = *th;
    if (!g_limiter_init) {
        g_limiter_acc = game;
        g_limiter_init = 1;
        return;
    }
    expected = g_limiter_acc + *(uint8_t *)G_FRAME_STEP * *(double *)G_FRAME_MS;
    /* the game reset the threshold (level load, frame skip): follow it */
    g_limiter_acc = fabs(game - expected) > 8.0 ? game : expected;
    *th = (float)g_limiter_acc;
}

static void install_timing(void)
{
    static const uint8_t walker_call[] = { 0xE8, 0x96, 0x05, 0x05, 0x00 };   /* call 0x4F7680 */
    void **slot = (void **)G_IAT_GETTICKCOUNT;
    void *real = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetTickCount");
    void *mine = (void *)rebased_gettickcount;

    if (*slot != real) {
        log_printf("timing: GetTickCount import slot holds %p (expected %p), skipped", *slot, real);
    } else {
        g_real_gettickcount = (DWORD(WINAPI *)(void))real;
        g_tick_base = g_real_gettickcount();
        QueryPerformanceFrequency(&g_qpc_freq);
        QueryPerformanceCounter(&g_qpc_base);
        mem_write(G_IAT_GETTICKCOUNT, &mine, sizeof mine);
        log_printf("timing: GetTickCount rebased");
    }
    if (hook_mid(G_SITE_WALKER_CALL, sizeof walker_call, walker_call, on_frame))
        log_printf("timing: frame-limiter drift fix ON");
}

/* ---- intro ---- */

/* returns 1 if the intro still plays (so the gamepad skip is wanted) */
static int install_intro(void)
{
    static const uint8_t call_intro[] = { 0xE8, 0xB0, 0x29, 0x05, 0x00 };
    static const uint8_t nops[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    char mode[16];

    ini_string("general", "intro", "play", mode, sizeof mode);
    if (_stricmp(mode, "skip") != 0)
        return 1;
    if (mem_expect(G_SITE_INTRO_CALL, call_intro, sizeof call_intro) &&
        mem_write(G_SITE_INTRO_CALL, nops, sizeof nops))
        log_printf("intro: bypassed");
    else {
        log_printf("intro: unexpected bytes, left vanilla");
        return 1;
    }
    return 0;
}

/* ---- entry ---- */

static void init(void)
{
    char aspect[16], controls[16], mode[16];
    char *slash;
    int intro_plays;

    GetModuleFileNameA(NULL, g_game_dir, sizeof g_game_dir);
    slash = strrchr(g_game_dir, '\\');
    if (slash)
        *slash = 0;
    strcpy_s(g_ini, sizeof g_ini, g_game_dir);
    strcat_s(g_ini, sizeof g_ini, "\\mml.ini");
    log_open(g_game_dir);
    log_printf("mml.dll %s loaded into %s", MML_NATIVE_BUILD, g_game_dir);

    ini_string("video", "aspect", "16:9", aspect, sizeof aspect);
    g_closer = ini_float("video", "closer_factor", g_closer);

    install_timing();
    ini_string("video", "mode", "anamorphic", mode, sizeof mode);
    if (strcmp(aspect, "4:3") != 0) {
        if (_stricmp(mode, "native") == 0)
            g_native_wide = display_install(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                                            GetPrivateProfileIntA("video", "native_width", 0, g_ini),
                                            GetPrivateProfileIntA("video", "hud_edges", 1, g_ini));
        install_widescreen();
    }
    else
        log_printf("widescreen: 4:3 selected, vanilla projection");
    intro_plays = install_intro();
    ini_string("controls", "mode", "modern", controls, sizeof controls);
    controls_install(_stricmp(controls, "vanilla") != 0, intro_plays);
    lockon_install();
    {
        char c[16];
        ini_string("controls", "reticle_color", "", c, sizeof c);
        if (strlen(c) == 6 && strspn(c, "0123456789abcdefABCDEF") == 6)
            lockon_set_color((unsigned)strtoul(c, NULL, 16));
    }
    controls_load_buttons(g_ini);
    controls_camera(GetPrivateProfileIntA("controls", "camera_speed_x", 100, g_ini),
                    GetPrivateProfileIntA("controls", "camera_speed_y", 100, g_ini),
                    GetPrivateProfileIntA("controls", "invert_x", 0, g_ini),
                    GetPrivateProfileIntA("controls", "invert_y", 0, g_ini),
                    GetPrivateProfileIntA("controls", "deadzone", 24, g_ini),
                    GetPrivateProfileIntA("controls", "orbit_start", 0, g_ini));
    controls_tune(GetPrivateProfileIntA("controls", "roll_speed", 0, g_ini),
                  GetPrivateProfileIntA("controls", "roll_anim", -1, g_ini),
                  GetPrivateProfileIntA("controls", "walk_tilt", -1, g_ini),
                  GetPrivateProfileIntA("controls", "walk_min", -1, g_ini),
                  GetPrivateProfileIntA("controls", "run_min", -1, g_ini));
    if (GetPrivateProfileIntA("debug", "remote_pad", 0, g_ini))
        controls_debug_pad();
    if (GetPrivateProfileIntA("debug", "lock_test", 0, g_ini))
        controls_debug_lock_test();
    {
        char in[16];
        ini_string("controls", "input", "sdl", in, sizeof in);
        if (_stricmp(in, "legacy") != 0)
            input_install();
        else
            log_printf("input: legacy controller path selected (DirectInput joystick via Xidi)");
    }
    {
        char w[128];
        ini_string("debug", "watch", "", w, sizeof w);
        if (w[0])
            watch_configure(w);
    }
    g_player_log = GetPrivateProfileIntA("debug", "player_log", 0, g_ini);
    if (GetPrivateProfileIntA("debug", "census", 0, g_ini))
        census_enable();
    {
        char f[16];
        ini_string("video", "fog", "original", f, sizeof f);
        if (_stricmp(f, "off") == 0 || GetPrivateProfileIntA("video", "no_fog", 0, g_ini))
            fog_install(FOG_OFF, 0.0f);
        else if (_stricmp(f, "light") == 0)
            fog_install(FOG_LIGHT, ini_float("video", "fog_distance", 2.0f));
    }
    {
        char l[16];
        ini_string("video", "lod", "full", l, sizeof l);
        if (_stricmp(l, "original") != 0)
            lod_install(GetPrivateProfileIntA("debug", "keep_small_tris", 1, g_ini));
    }
    if (GetPrivateProfileIntA("video", "terrain_clip", 1, g_ini))
        terrain_clip_install(ini_float("debug", "clip_near_scale", 1.0f));
    if (GetPrivateProfileIntA("debug", "clip_census", 0, g_ini))
        terrain_census_enable();
    if (GetPrivateProfileIntA("debug", "api_census", 0, g_ini))
        apicensus_install();
    {
        /* [video] renderer=mml (default): our own Direct3D 11 renderer; ddrawcompat = leave DirectDraw to the wrapper */
        char r[16];
        ini_string("video", "renderer", "mml", r, sizeof r);
        if (_stricmp(r, "mml") == 0) {
            gfx_options o;
            memset(&o, 0, sizeof o);
            o.windowed = GetPrivateProfileIntA("video", "windowed", 0, g_ini);
            o.exclusive = GetPrivateProfileIntA("video", "exclusive", 0, g_ini);
            o.render_scale = GetPrivateProfileIntA("video", "render_scale", 0, g_ini);
            o.nearest_upscale = GetPrivateProfileIntA("video", "nearest_upscale", 0, g_ini);
            o.msaa = GetPrivateProfileIntA("video", "msaa", 1, g_ini);
            o.anisotropy = GetPrivateProfileIntA("video", "anisotropy", 16, g_ini);
            o.vsync = GetPrivateProfileIntA("video", "vsync", 0, g_ini);
            o.no_fog = GetPrivateProfileIntA("video", "no_fog", 0, g_ini);
            o.fog_from_w = GetPrivateProfileIntA("video", "fog_from_w", 0, g_ini);
            {
                char gpu[16];
                ini_string("video", "gpu", "performance", gpu, sizeof gpu);
                o.gpu_display = _stricmp(gpu, "display") == 0;
            }
            g_fog_debug = GetPrivateProfileIntA("debug", "fog_debug", 0, g_ini);
            g_drop_clamped = GetPrivateProfileIntA("video", "drop_clamped", 1, g_ini);
            {
                char pr[32];
                ini_string("debug", "probe", "", pr, sizeof pr);
                if (pr[0])
                    sscanf_s(pr, "%d,%d", &g_probe_x, &g_probe_y);
            }
            /* Per-monitor DPI aware: otherwise on a scaled display (4K at 200%) Windows
             * hands us a 1080p window and the desktop compositor stretches every frame
             * up to the real resolution, which is blurry and costs a lot of GPU. */
            {
                typedef BOOL(WINAPI *set_ctx_t)(HANDLE);
                set_ctx_t set_ctx = (set_ctx_t)GetProcAddress(GetModuleHandleA("user32.dll"),
                                                              "SetProcessDpiAwarenessContext");
                typedef UINT(WINAPI *get_dpi_t)(void);
                get_dpi_t get_dpi = (get_dpi_t)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForSystem");
                BOOL ok = set_ctx ? set_ctx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */) : FALSE;
                DWORD err = GetLastError();
                log_printf("video: per-monitor DPI aware %s (error %lu); screen %dx%d, system DPI %u", ok ? "ON" : "failed",
                           err, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), get_dpi ? get_dpi() : 0);
            }
            g_blend_census = GetPrivateProfileIntA("debug", "blend_census", 0, g_ini);
            ddshim_install(&o);
            /* [video] smooth_sky=1: background layers scroll sub-pixel smooth */
            if (GetPrivateProfileIntA("video", "smooth_sky", 1, g_ini) && bglayer_install()) {
                extern int g_bglayer_log;
                g_bglayer_on = g_smooth_bg = 1;
                g_bglayer_log = GetPrivateProfileIntA("debug", "bglayer_log", 0, g_ini);
            }
            /* [video] interp=1: 60 fps by drawing an in-between frame per game tick */
            {
                int want = GetPrivateProfileIntA("video", "interp", 1, g_ini);
                g_interp_census = GetPrivateProfileIntA("debug", "interp_census", 0, g_ini);
                if ((want || g_interp_census) && gte_capture_install(g_interp_census)) {
                    g_interp_on = 1;
                    rec_enable(want);
                    rec_debug(GetPrivateProfileIntA("debug", "interp_mid_only", 0, g_ini),
                              GetPrivateProfileIntA("debug", "interp_timing", 0, g_ini));
                    log_printf("interp: %s", want ? "60 fps interpolation ON" : "census only");
                }
            }
        }
    }
    log_printf("init done");
}

/* Referenced by the exe's import entry so the loader pulls us in. */
__declspec(dllexport) void mml_entry(void) {}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init();
    }
    return TRUE;
}
