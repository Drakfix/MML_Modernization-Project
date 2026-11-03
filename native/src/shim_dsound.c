/* DirectSound for the recompiled game, mixed on an SDL3 audio stream.
 *
 * What the game uses (FUN_004F9AD6..FUN_004FA8A3): DirectSoundCreate, SetCooperativeLevel, a
 * primary buffer for SetFormat (22050 Hz, 16-bit stereo), and secondary buffers made from WAV
 * data: static ones (sound effects; music in two parts, the intro part with a notification
 * 0x200 bytes before its end that starts the looping part) and one 0x20000-byte looping stream
 * refilled half by half from notifications at 0 and 0x10000. Buffer methods: Lock / Unlock,
 * Play, Stop, SetCurrentPosition, SetVolume, SetPan, GetStatus, Restore, Release, and
 * QueryInterface for IDirectSoundNotify (SetNotificationPositions).
 *
 * Behaviour copied from Windows' DirectSound, measured with the scratchpad test dstest (RE-notes
 * 7.34): a notification at offset 0 fires when looping playback starts; a one-shot that reaches
 * its end stops with its position back at 0; Stop keeps the position; SetCurrentPosition works
 * while playing.
 *
 * The objects live in guest memory ({vtable, our index}); their methods are shim methods. The
 * mixer runs on SDL's audio thread under one mutex; the game writes sample data between Lock
 * and Unlock without it, as it does with Windows' (the play cursor is elsewhere). */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "input.h"
#include "log.h"
#include "recomp_rt.h"
#include "shim.h"

#define DS_OK              0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_NODRIVER     0x88780078u
#define DSERR_OUTOFMEMORY  0x8007000Eu
#define E_NOINTERFACE_     0x80004002u
#define DSERR_UNSUPPORTED  0x80004001u

#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBPLAY_LOOPING       0x1u
#define DSBSTATUS_PLAYING     0x1u
#define DSBSTATUS_LOOPING     0x4u
#define DSBPN_OFFSETSTOP      0xFFFFFFFFu

#define OUT_RATE 48000

/* SDL3, from the SDL3.dll input.c loaded */
typedef struct { uint32_t format; int channels; int freq; } sdl_audio_spec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef struct SDL_Mutex SDL_Mutex;
typedef void(__cdecl *sdl_stream_cb)(void *user, SDL_AudioStream *s, int additional, int total);
static struct {
    unsigned char(__cdecl *InitSubSystem)(uint32_t);
    SDL_AudioStream *(__cdecl *OpenAudioDeviceStream)(uint32_t, const sdl_audio_spec *, sdl_stream_cb, void *);
    unsigned char(__cdecl *PutAudioStreamData)(SDL_AudioStream *, const void *, int);
    unsigned char(__cdecl *ResumeAudioStreamDevice)(SDL_AudioStream *);
    SDL_Mutex *(__cdecl *CreateMutex)(void);
    void(__cdecl *LockMutex)(SDL_Mutex *);
    void(__cdecl *UnlockMutex)(SDL_Mutex *);
    const char *(__cdecl *GetError)(void);
} sa;
#define SDL_INIT_AUDIO_ 0x10u
#define SDL_AUDIO_F32_  0x8120u
#define SDL_DEFAULT_PLAYBACK 0xFFFFFFFFu

static SDL_AudioStream *g_stream;
static SDL_Mutex *g_mutex;

/* [debug] dsound_record=N: the first N seconds of the mix to dsound_record.wav (checking) */
static int16_t *g_rec;
static uint32_t g_rec_frames, g_rec_used;
static char g_rec_path[MAX_PATH];
static int g_log;   /* [debug] dsound_log=1: every buffer call but the volume and pan ones */
extern int g_sync_log;   /* shim_sync.c: and every event a wait returns */
#define DLOG(...) do { if (g_log) log_printf(__VA_ARGS__); } while (0)

static void record(const float *mix, int frames)
{
    int k;
    if (!g_rec || g_rec_used >= g_rec_frames)
        return;
    for (k = 0; k < frames && g_rec_used < g_rec_frames; k++, g_rec_used++) {
        g_rec[2 * g_rec_used] = (int16_t)(mix[2 * k] * 32767.0f);
        g_rec[2 * g_rec_used + 1] = (int16_t)(mix[2 * k + 1] * 32767.0f);
    }
    if (g_rec_used == g_rec_frames) {
        FILE *f = fopen(g_rec_path, "wb");
        uint32_t data = g_rec_frames * 4, v;
        if (f) {
            fwrite("RIFF", 1, 4, f);
            v = 36 + data;
            fwrite(&v, 4, 1, f);
            fwrite("WAVEfmt ", 1, 8, f);
            v = 16;
            fwrite(&v, 4, 1, f);
            v = 0x00020001u;   /* PCM, 2 channels */
            fwrite(&v, 4, 1, f);
            v = OUT_RATE;
            fwrite(&v, 4, 1, f);
            v = OUT_RATE * 4;
            fwrite(&v, 4, 1, f);
            v = 0x00100004u;   /* align 4, 16 bits */
            fwrite(&v, 4, 1, f);
            fwrite("data", 1, 4, f);
            fwrite(&data, 4, 1, f);
            fwrite(g_rec, 4, g_rec_frames, f);
            fclose(f);
        }
        log_printf("dsound: %u s of the mix recorded to %s", g_rec_frames / OUT_RATE, g_rec_path);
    }
}

#define MAX_NOTIFY 8
typedef struct {
    int used, refs, primary;
    uint32_t obj, notify_obj;     /* guest objects: the buffer, its IDirectSoundNotify */
    uint32_t mem, size;           /* guest sample memory */
    int channels, bits, rate, align;
    int playing, looping;
    uint64_t pos;                 /* frames, 32.32 fixed point */
    int32_t volume, pan;          /* DirectSound units (hundredths of a dB) */
    float gain_l, gain_r;
    int nnotify;
    uint32_t notify_at[MAX_NOTIFY], notify_ev[MAX_NOTIFY];
} dsbuf;

#define MAX_BUFFERS 128
static dsbuf g_buf[MAX_BUFFERS];
static uint32_t g_vt_ds, g_vt_buf, g_vt_notify;   /* guest vtables */
static uint32_t g_ds_obj;
static int g_ds_refs;

static uint32_t get32(uint32_t g)
{
    uint32_t v;
    memcpy(&v, HPTR(g), 4);
    return v;
}

static void put32(uint32_t g, uint32_t v)
{
    memcpy(HPTR(g), &v, 4);
}

static void lock(void)
{
    sa.LockMutex(g_mutex);
}

static void unlock(void)
{
    sa.UnlockMutex(g_mutex);
}

/* a guest object: {vtable, our index} */
static uint32_t new_object(uint32_t vt, uint32_t index)
{
    uint32_t o = guest_alloc(8);
    if (o) {
        put32(o, vt);
        put32(o + 4, index);
    }
    return o;
}

static dsbuf *buf_of(uint32_t obj)
{
    uint32_t i = get32(obj + 4);
    return i < MAX_BUFFERS && g_buf[i].used ? &g_buf[i] : NULL;
}

static void set_gains(dsbuf *b)
{
    float g = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    b->gain_l = b->gain_r = g;
    if (b->pan > 0)
        b->gain_l *= b->pan >= 10000 ? 0.0f : powf(10.0f, -(float)b->pan / 2000.0f);
    else if (b->pan < 0)
        b->gain_r *= b->pan <= -10000 ? 0.0f : powf(10.0f, (float)b->pan / 2000.0f);
}

/* ---- the mixer (SDL's audio thread, under the mutex) ---- */

static float sample_at(const dsbuf *b, const uint8_t *mem, uint32_t frame, int ch)
{
    const uint8_t *p = mem + (size_t)frame * (size_t)b->align + (size_t)(ch * (b->bits / 8));
    if (b->bits == 8)
        return ((float)p[0] - 128.0f) / 128.0f;
    {
        int16_t v;
        memcpy(&v, p, 2);
        return (float)v / 32768.0f;
    }
}

/* the notifications for byte offsets in [from, from + n), wrapping in the buffer */
static void notify_span(const dsbuf *b, uint32_t from, uint32_t n)
{
    int k;
    for (k = 0; k < b->nnotify; k++) {
        uint32_t at = b->notify_at[k];
        if (at == DSBPN_OFFSETSTOP)
            continue;
        if (n >= b->size || (at >= from ? at - from : at + b->size - from) < n)
            shim_event_set(b->notify_ev[k]);
    }
}

static void notify_stop(const dsbuf *b)
{
    int k;
    for (k = 0; k < b->nnotify; k++)
        if (b->notify_at[k] == DSBPN_OFFSETSTOP)
            shim_event_set(b->notify_ev[k]);
}

static void mix_buffer(dsbuf *b, float *out, int frames)
{
    const uint8_t *mem = HPTR(b->mem);
    uint32_t nframes = b->size / (uint32_t)b->align;
    uint64_t step = ((uint64_t)(uint32_t)b->rate << 32) / OUT_RATE, end = (uint64_t)nframes << 32;
    uint32_t start_byte = (uint32_t)(b->pos >> 32) * (uint32_t)b->align;
    uint64_t advanced = 0;
    int i, stereo = b->channels == 2;
    if (!nframes)
        return;
    for (i = 0; i < frames; i++) {
        uint32_t f0 = (uint32_t)(b->pos >> 32), f1 = f0 + 1;
        float t = (float)(uint32_t)b->pos * (1.0f / 4294967296.0f), l, r;
        if (f1 >= nframes)
            f1 = b->looping ? 0 : f0;
        l = sample_at(b, mem, f0, 0);
        l += (sample_at(b, mem, f1, 0) - l) * t;
        if (stereo) {
            r = sample_at(b, mem, f0, 1);
            r += (sample_at(b, mem, f1, 1) - r) * t;
        } else {
            r = l;
        }
        out[2 * i] += l * b->gain_l;
        out[2 * i + 1] += r * b->gain_r;
        b->pos += step;
        advanced += step;
        if (b->pos >= end) {
            if (b->looping) {
                b->pos -= end;
            } else {   /* the end: stopped, back at the start (as Windows does) */
                notify_span(b, start_byte, b->size - start_byte);
                b->playing = 0;
                b->pos = 0;
                notify_stop(b);
                return;
            }
        }
    }
    /* from the frame the chunk started on to the one it ended on (whole buffer if it went round) */
    if ((advanced >> 32) >= nframes)
        notify_span(b, start_byte, b->size);
    else
        notify_span(b, start_byte, ((uint32_t)(b->pos >> 32) * (uint32_t)b->align + b->size - start_byte) % b->size);
}

static void __cdecl on_audio(void *user, SDL_AudioStream *s, int additional, int total)
{
    static float mix[2 * 4096];
    (void)user;
    (void)total;
    while (additional > 0) {
        int frames = additional / 8, k;
        if (frames > 4096)
            frames = 4096;
        if (frames <= 0)
            break;
        memset(mix, 0, sizeof(float) * 2 * (size_t)frames);
        lock();
        if (g_log) {   /* every ~5 s: what is playing */
            static uint32_t since;
            since += (uint32_t)frames;
            if (since >= 5 * OUT_RATE) {
                char line[512];
                size_t m = 0;
                since = 0;
                line[0] = 0;
                for (k = 0; k < MAX_BUFFERS && m < sizeof line - 40; k++)
                    if (g_buf[k].used && g_buf[k].playing && !g_buf[k].primary)
                        m += (size_t)_snprintf_s(line + m, sizeof line - m, _TRUNCATE, " %d(vol %d pan %d)", k,
                                                 g_buf[k].volume, g_buf[k].pan);
                log_printf("dsound: mixing:%s", m ? line : " nothing");
            }
        }
        for (k = 0; k < MAX_BUFFERS; k++)
            if (g_buf[k].used && g_buf[k].playing && !g_buf[k].primary)
                mix_buffer(&g_buf[k], mix, frames);
        unlock();
        for (k = 0; k < 2 * frames; k++)
            mix[k] = mix[k] > 1.0f ? 1.0f : mix[k] < -1.0f ? -1.0f : mix[k];
        record(mix, frames);
        sa.PutAudioStreamData(s, mix, frames * 8);
        additional -= frames * 8;
    }
}

/* ---- IUnknown parts ---- */

static uint32_t m_unsupported(const uint32_t *a)
{
    (void)a;
    log_printf("dsound: a method the game wasn't seen to use was called");
    return DSERR_UNSUPPORTED;
}

static uint32_t m_ok(const uint32_t *a)
{
    (void)a;
    return DS_OK;
}

/* ---- IDirectSoundBuffer ---- */

static void buf_free(dsbuf *b)
{
    lock();
    b->playing = 0;
    b->used = 0;
    unlock();
    if (b->mem)
        guest_free(b->mem);
    if (b->notify_obj)
        guest_free(b->notify_obj);
    guest_free(b->obj);
    memset(b, 0, sizeof *b);
}

static const uint8_t IID_IDirectSoundNotify_[16] = { 0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11,
                                                     0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 };

/* QueryInterface(this, iid, out) */
static uint32_t b_query_interface(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b || memcmp(HPTR(a[1]), IID_IDirectSoundNotify_, 16) != 0) {
        if (a[2])
            put32(a[2], 0);
        return E_NOINTERFACE_;
    }
    if (!b->notify_obj)
        b->notify_obj = new_object(g_vt_notify, get32(a[0] + 4));
    b->refs++;   /* the notify interface holds the buffer */
    put32(a[2], b->notify_obj);
    return DS_OK;
}

static uint32_t b_add_ref(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    return b ? (uint32_t)++b->refs : 0;
}

static uint32_t b_release(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b)
        return 0;
    if (--b->refs > 0)
        return (uint32_t)b->refs;
    DLOG("dsound: release %d", (int)(b - g_buf));
    buf_free(b);
    return 0;
}

/* GetCurrentPosition(this, play, write) */
static uint32_t b_get_current_position(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    uint32_t play;
    if (!b)
        return DSERR_INVALIDPARAM;
    lock();
    play = (uint32_t)(b->pos >> 32) * (uint32_t)b->align;
    unlock();
    if (a[1])
        put32(a[1], play);
    if (a[2])   /* the write cursor: ~10 ms ahead while playing */
        put32(a[2], b->size ? (play + (b->playing ? (uint32_t)(b->rate / 100 * b->align) : 0)) % b->size : 0);
    return DS_OK;
}

/* GetStatus(this, status) */
static uint32_t b_get_status(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    uint32_t st = 0;
    if (!b)
        return DSERR_INVALIDPARAM;
    lock();
    if (b->playing)
        st = DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0);
    unlock();
    put32(a[1], st);
    return DS_OK;
}

/* GetVolume / GetPan (this, out) */
static uint32_t b_get_volume(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b)
        return DSERR_INVALIDPARAM;
    put32(a[1], (uint32_t)b->volume);
    return DS_OK;
}

static uint32_t b_get_pan(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b)
        return DSERR_INVALIDPARAM;
    put32(a[1], (uint32_t)b->pan);
    return DS_OK;
}

static uint32_t b_get_frequency(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b)
        return DSERR_INVALIDPARAM;
    put32(a[1], (uint32_t)b->rate);
    return DS_OK;
}

/* Lock(this, offset, bytes, p1, n1, p2, n2, flags) */
static uint32_t b_lock(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    uint32_t off = a[1], n = a[2];
    if (!b || b->primary || !b->size)
        return DSERR_INVALIDPARAM;
    if (a[7] & 2)   /* DSBLOCK_ENTIREBUFFER */
        off = 0, n = b->size;
    else if (a[7] & 1)
        log_printf("dsound: Lock from the write cursor (not seen in the game)");
    if (off >= b->size || n > b->size)
        return DSERR_INVALIDPARAM;
    DLOG("dsound: lock %d %X+%X", (int)(b - g_buf), off, n);
    put32(a[3], b->mem + off);
    if (off + n <= b->size) {
        put32(a[4], n);
        if (a[5])
            put32(a[5], 0);
        if (a[6])
            put32(a[6], 0);
    } else {
        put32(a[4], b->size - off);
        if (a[5])
            put32(a[5], b->mem);
        if (a[6])
            put32(a[6], off + n - b->size);
    }
    return DS_OK;
}

/* Play(this, reserved, priority, flags) */
static uint32_t b_play(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b)
        return DSERR_INVALIDPARAM;
    lock();
    b->looping = (a[3] & DSBPLAY_LOOPING) != 0;
    b->playing = 1;
    unlock();
    DLOG("dsound: play %d%s at %u", (int)(b - g_buf), b->looping ? " looping" : "", (uint32_t)(b->pos >> 32) * (uint32_t)b->align);
    return DS_OK;
}

/* SetCurrentPosition(this, offset) */
static uint32_t b_set_current_position(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b || b->primary || (b->size && a[1] >= b->size))
        return DSERR_INVALIDPARAM;
    lock();
    b->pos = (uint64_t)(a[1] / (uint32_t)b->align) << 32;
    unlock();
    DLOG("dsound: position %d = %u", (int)(b - g_buf), a[1]);
    return DS_OK;
}

/* SetFormat(this, WAVEFORMATEX): only the primary buffer's, which only sets the mix format */
static uint32_t b_set_format(const uint32_t *a)
{
    (void)a;
    return DS_OK;
}

static uint32_t b_set_volume(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b || (int32_t)a[1] > 0 || (int32_t)a[1] < -10000)
        return DSERR_INVALIDPARAM;
    lock();
    b->volume = (int32_t)a[1];
    set_gains(b);
    unlock();
    DLOG("dsound: volume %d = %d", (int)(b - g_buf), (int32_t)a[1]);
    return DS_OK;
}

static uint32_t b_set_pan(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b || (int32_t)a[1] > 10000 || (int32_t)a[1] < -10000)
        return DSERR_INVALIDPARAM;
    lock();
    b->pan = (int32_t)a[1];
    set_gains(b);
    unlock();
    return DS_OK;
}

static uint32_t b_set_frequency(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b)
        return DSERR_INVALIDPARAM;
    lock();
    if (a[1] >= 100 && a[1] <= 200000)
        b->rate = (int)a[1];
    unlock();
    return DS_OK;
}

static uint32_t b_stop(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    if (!b)
        return DSERR_INVALIDPARAM;
    lock();
    if (b->playing) {
        b->playing = 0;
        notify_stop(b);
    }
    unlock();
    DLOG("dsound: stop %d", (int)(b - g_buf));
    return DS_OK;
}

/* ---- IDirectSoundNotify ---- */

static uint32_t n_query_interface(const uint32_t *a)
{
    if (a[2])
        put32(a[2], 0);
    return E_NOINTERFACE_;
}

static uint32_t n_add_ref(const uint32_t *a)
{
    return b_add_ref(a);   /* the notify object carries the buffer's index */
}

static uint32_t n_release(const uint32_t *a)
{
    return b_release(a);
}

/* SetNotificationPositions(this, count, DSBPOSITIONNOTIFY[] {offset, event}) */
static uint32_t n_set_notification_positions(const uint32_t *a)
{
    dsbuf *b = buf_of(a[0]);
    uint32_t k;
    if (!b || a[1] > MAX_NOTIFY)
        return DSERR_INVALIDPARAM;
    lock();
    b->nnotify = (int)a[1];
    for (k = 0; k < a[1]; k++) {
        b->notify_at[k] = get32(a[2] + 8 * k);
        b->notify_ev[k] = get32(a[2] + 8 * k + 4);
        DLOG("dsound: notify %d at %X -> %08X", (int)(b - g_buf), b->notify_at[k], b->notify_ev[k]);
    }
    unlock();
    return DS_OK;
}

/* ---- IDirectSound ---- */

static uint32_t d_query_interface(const uint32_t *a)
{
    if (a[2])
        put32(a[2], 0);
    return E_NOINTERFACE_;
}

static uint32_t d_add_ref(const uint32_t *a)
{
    (void)a;
    return (uint32_t)++g_ds_refs;
}

static uint32_t d_release(const uint32_t *a)
{
    (void)a;
    if (g_ds_refs > 0)
        g_ds_refs--;
    return (uint32_t)g_ds_refs;   /* the device stays open: the game makes one */
}

/* CreateSoundBuffer(this, DSBUFFERDESC {size, flags, bytes, reserved, format}, out, outer) */
static uint32_t d_create_sound_buffer(const uint32_t *a)
{
    uint32_t desc = a[1], flags = get32(desc + 4), bytes = get32(desc + 8), wfx = get32(desc + 16);
    int i;
    dsbuf *b;
    for (i = 0; i < MAX_BUFFERS && g_buf[i].used; i++)
        ;
    if (i == MAX_BUFFERS) {
        log_printf("dsound: out of buffers");
        return DSERR_OUTOFMEMORY;
    }
    b = &g_buf[i];
    memset(b, 0, sizeof *b);
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    if (!b->primary) {
        uint16_t tag, ch, align, bits;
        uint32_t rate;
        if (!wfx || !bytes)
            return DSERR_INVALIDPARAM;
        memcpy(&tag, HPTR(wfx), 2);
        memcpy(&ch, HPTR(wfx + 2), 2);
        memcpy(&rate, HPTR(wfx + 4), 4);
        memcpy(&align, HPTR(wfx + 12), 2);
        memcpy(&bits, HPTR(wfx + 14), 2);
        if (tag != 1 || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || align != ch * bits / 8 || !rate) {
            log_printf("dsound: format %u, %u channels, %u bits, %u Hz not supported", tag, ch, bits, rate);
            return DSERR_INVALIDPARAM;
        }
        b->channels = ch, b->bits = bits, b->rate = (int)rate, b->align = align;
        b->size = bytes - bytes % align;
        b->mem = guest_alloc(b->size);
        if (!b->mem)
            return DSERR_OUTOFMEMORY;
    }
    b->refs = 1;
    b->obj = new_object(g_vt_buf, (uint32_t)i);
    set_gains(b);
    lock();
    b->used = 1;
    unlock();
    DLOG("dsound: buffer %d: %s%u bytes, %d ch, %d bits, %d Hz, flags %X", i, b->primary ? "primary " : "", b->size,
         b->channels, b->bits, b->rate, flags);
    put32(a[2], b->obj);
    return DS_OK;
}

/* GetCaps(this, DSCAPS): not used by the game; zeroed past the size field */
static uint32_t d_get_caps(const uint32_t *a)
{
    uint32_t n = get32(a[1]);
    if (n > 4)
        memset(HPTR(a[1] + 4), 0, n - 4);
    return DS_OK;
}

/* GetSpeakerConfig(this, out) */
static uint32_t d_get_speaker_config(const uint32_t *a)
{
    put32(a[1], 4);   /* DSSPEAKER_STEREO */
    return DS_OK;
}

/* DirectSoundCreate(guid, out, outer) */
static uint32_t sh_direct_sound_create(const uint32_t *a)
{
    if (!g_stream) {
        sdl_audio_spec spec;
        if (!sa.InitSubSystem(SDL_INIT_AUDIO_)) {
            log_printf("dsound: SDL audio failed (%s)", sa.GetError());
            put32(a[1], 0);
            return DSERR_NODRIVER;
        }
        spec.format = SDL_AUDIO_F32_;
        spec.channels = 2;
        spec.freq = OUT_RATE;
        g_stream = sa.OpenAudioDeviceStream(SDL_DEFAULT_PLAYBACK, &spec, on_audio, NULL);
        if (!g_stream) {
            log_printf("dsound: no audio device (%s)", sa.GetError());
            put32(a[1], 0);
            return DSERR_NODRIVER;
        }
        sa.ResumeAudioStreamDevice(g_stream);
        log_printf("dsound: SDL audio open (%d Hz stereo)", OUT_RATE);
    }
    if (!g_ds_obj)
        g_ds_obj = new_object(g_vt_ds, 0);
    g_ds_refs++;
    put32(a[1], g_ds_obj);
    return DS_OK;
}

static uint32_t vtable(const shim_fn *fn, const int *nargs, int n)
{
    uint32_t vt = guest_alloc(4 * (size_t)n);
    int k;
    for (k = 0; k < n && vt; k++)
        put32(vt + 4 * (uint32_t)k, shim_method(nargs[k], fn[k]));
    return vt;
}

int shim_dsound_install(const char *ini, const char *game_dir)
{
    static const shim_fn ds_fn[11] = { d_query_interface, d_add_ref, d_release, d_create_sound_buffer, d_get_caps,
                                       m_unsupported, m_ok, m_ok, d_get_speaker_config, m_ok, m_ok };
    static const int ds_n[11] = { 3, 1, 1, 4, 2, 3, 3, 1, 2, 2, 2 };
    /* QueryInterface AddRef Release GetCaps GetCurrentPosition GetFormat GetVolume GetPan
       GetFrequency GetStatus Initialize Lock Play SetCurrentPosition SetFormat SetVolume SetPan
       SetFrequency Stop Unlock Restore */
    static const shim_fn buf_fn[21] = { b_query_interface, b_add_ref, b_release, m_unsupported, b_get_current_position,
                                        m_unsupported, b_get_volume, b_get_pan, b_get_frequency, b_get_status, m_ok,
                                        b_lock, b_play, b_set_current_position, b_set_format, b_set_volume, b_set_pan,
                                        b_set_frequency, b_stop, m_ok, m_ok };
    static const int buf_n[21] = { 3, 1, 1, 2, 3, 4, 2, 2, 2, 2, 3, 8, 4, 2, 2, 2, 2, 2, 1, 5, 1 };
    static const shim_fn notify_fn[4] = { n_query_interface, n_add_ref, n_release, n_set_notification_positions };
    static const int notify_n[4] = { 3, 1, 1, 3 };
#define GET(f, name) if (!(*(void **)&sa.f = input_sdl_proc(name))) return 0;
    GET(InitSubSystem, "SDL_InitSubSystem")
    GET(OpenAudioDeviceStream, "SDL_OpenAudioDeviceStream")
    GET(PutAudioStreamData, "SDL_PutAudioStreamData")
    GET(ResumeAudioStreamDevice, "SDL_ResumeAudioStreamDevice")
    GET(CreateMutex, "SDL_CreateMutex")
    GET(LockMutex, "SDL_LockMutex")
    GET(UnlockMutex, "SDL_UnlockMutex")
    GET(GetError, "SDL_GetError")
#undef GET
    g_mutex = sa.CreateMutex();
    if (!g_mutex)
        return 0;
    g_log = (int)GetPrivateProfileIntA("debug", "dsound_log", 0, ini);
    g_sync_log = g_log;
    g_rec_frames = (uint32_t)GetPrivateProfileIntA("debug", "dsound_record", 0, ini) * OUT_RATE;
    if (g_rec_frames) {
        g_rec = (int16_t *)malloc((size_t)g_rec_frames * 4);
        _snprintf_s(g_rec_path, sizeof g_rec_path, _TRUNCATE, "%s\\dsound_record.wav", game_dir);
    }
    g_vt_ds = vtable(ds_fn, ds_n, 11);
    g_vt_buf = vtable(buf_fn, buf_n, 21);
    g_vt_notify = vtable(notify_fn, notify_n, 4);
    return shim_add("DSOUND.dll", "#1", 3, sh_direct_sound_create);
}
