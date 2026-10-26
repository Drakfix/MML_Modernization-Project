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

#include "fog.h"
#include "hook.h"
#include "log.h"

#define G_D3D_DEVICE        0x551D10u   /* IDirect3DDevice3* */
#define VT_SET_RENDER_STATE 22
#define RS_FOGENABLE        28
#define RS_FOGTABLESTART    36
#define RS_FOGTABLEEND      37

typedef HRESULT(WINAPI *set_rs_t)(void *dev, DWORD state, DWORD value);

static int g_mode = FOG_ORIGINAL;
static float g_scale = 2.0f;
static set_rs_t g_next_set_rs;

static HRESULT WINAPI fog_set_rs(void *dev, DWORD state, DWORD value)
{
    if (state == RS_FOGENABLE && g_mode == FOG_OFF) {
        value = FALSE;
    } else if ((state == RS_FOGTABLESTART || state == RS_FOGTABLEEND) && g_mode == FOG_LIGHT) {
        float f;
        memcpy(&f, &value, 4);
        f *= g_scale;
        memcpy(&value, &f, 4);
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

void fog_frame(void)
{
    void *dev;
    if (g_mode == FOG_ORIGINAL || g_next_set_rs)
        return;
    dev = *(void **)G_D3D_DEVICE;
    if (dev) {
        void **vt = *(void ***)dev, *mine = (void *)fog_set_rs;
        g_next_set_rs = (set_rs_t)vt[VT_SET_RENDER_STATE];
        mem_write((uintptr_t)&vt[VT_SET_RENDER_STATE], &mine, sizeof mine);
        log_printf("fog: SetRenderState hooked (vtable %p)", (void *)vt);
    }
}
