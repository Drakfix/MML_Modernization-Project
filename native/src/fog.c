/* Distance fog options ([video] fog = original | light | off).
 *
 * The game's fog is plain Direct3D table fog: per scene it sets a colour and
 * a linear start/end in screen depth (Downtown: 0.1 to 0.7, white), and turns
 * it on and off per draw. The world is drawn well past where the fog maxes
 * out (depth ~0.93 in Downtown), so there's real scenery behind it. "light"
 * multiplies start and end by fog_distance (2 by default: fog twice as far
 * away), "off" keeps fog disabled. Done on the game's SetRenderState calls,
 * so it works the same with either renderer. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "ddshim.h"
#include "fog.h"
#include "hook.h"
#include "log.h"
#include "terrain.h"

#define VT_SET_RENDER_STATE 22
#define RS_FOGENABLE        28
#define RS_FOGTABLESTART    36
#define RS_FOGTABLEEND      37

typedef HRESULT(WINAPI *set_rs_t)(void *dev, DWORD state, DWORD value);

static int g_mode = FOG_ORIGINAL;
static float g_scale = 2.0f;
static set_rs_t g_next_set_rs;
/* the game's own last values, so a mode change can re-send them */
static void *g_dev;
static DWORD g_start, g_end;
static int g_have_range;
/* whether the game drew with fog lately (it switches fog per draw; a scene without fog never does) */
static int g_fog_drawn, g_frames_without;
#define FOG_GONE_FRAMES 15

static DWORD scaled(DWORD value)
{
    float f;
    if (g_mode != FOG_LIGHT)
        return value;
    memcpy(&f, &value, 4);
    f *= g_scale;
    memcpy(&value, &f, 4);
    return value;
}

static HRESULT WINAPI fog_set_rs(void *dev, DWORD state, DWORD value)
{
    g_dev = dev;
    if (state == RS_FOGTABLESTART) {
        g_start = value;
        g_have_range |= 1;
    } else if (state == RS_FOGTABLEEND) {
        g_end = value;
        g_have_range |= 2;
    }
    if (state == RS_FOGENABLE && value)
        g_fog_drawn = 1;
    if (state == RS_FOGENABLE && g_mode == FOG_OFF) {
        value = FALSE;
    } else if (state == RS_FOGTABLESTART || state == RS_FOGTABLEEND) {
        value = scaled(value);
    }
    return g_next_set_rs(dev, state, value);
}

void fog_install(int mode, float distance)
{
    g_mode = mode;
    if (distance > 0.0f)
        g_scale = distance;
    if (mode == FOG_OFF)
        log_printf("fog: off");
    else if (mode == FOG_LIGHT)
        log_printf("fog: light (%.2fx as far)", (double)g_scale);
}

/* in-game menu: switch while running (the range is re-sent at once; on/off
 * follows from the game's next per-draw fog switch) */
void fog_set_mode(int mode)
{
    g_mode = mode;
    if (g_next_set_rs && g_dev) {
        if (g_have_range & 1)
            g_next_set_rs(g_dev, RS_FOGTABLESTART, scaled(g_start));
        if (g_have_range & 2)
            g_next_set_rs(g_dev, RS_FOGTABLEEND, scaled(g_end));
    }
}

int fog_mode(void)
{
    return g_mode;
}

/* the terrain's range follows the fog: where it is full this frame, 0 = no fog in the picture */
static void report_fog_end(void)
{
    float end = 0.0f;
    DWORD v;
    if (g_fog_drawn)
        g_frames_without = 0;
    else if (g_frames_without < FOG_GONE_FRAMES)
        g_frames_without++;
    g_fog_drawn = 0;
    if (g_mode != FOG_OFF && (g_have_range & 2) && g_frames_without < FOG_GONE_FRAMES) {
        v = scaled(g_end);
        memcpy(&end, &v, 4);
        if (!(end > 0.0f))
            end = 0.0f;
    }
    terrain_fog_end(end);
}

void fog_frame(void)
{
    void **vt;
    if (g_next_set_rs) {   /* hooked even for "original", so it can change in game */
        report_fog_end();
        return;
    }
    vt = ddshim_device_vtable();
    if (vt) {
        void *mine = (void *)fog_set_rs;
        g_next_set_rs = (set_rs_t)vt[VT_SET_RENDER_STATE];
        host_write(&vt[VT_SET_RENDER_STATE], &mine, sizeof mine);
        log_printf("fog: SetRenderState hooked (vtable %p)", (void *)vt);
    }
}
