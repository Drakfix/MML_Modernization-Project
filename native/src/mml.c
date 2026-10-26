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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apicensus.h"
#include "controls.h"
#include "ddshim.h"
#include "display.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "mml.h"
#include "packets.h"
#include "toast.h"

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

static DWORD WINAPI rebased_gettickcount(void)
{
    return g_real_gettickcount() - g_tick_base;
}

static double g_limiter_acc;
static int g_limiter_init;

static unsigned g_frames;

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
    }
    limiter_frame();
    apicensus_frame();
    ddshim_frame();
    display_frame();
    toast_frame();
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
    if (GetPrivateProfileIntA("debug", "remote_pad", 0, g_ini))
        controls_debug_pad();
    if (GetPrivateProfileIntA("debug", "census", 0, g_ini))
        census_enable();
    if (GetPrivateProfileIntA("debug", "api_census", 0, g_ini))
        apicensus_install();
    {
        /* [video] renderer=mml: our own Direct3D 11 renderer instead of DDrawCompat (experimental) */
        char r[16];
        ini_string("video", "renderer", "ddrawcompat", r, sizeof r);
        if (_stricmp(r, "mml") == 0) {
            gfx_options o;
            memset(&o, 0, sizeof o);
            o.windowed = GetPrivateProfileIntA("video", "windowed", 0, g_ini);
            o.msaa = GetPrivateProfileIntA("video", "msaa", 1, g_ini);
            o.anisotropy = GetPrivateProfileIntA("video", "anisotropy", 16, g_ini);
            o.vsync = GetPrivateProfileIntA("video", "vsync", 0, g_ini);
            o.no_fog = GetPrivateProfileIntA("video", "no_fog", 0, g_ini);
            o.fog_from_w = GetPrivateProfileIntA("video", "fog_from_w", 0, g_ini);
            g_fog_debug = GetPrivateProfileIntA("debug", "fog_debug", 0, g_ini);
            g_drop_clamped = GetPrivateProfileIntA("video", "drop_clamped", 1, g_ini);
            {
                char pr[32];
                ini_string("debug", "probe", "", pr, sizeof pr);
                if (pr[0])
                    sscanf_s(pr, "%d,%d", &g_probe_x, &g_probe_y);
            }
            ddshim_install(&o);
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
