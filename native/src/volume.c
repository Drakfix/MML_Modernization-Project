/* Music, sound effect and voice volumes ([general] music_volume, sound_volume, voice_volume: percent;
 * GitHub issue #17). Music 0-400%, effects and voices 0-100%.
 *
 * The game keeps each sound in an entry {part A, part B, buffer A, buffer B} (ambience.c): the music
 * in one (0x55D430: BGM\MUSICxx.WAV, loaded by FUN_004D89E4), the voice in another (0x55D5C0:
 * play_voice FUN_004D8E42), the effects in the sound banks' (0x56220C + slot * 0x10 + bank * 0x808).
 * A voice clip above 512 KB is streamed through a 0x20000-byte buffer (the entry's part A is then -1,
 * buffer B is no buffer); the opening's music is such a stream, in stereo where voices are mono, so a
 * stereo buffer of the voice entry counts as music. Volumes: FUN_004FA436 (EAX entry, EDX 0..127)
 * sets vol / 127 * 10000 - 10000 hundredths of a dB on both buffers; music and voices start at the
 * full volume new buffers have (only fades and effects call it). FUN_004FA2C9 (EAX entry) plays.
 *
 * Above 100% only the music goes, and only as far as each track's loudest sample allows, so it never
 * distorts: the "Unfinished Business" BGM peaks 6 to 24 dB below full scale (City Hall: -20 dB, with
 * the effects at -8), the original tracks close to it. A stream's samples aren't known ahead: 100% at
 * most.
 *
 * Recompiled game (the runner, Android, Linux): our DirectSound mixer (shim_dsound.c) asks
 * volume_category when a buffer starts playing and multiplies its gain by volume_gain.
 *
 * Original game (mml.dll in MEGAMAN.EXE, with Windows' DirectSound or DSOAL): DirectSound plays no
 * louder than the samples, so the music's samples are multiplied in its buffers before they play
 * (and again when the setting changes, from the multiplied ones); the rest is the buffers' volume,
 * the game's plus ours. Hooks in the game's machine code:
 *   0x4FA12B  FUN_004FA12B entry (EAX entry): the entry gets new buffers (full volume, samples as
 *             loaded): forget what we knew about it
 *   0x4FA461  FUN_004FA436 after the volume is computed ([ESP], EBX entry): remember it, add ours
 *   0x4FA2C9  FUN_004FA2C9 entry (EAX entry): multiply the music's samples; our volume on the buffers
 * They aren't in tools/recomp/patch_sites.txt on purpose: the recompiled game has our mixer instead.
 * (A buffer DirectSound loses and the game restores, FUN_004FA1EF, gets its samples back unmultiplied
 * until the next track; DSOAL never loses buffers.) */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"
#include "hook.h"
#include "log.h"
#include "recomp.h"
#include "volume.h"

#define G_MUSIC_ENTRY 0x55D430u   /* .bss: the same in the Japanese program */
#define G_VOICE_ENTRY 0x55D5C0u
#define MUSIC_MAX 400
#define HEADROOM 0.98f            /* the boost leaves the loudest sample this far from full scale */

static volatile int g_pct[VOL_NCAT] = { 100, 100, 100 };
volatile long volume_gen;

static const char *const KEYS[VOL_NCAT] = { "sound_volume", "voice_volume", "music_volume" };

static int clamp_pct(int cat, int p)
{
    int max = cat == VOL_MUSIC ? MUSIC_MAX : 100;
    return p < 0 ? 0 : p > max ? max : p;
}

void volume_load(const char *ini)
{
    int k;
    for (k = 0; k < VOL_NCAT; k++)
        g_pct[k] = clamp_pct(k, (int)GetPrivateProfileIntA("general", KEYS[k], 100, ini));
    InterlockedIncrement(&volume_gen);
}

int volume_percent(int cat)
{
    return cat >= 0 && cat < VOL_NCAT ? g_pct[cat] : 100;
}

float volume_peak(const void *pcm, uint32_t bytes, int bits)
{
    uint32_t i;
    int top = 0;
    if (bits == 16) {
        const int16_t *s = (const int16_t *)pcm;
        for (i = 0; i < bytes / 2; i++) {
            int v = s[i] < 0 ? -(int)s[i] : s[i];
            if (v > top)
                top = v;
        }
        return (float)top / 32768.0f;
    }
    {
        const uint8_t *s = (const uint8_t *)pcm;
        for (i = 0; i < bytes; i++) {
            int v = s[i] < 128 ? 128 - s[i] : s[i] - 128;
            if (v > top)
                top = v;
        }
        return (float)top / 128.0f;
    }
}

float volume_gain(int cat, int stream, float peak)
{
    float g = (float)volume_percent(cat) / 100.0f;
    if (g > 1.0f) {
        if (cat != VOL_MUSIC || stream || peak <= 0.0f) {
            g = 1.0f;
        } else {
            float cap = HEADROOM / peak;
            if (cap < 1.0f)
                cap = 1.0f;
            if (g > cap)
                g = cap;
        }
    }
    return g;
}

int volume_category(uint32_t buf, int channels)
{
    if (!buf)
        return VOL_SOUND;
    if (buf == GVAR(uint32_t, G_MUSIC_ENTRY + 8) || buf == GVAR(uint32_t, G_MUSIC_ENTRY + 12))
        return VOL_MUSIC;
    if (buf == GVAR(uint32_t, G_VOICE_ENTRY + 8) ||
        (GVAR(uint32_t, G_VOICE_ENTRY) != 0xFFFFFFFFu && buf == GVAR(uint32_t, G_VOICE_ENTRY + 12)))
        return channels == 2 ? VOL_MUSIC : VOL_VOICE;
    return VOL_SOUND;
}

/* ---- the original game: DirectSound buffers through their vtables ---- */
#ifndef MML_RUNNER

#include "recomp_int.h"   /* recomp_select */

#define G_FN_CREATE    GA(0x4FA12Bu)   /* push ebx; push ecx; push esi; sub esp,4 */
#define G_SITE_VOLUME  GA(0x4FA461u)   /* cmp dword [0x551FB0],0 (after fistp [esp]) */
#define G_FN_PLAY      GA(0x4FA2C9u)   /* push ebx; push ecx; push edx; push esi; mov ebx,eax */
#define G_SOUND_ON     GA(0x551FB0u)   /* the game's DirectSound object (0: no sound) */

typedef HRESULT(WINAPI *ds_get_format)(void *, WAVEFORMATEX *, DWORD, DWORD *);
typedef HRESULT(WINAPI *ds_lock)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD);
typedef HRESULT(WINAPI *ds_set_volume)(void *, LONG);
typedef HRESULT(WINAPI *ds_unlock)(void *, void *, DWORD, void *, DWORD);
#define VT(obj, off) ((*(void ***)(obj))[(off) / 4])   /* IDirectSoundBuffer: GetFormat 0x14, Lock 0x2C, SetVolume 0x3C, Unlock 0x4C */
#define DSBLOCK_ENTIREBUFFER_ 0x2u

static int g_hooked;
static int g_log;   /* [debug] volume_log=1: every volume of ours and the music's scaling */

/* the game's own volume of each entry (hundredths of a dB, 0 = full): open addressing on the
   entry's address (entries are 16 bytes apart) */
#define NENT 2048
static struct { uint32_t entry; int32_t vol; } g_ent[NENT];

static int ent_slot(uint32_t e, int add)
{
    uint32_t h = (e >> 4) & (NENT - 1), k;
    for (k = 0; k < NENT; k++, h = (h + 1) & (NENT - 1)) {
        if (g_ent[h].entry == e)
            return (int)h;
        if (!g_ent[h].entry) {
            if (!add)
                return -1;
            g_ent[h].entry = e;
            g_ent[h].vol = 0;
            return (int)h;
        }
    }
    return -1;
}

static int32_t game_vol(uint32_t e)
{
    int h = ent_slot(e, 0);
    return h < 0 ? 0 : g_ent[h].vol;
}

/* the music entry's buffers: how much their samples are multiplied by, their loudest sample before */
static struct { uint32_t buf; float scale, peak; int bits; } g_mus[2];
static float g_mus_scale = 1.0f;   /* the factor both have (one per entry: one volume for both) */
static int g_voice_stereo = -1;    /* the voice entry's buffer A: stereo (the opening's music)? -1 not known */

static int buf_format(uint32_t buf, WAVEFORMATEX *wf)
{
    DWORD got = 0;
    memset(wf, 0, sizeof *wf);
    return ((ds_get_format)VT(GUEST(buf), 0x14))(GUEST(buf), wf, sizeof *wf, &got) == S_OK;
}

/* every sample of the buffer times f (clipped), or only its peak (f == 0) */
static int buf_samples(uint32_t buf, int bits, float f, float *peak)
{
    void *p1 = NULL, *p2 = NULL;
    DWORD n1 = 0, n2 = 0, i;
    if (((ds_lock)VT(GUEST(buf), 0x2C))(GUEST(buf), 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER_) != S_OK || !p1)
        return 0;
    if (f == 0.0f) {
        *peak = volume_peak(p1, n1, bits);
    } else if (bits == 16) {
        int16_t *s = (int16_t *)p1;
        for (i = 0; i < n1 / 2; i++) {
            float v = (float)s[i] * f;
            s[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)lrintf(v));
        }
    } else {
        uint8_t *s = (uint8_t *)p1;
        for (i = 0; i < n1; i++) {
            float v = ((float)s[i] - 128.0f) * f;
            s[i] = (uint8_t)(v > 127.0f ? 255 : v < -128.0f ? 0 : (int)lrintf(v) + 128);
        }
    }
    ((ds_unlock)VT(GUEST(buf), 0x4C))(GUEST(buf), p1, n1, p2, n2);
    return 1;
}

/* the music's samples multiplied as far as the setting asks and its peak allows */
static void music_samples(void)
{
    float want = 1.0f, f;
    int k;
    for (k = 0; k < 2; k++) {
        uint32_t buf = GVAR(uint32_t, G_MUSIC_ENTRY + 8 + 4u * (uint32_t)k);
        if (k == 1 && GVAR(uint32_t, G_MUSIC_ENTRY) == 0xFFFFFFFFu)
            buf = 0;   /* (a stream: buffer B is no buffer; the music never is one) */
        if (buf != g_mus[k].buf) {   /* (a buffer of an earlier track: the create hook forgot it) */
            WAVEFORMATEX wf;
            g_mus[k].buf = buf;
            g_mus[k].peak = 0.0f;
            g_mus[k].scale = 1.0f;
            g_mus[k].bits = buf && buf_format(buf, &wf) ? wf.wBitsPerSample : 0;
            if (g_mus[k].bits == 8 || g_mus[k].bits == 16)
                buf_samples(buf, g_mus[k].bits, 0.0f, &g_mus[k].peak);
            else
                g_mus[k].buf = 0;   /* (no buffer, or a format we leave alone) */
        }
    }
    if (volume_percent(VOL_MUSIC) > 100) {
        float peak = g_mus[0].peak > g_mus[1].peak ? g_mus[0].peak : g_mus[1].peak;
        want = volume_gain(VOL_MUSIC, 0, peak);
    }
    for (k = 0; k < 2; k++) {
        if (!g_mus[k].buf || fabsf(g_mus[k].scale - want) < 0.001f)
            continue;
        f = want / g_mus[k].scale;
        if (buf_samples(g_mus[k].buf, g_mus[k].bits, f, NULL)) {
            g_mus[k].scale = want;
            log_printf("volume: music samples x%.2f (peak %.3f)", want, g_mus[k].peak);
        }
    }
    g_mus_scale = g_mus[0].buf ? g_mus[0].scale : g_mus[1].buf ? g_mus[1].scale : 1.0f;
}

/* ours on top of the game's volume v (hundredths of a dB), for the entry */
static int32_t adjusted(uint32_t e, int32_t v)
{
    float g;
    int32_t out;
    if (e == G_MUSIC_ENTRY) {
        g = (float)volume_percent(VOL_MUSIC) / 100.0f / g_mus_scale;
    } else if (e == G_VOICE_ENTRY) {
        if (g_voice_stereo < 0) {
            WAVEFORMATEX wf;
            uint32_t buf = GVAR(uint32_t, e + 8);
            g_voice_stereo = buf && buf_format(buf, &wf) ? wf.nChannels == 2 : -1;
        }
        g = volume_gain(g_voice_stereo == 1 ? VOL_MUSIC : VOL_VOICE, 1, 0.0f);
    } else {
        g = volume_gain(VOL_SOUND, 1, 0.0f);
    }
    if (g > 1.0f)
        g = 1.0f;
    if (g >= 0.999f)
        return v;
    out = g < 0.0001f ? -10000 : v + (int32_t)lrintf(2000.0f * log10f(g));
    return out < -10000 ? -10000 : out > 0 ? 0 : out;
}

/* ours on the entry's buffers (force: also where it adds nothing, to undo an earlier one) */
static void apply(uint32_t e, int force)
{
    int32_t v = game_vol(e), a = adjusted(e, v);
    uint32_t b;
    if (a == v && !force)
        return;
    if (g_log)
        log_printf("volume: entry %X: %d + ours = %d", e, v, a);
    b = GVAR(uint32_t, e + 8);
    if (b)
        ((ds_set_volume)VT(GUEST(b), 0x3C))(GUEST(b), a);
    b = GVAR(uint32_t, e + 12);
    if (b && GVAR(uint32_t, e) != 0xFFFFFFFFu)
        ((ds_set_volume)VT(GUEST(b), 0x3C))(GUEST(b), a);
}

static void on_create(regs_t *r)
{
    int h;
    if (!g_hooked)
        return;
    h = ent_slot(r->eax, 0);
    if (h >= 0)
        g_ent[h].vol = 0;
    if (r->eax == G_MUSIC_ENTRY) {
        g_mus[0].buf = g_mus[1].buf = 0;
        g_mus_scale = 1.0f;
    } else if (r->eax == G_VOICE_ENTRY) {
        g_voice_stereo = -1;
    }
}

static void on_volume(regs_t *r)
{
    int32_t *v = (int32_t *)GUEST(r->esp + 4);   /* the game's [esp] */
    int h;
    if (!g_hooked)
        return;
    h = ent_slot(r->ebx, 1);
    if (h >= 0)
        g_ent[h].vol = *v;
    if (g_log && (r->ebx == G_MUSIC_ENTRY || r->ebx == G_VOICE_ENTRY) && adjusted(r->ebx, *v) != *v)   /* (effects: too many) */
        log_printf("volume: entry %X set to %d, with ours %d", r->ebx, *v, adjusted(r->ebx, *v));
    *v = adjusted(r->ebx, *v);
}

static void on_play(regs_t *r)
{
    if (!g_hooked || !GVAR(uint32_t, G_SOUND_ON))
        return;
    if (r->eax == G_MUSIC_ENTRY)
        music_samples();
    apply(r->eax, 0);
}

void volume_set(int cat, int percent)
{
    int h;
    if (cat < 0 || cat >= VOL_NCAT)
        return;
    g_pct[cat] = clamp_pct(cat, percent);
    InterlockedIncrement(&volume_gen);
    if (!g_hooked || !GVAR(uint32_t, G_SOUND_ON))
        return;
    /* what is playing: the music's samples, then every entry the game gave a volume or plays */
    music_samples();
    ent_slot(G_MUSIC_ENTRY, 1);
    ent_slot(G_VOICE_ENTRY, 1);
    for (h = 0; h < NENT; h++)
        if (g_ent[h].entry)
            apply(g_ent[h].entry, 1);
}

void volume_install(const char *ini)
{
    static const uint8_t create[6] = { 0x53, 0x51, 0x56, 0x83, 0xEC, 0x04 };
    static const uint8_t vol[7] = { 0x83, 0x3D, 0xB0, 0x1F, 0x55, 0x00, 0x00 };
    static const uint8_t play[6] = { 0x53, 0x51, 0x52, 0x56, 0x89, 0xC3 };
    char mode[16];
    volume_load(ini);
    log_printf("volume: music %d%%, effects %d%%, voices %d%%", g_pct[VOL_MUSIC], g_pct[VOL_SOUND], g_pct[VOL_VOICE]);
    g_log = (int)GetPrivateProfileIntA("debug", "volume_log", 0, ini);
    /* the recompiled game in mml.dll ([debug] recomp) plays through our mixer; recomp_install
       (later) decides the same way, and runs the machine code for a program it has no code for */
    GetPrivateProfileStringA("debug", "recomp", "off", mode, sizeof mode, ini);
    if ((_stricmp(mode, "replace") == 0 || _stricmp(mode, "diff") == 0) && !(addr_game == GAME_JP && !recomp_select()))
        return;
    if (!hook_mid(G_FN_CREATE, sizeof create, create, on_create) || !hook_mid(G_SITE_VOLUME, sizeof vol, vol, on_volume) ||
        !hook_mid(G_FN_PLAY, sizeof play, play, on_play)) {
        log_printf("volume: sound hooks failed, the volume settings do nothing");   /* (the ones in place do nothing) */
        return;
    }
    g_hooked = 1;
}

#else   /* the runner: our mixer does it all */

void volume_set(int cat, int percent)
{
    if (cat < 0 || cat >= VOL_NCAT)
        return;
    g_pct[cat] = clamp_pct(cat, percent);
    InterlockedIncrement(&volume_gen);
}

void volume_install(const char *ini)
{
    volume_load(ini);
    log_printf("volume: music %d%%, effects %d%%, voices %d%%", g_pct[VOL_MUSIC], g_pct[VOL_SOUND], g_pct[VOL_VOICE]);
}

#endif
