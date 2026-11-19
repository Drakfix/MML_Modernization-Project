/* The start menu's music: a track from the player's own soundtrack (BGM\MUSICxx.WAV, [general]
 * menu_music), looped through SDL3's audio, faded out when the game starts. Nothing is shipped:
 * without the game's files the start menu stays silent. SDL comes from input.c's library. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input.h"
#include "log.h"
#include "menu_music.h"
#include "volume.h"

typedef struct { int format, channels, freq; } sdl_spec;   /* SDL_AudioSpec */
typedef struct SDL_AudioStream SDL_AudioStream;

static struct {
    unsigned char (*InitSubSystem)(uint32_t);   /* (SDL3's bool: one byte) */
    unsigned char (*LoadWAV)(const char *, sdl_spec *, uint8_t **, uint32_t *);
    SDL_AudioStream *(*OpenAudioDeviceStream)(uint32_t, const sdl_spec *, void *, void *);
    unsigned char (*PutAudioStreamData)(SDL_AudioStream *, const void *, int);
    int (*GetAudioStreamQueued)(SDL_AudioStream *);
    unsigned char (*ResumeAudioStreamDevice)(SDL_AudioStream *);
    unsigned char (*SetAudioStreamGain)(SDL_AudioStream *, float);
    void (*DestroyAudioStream)(SDL_AudioStream *);
    void (*free)(void *);
} sdl;

static SDL_AudioStream *g_stream;
static uint8_t *g_buf;
static uint32_t g_len;
static float g_peak;   /* the track's loudest sample (how far the Music volume can take it up) */

#define SDL_INIT_AUDIO_ 0x10u
#define SDL_DEFAULT_PLAYBACK 0xFFFFFFFFu
#define GAIN 0.7f

void menu_music_start(const char *game_dir, const char *ini)
{
    char name[64], path[MAX_PATH];
    sdl_spec spec;
    GetPrivateProfileStringA("general", "menu_music", "MUSIC30.WAV", name, sizeof name, ini);
    if (!name[0] || _stricmp(name, "off") == 0)
        return;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\BGM\\%s", game_dir, name);
#ifndef _WIN32
    {   /* SDL opens it itself: the path as the system has it */
        char real[MAX_PATH];
        snprintf(path, sizeof path, "%s", compat_path(path, real, sizeof real));
    }
#endif
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return;   /* (no game files yet: silence) */
#define GET(f, n) (*(void **)&sdl.f = input_sdl_proc(n))
    if (!GET(InitSubSystem, "SDL_InitSubSystem") || !GET(LoadWAV, "SDL_LoadWAV") ||
        !GET(OpenAudioDeviceStream, "SDL_OpenAudioDeviceStream") || !GET(PutAudioStreamData, "SDL_PutAudioStreamData") ||
        !GET(GetAudioStreamQueued, "SDL_GetAudioStreamQueued") ||
        !GET(ResumeAudioStreamDevice, "SDL_ResumeAudioStreamDevice") ||
        !GET(SetAudioStreamGain, "SDL_SetAudioStreamGain") || !GET(DestroyAudioStream, "SDL_DestroyAudioStream") ||
        !GET(free, "SDL_free"))
        return;
#undef GET
    if (!sdl.InitSubSystem(SDL_INIT_AUDIO_) || !sdl.LoadWAV(path, &spec, &g_buf, &g_len)) {
        log_printf("menu: music %s not playable", name);
        return;
    }
    g_stream = sdl.OpenAudioDeviceStream(SDL_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_stream) {
        sdl.free(g_buf);
        g_buf = NULL;
        return;
    }
    volume_load(ini);   /* [general] music_volume (the menu changes it live) */
    g_peak = spec.format == 0x8010 ? volume_peak(g_buf, g_len, 16) : spec.format == 0x0008 ? volume_peak(g_buf, g_len, 8) : 0.0f;
    sdl.SetAudioStreamGain(g_stream, GAIN * volume_gain(VOL_MUSIC, g_peak <= 0.0f, g_peak));
    sdl.PutAudioStreamData(g_stream, g_buf, (int)g_len);
    sdl.ResumeAudioStreamDevice(g_stream);
    log_printf("menu: music %s", name);
}

void menu_music_frame(float volume)
{
    if (!g_stream)
        return;
    if (sdl.GetAudioStreamQueued(g_stream) < (int)(g_len / 4))   /* the track again, before it runs out */
        sdl.PutAudioStreamData(g_stream, g_buf, (int)g_len);
    sdl.SetAudioStreamGain(g_stream, GAIN * volume * volume_gain(VOL_MUSIC, g_peak <= 0.0f, g_peak));
}

void menu_music_stop(void)
{
    if (!g_stream)
        return;
    sdl.DestroyAudioStream(g_stream);
    sdl.free(g_buf);
    g_stream = NULL;
    g_buf = NULL;
}
