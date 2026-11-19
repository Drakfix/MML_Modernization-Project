#pragma once
/* Music, sound effect and voice volumes ([general] music_volume, sound_volume, voice_volume: percent).
   See volume.c. */
#include <stdint.h>

enum { VOL_SOUND, VOL_VOICE, VOL_MUSIC, VOL_NCAT };

/* the settings from mml.ini (the start menu's music reads them too) */
void volume_load(const char *ini);
/* the settings, and in the original game the hooks that apply them (once, at startup) */
void volume_install(const char *ini);
/* a setting changed (the menus, on the game thread): applied to what is playing */
void volume_set(int cat, int percent);
int volume_percent(int cat);

/* bumped by every change: the mixer recomputes its gains when it differs from the last it saw */
extern volatile long volume_gen;

/* our DirectSound (shim_dsound.c): which kind of sound the guest buffer object is, when it starts
   playing (the game's music and voice entries hold it then) */
int volume_category(uint32_t buf, int channels);
/* the gain for a sound of that kind: stream = its samples aren't all known (a refilled buffer),
   peak = its loudest sample (0..1; 0 unknown). Above 1 only for the music, as far as its peak
   allows. */
float volume_gain(int cat, int stream, float peak);
/* the loudest sample of 8-bit unsigned or 16-bit signed PCM, 0..1 */
float volume_peak(const void *pcm, uint32_t bytes, int bits);
