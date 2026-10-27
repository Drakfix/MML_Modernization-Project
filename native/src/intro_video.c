/* Our own intro player: Modernization\intro.mpg (MPEG-1 video + MP2 audio)
 * decoded with pl_mpeg, drawn by our Direct3D 11 renderer, sound through SDL3.
 *
 * The game plays CAPCOM.AVI through MCI (intro_play_avi 0x4F9933), which
 * limits the file to Cinepak-era formats, needs a 640x480 window dance and
 * silently gives up when the full path reaches 128 characters. We detour that
 * function: with the modern renderer, SDL3 and our file present we play the
 * intro ourselves and return; otherwise the original code runs as before.
 *
 * Skipping: any key, mouse button or controller button (pressed after the
 * video started, so the button that started the game doesn't skip it). */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma warning(push, 0)
#define PL_MPEG_IMPLEMENTATION
#include "../vendor/pl_mpeg/pl_mpeg.h"
#pragma warning(pop)

#include "ddshim.h"
#include "display.h"
#include "gfx.h"
#include "hook.h"
#include "input.h"
#include "intro_video.h"
#include "log.h"
#include "mml.h"

#define G_FN_INTRO_PLAY_AVI  0x4F9933u   /* watcall EAX file name, EDX hwnd, EBX flag */
#define G_INTRO_PLAYING_FLAG 0x551FA8u

/* SDL3 audio, borrowed from the SDL3.dll input.c loaded */
typedef struct { uint32_t format; int channels; int freq; } sdl_audio_spec;
typedef struct SDL_AudioStream SDL_AudioStream;
static struct {
    unsigned char (__cdecl *InitSubSystem)(uint32_t);
    SDL_AudioStream *(__cdecl *OpenAudioDeviceStream)(uint32_t, const sdl_audio_spec *, void *, void *);
    unsigned char (__cdecl *PutAudioStreamData)(SDL_AudioStream *, const void *, int);
    unsigned char (__cdecl *ResumeAudioStreamDevice)(SDL_AudioStream *);
    void (__cdecl *DestroyAudioStream)(SDL_AudioStream *);
    const char *(__cdecl *GetError)(void);
} sa;
#define SDL_INIT_AUDIO_ 0x10u
#define SDL_AUDIO_F32_  0x8120u
#define SDL_DEFAULT_PLAYBACK 0xFFFFFFFFu

static char g_file[MAX_PATH];
static volatile LONG g_handled;
static uint8_t *g_trampoline;

/* per playback */
static gfx_tex *g_tex;
static uint32_t *g_rgba;
static int g_vw, g_vh, g_have_frame;
static SDL_AudioStream *g_stream;

static int load_sdl_audio(void)
{
#define GET(f, n) if (!(*(void **)&sa.f = input_sdl_proc(n))) return 0;
    GET(InitSubSystem, "SDL_InitSubSystem")
    GET(OpenAudioDeviceStream, "SDL_OpenAudioDeviceStream")
    GET(PutAudioStreamData, "SDL_PutAudioStreamData")
    GET(ResumeAudioStreamDevice, "SDL_ResumeAudioStreamDevice")
    GET(DestroyAudioStream, "SDL_DestroyAudioStream")
    GET(GetError, "SDL_GetError")
#undef GET
    return 1;
}

static void on_video(plm_t *plm, plm_frame_t *frame, void *user)
{
    (void)plm; (void)user;
    plm_frame_to_rgba(frame, (uint8_t *)g_rgba, g_vw * 4);
    gfx_tex_upload(g_tex, g_rgba, g_vw, g_vh);
    g_have_frame = 1;
}

static void on_audio(plm_t *plm, plm_samples_t *samples, void *user)
{
    (void)plm; (void)user;
    if (g_stream)
        sa.PutAudioStreamData(g_stream, samples->interleaved, (int)(samples->count * 2 * sizeof(float)));
}

static void draw_frame(void)
{
    gfx_state st;
    gfx_vertex v[4];
    float sw = (float)display_native_width(), sh = 480.0f, w = sw, h = sh, x, y;
    int i;
    gfx_clear(NULL, 1, 0xFF000000u, 1, 1.0f);
    if (g_have_frame) {
        /* fit the video into the screen, keeping its aspect */
        if (w * g_vh > h * g_vw)
            w = h * g_vw / g_vh;
        else
            h = w * g_vh / g_vw;
        x = (sw - w) / 2;
        y = (sh - h) / 2;
        memset(&st, 0, sizeof st);
        st.linear = 1;
        gfx_set_state(&st);
        gfx_set_texture(g_tex);
        for (i = 0; i < 4; i++) {
            v[i].x = x + ((i & 1) ? w : 0);
            v[i].y = y + ((i & 2) ? h : 0);
            v[i].z = 0.5f;
            v[i].rhw = 1.0f;
            v[i].diffuse = 0xFFFFFFFFu;
            v[i].specular = 0;
            v[i].u = (i & 1) ? 1.0f : 0.0f;
            v[i].v = (i & 2) ? 1.0f : 0.0f;
        }
        gfx_draw(GFX_TRISTRIP, v, 4);
        gfx_set_texture(NULL);
    }
    gfx_present();
}

/* 1 = played (or skipped) here; 0 = let the game's MCI player run */
static int __cdecl intro_native(HWND hwnd)
{
    plm_t *plm;
    LARGE_INTEGER f, t0, t1;
    int skip = 0, pad_was_down = 1, frames = 0;
    double played = 0;

    if (!g_file[0] || !hwnd || !input_sdl_active() || !load_sdl_audio())
        return 0;
    plm = plm_create_with_filename(g_file);
    if (!plm)
        return 0;
    if (!plm_probe(plm, 5000 * 1024) || !plm_get_width(plm) || !ddshim_video_begin(hwnd)) {
        log_printf("intro: %s not usable, original intro instead", g_file);
        plm_destroy(plm);
        return 0;
    }
    g_vw = plm_get_width(plm);
    g_vh = plm_get_height(plm);
    g_rgba = (uint32_t *)malloc((size_t)g_vw * g_vh * 4);
    g_tex = gfx_tex_create(g_vw, g_vh);
    g_have_frame = 0;
    g_stream = NULL;
    if (g_rgba && g_tex && plm_get_num_audio_streams(plm) > 0 && sa.InitSubSystem(SDL_INIT_AUDIO_)) {
        sdl_audio_spec spec;
        spec.format = SDL_AUDIO_F32_;
        spec.channels = 2;
        spec.freq = plm_get_samplerate(plm);
        g_stream = sa.OpenAudioDeviceStream(SDL_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (g_stream)
            sa.ResumeAudioStreamDevice(g_stream);
        else
            log_printf("intro: no audio device (%s)", sa.GetError());
    }
    if (!g_rgba || !g_tex) {
        log_printf("intro: out of memory for %dx%d", g_vw, g_vh);
        goto done;
    }
    plm_set_audio_enabled(plm, g_stream != NULL);
    plm_set_audio_lead_time(plm, 0.05);
    plm_set_video_decode_callback(plm, on_video, NULL);
    plm_set_audio_decode_callback(plm, on_audio, NULL);
    log_printf("intro: playing %s (%dx%d, %.2f fps, %s)", g_file, g_vw, g_vh, plm_get_framerate(plm),
               g_stream ? "with sound" : "silent");

    *(volatile uint8_t *)G_INTRO_PLAYING_FLAG = 1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    while (!plm_has_ended(plm) && !skip) {
        MSG m;
        double dt;
        int pad;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                PostQuitMessage((int)m.wParam);
                skip = 1;
                break;
            }
            if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN || m.message == WM_LBUTTONDOWN ||
                m.message == WM_RBUTTONDOWN)
                skip = 1;
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        pad = input_sdl_any_button();
        if (pad == 1 && !pad_was_down)
            skip = 1;
        pad_was_down = pad == 1;
        QueryPerformanceCounter(&t1);
        dt = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        t0 = t1;
        if (dt > 0.1)
            dt = 0.1;
        plm_decode(plm, dt);   /* 1080p: ~12 ms decode + ~7 ms colour conversion per new frame here */
        draw_frame();
        played += dt;
        frames++;
        {
            /* ~120 passes a second, sleeping only what's left after decoding */
            LARGE_INTEGER t2;
            double used;
            QueryPerformanceCounter(&t2);
            used = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)f.QuadPart;
            if (used < 1000.0 / 120.0)
                mml_sleep_ms(1000.0 / 120.0 - used);
        }
    }
    log_printf("intro: %s after %.1f s (%d frames drawn)", skip ? "skipped" : "finished", played, frames);
    *(volatile uint8_t *)G_INTRO_PLAYING_FLAG = 0;
    /* leave a black frame behind for the game to take over from */
    g_have_frame = 0;
    draw_frame();
done:
    if (g_stream)
        sa.DestroyAudioStream(g_stream);
    g_stream = NULL;
    if (g_tex)
        gfx_tex_destroy(g_tex);
    g_tex = NULL;
    free(g_rgba);
    g_rgba = NULL;
    plm_destroy(plm);
    return 1;
}

/* entry detour on intro_play_avi: our player, or the original via the trampoline */
static __declspec(naked) void intro_detour(void)
{
    __asm {
        pushad
        push edx                     ; hwnd
        call intro_native
        add esp, 4
        mov g_handled, eax
        popad
        cmp g_handled, 0
        jne played
        jmp dword ptr [g_trampoline]
    played:
        ret
    }
}

int intro_video_install(const char *game_dir)
{
    static const uint8_t prologue[] = { 0x51, 0x56, 0x57, 0x83, 0xEC, 0x6C };   /* push ecx/esi/edi; sub esp,0x6c */
    uint8_t jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    int32_t rel;
    snprintf(g_file, sizeof g_file, "%s\\Modernization\\intro.mpg", game_dir);
    if (GetFileAttributesA(g_file) == INVALID_FILE_ATTRIBUTES) {
        log_printf("intro: no %s, the original intro plays", g_file);
        g_file[0] = 0;
        return 0;
    }
    if (!mem_expect(G_FN_INTRO_PLAY_AVI, prologue, sizeof prologue)) {
        log_printf("intro: intro_play_avi has unexpected bytes, original intro");
        g_file[0] = 0;
        return 0;
    }
    /* trampoline: the stolen prologue, then back into the original */
    g_trampoline = (uint8_t *)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline)
        return 0;
    memcpy(g_trampoline, prologue, sizeof prologue);
    g_trampoline[6] = 0xE9;
    rel = (int32_t)((G_FN_INTRO_PLAY_AVI + sizeof prologue) - ((uintptr_t)g_trampoline + 11));
    memcpy(g_trampoline + 7, &rel, 4);
    rel = (int32_t)((uintptr_t)intro_detour - (G_FN_INTRO_PLAY_AVI + 5));
    memcpy(jmp + 1, &rel, 4);
    if (!mem_write(G_FN_INTRO_PLAY_AVI, jmp, sizeof jmp))
        return 0;
    log_printf("intro: native player ready (%s)", g_file);
    return 1;
}
