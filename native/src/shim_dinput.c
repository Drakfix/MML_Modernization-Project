/* DirectInput for the recompiled game: the keyboard from the platform, the joystick from the
 * SDL controller input.c reads.
 *
 * What the game uses (FUN_004F8C23 setup, pad_poll 0x4F8E15, FUN_004F8FEF / FUN_004F904F for
 * the configuration screens): DirectInputCreateA (version 0x500); EnumDevices for joysticks,
 * whose callback (0x4F8BBD) makes the first one with CreateDevice and asks it for
 * IDirectInputDevice2 (Poll); the system keyboard. Device methods: SetDataFormat,
 * SetCooperativeLevel, SetProperty (axis ranges -1000..1000), Acquire, Unacquire,
 * GetDeviceState (256 key bytes; DIJOYSTATE), Poll, Release.
 *
 * The joystick is presented as Xidi did (input.c's reading of it): buttons A B X Y LB RB LT RT
 * Back Start L3 R3, the left stick as X / Y, the D-pad as the point-of-view hat. pad_poll skips
 * it while input.c drives the controller (its gate); the game's own button configuration reads
 * it. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "input.h"
#include "log.h"
#include "plat.h"
#include "recomp.h"
#include "recomp_rt.h"
#include "shim.h"

#define DI_OK_                0u
#define DIERR_INVALIDPARAM_   0x80070057u
#define DIERR_DEVICENOTREG_   0x80040154u
#define DIERR_UNSUPPORTED_    0x80004001u
#define E_NOINTERFACE_        0x80004002u
#define DIDEVTYPE_JOYSTICK_   4u
#define DIDEVTYPE_KEYBOARD_   3u

enum { DEV_KEYBOARD = 1, DEV_JOYSTICK = 2 };

static const uint8_t GUID_SysKeyboard_[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                               0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
/* our controller's instance and product: made up, only CreateDevice reads them back */
static const uint8_t GUID_Pad_[16] = { 0x4D, 0x4D, 0x4C, 0x50, 0x41, 0x44, 0x00, 0x00,
                                       0x8A, 0x1E, 0x53, 0x44, 0x4C, 0x33, 0x00, 0x01 };
/* IDirectInputDeviceA / IDirectInputDevice2A: 5944E680 / 5944E682-C92E-11CF-BFC7-444553540000 */
static const uint8_t IID_Device_tail[14] = { 0xE6, 0x59, 0x2E, 0xC9, 0xCF, 0x11,
                                             0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };

static uint32_t g_vt_di, g_vt_dev;
static uint32_t g_di_obj, g_dev_obj[3];   /* by DEV_ kind */
static int g_dev_refs[3];
static int32_t g_range_min[6] = { 0 }, g_range_max[6] = { 65535, 65535, 65535, 65535, 65535, 65535 };

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

static uint32_t new_object(uint32_t vt, uint32_t kind)
{
    uint32_t o = guest_alloc(8);
    if (o) {
        put32(o, vt);
        put32(o + 4, kind);
    }
    return o;
}

static int kind_of(uint32_t obj)
{
    uint32_t k = get32(obj + 4);
    return k == DEV_KEYBOARD || k == DEV_JOYSTICK ? (int)k : 0;
}

/* ---- IDirectInputDevice2A ---- */

static uint32_t v_query_interface(const uint32_t *a)
{
    const uint8_t *iid = HPTR(a[1]);
    if ((iid[0] == 0x80 || iid[0] == 0x82) && iid[1] == 0xE6 && memcmp(iid + 2, IID_Device_tail, 14) == 0) {
        g_dev_refs[kind_of(a[0])]++;
        put32(a[2], a[0]);   /* one object answers both versions */
        return DI_OK_;
    }
    put32(a[2], 0);
    return E_NOINTERFACE_;
}

static uint32_t v_add_ref(const uint32_t *a)
{
    return (uint32_t)++g_dev_refs[kind_of(a[0])];
}

static uint32_t v_release(const uint32_t *a)
{
    int k = kind_of(a[0]);
    if (g_dev_refs[k] > 0)
        g_dev_refs[k]--;
    return (uint32_t)g_dev_refs[k];   /* the object stays: the game makes each device once */
}

static uint32_t v_ok(const uint32_t *a)
{
    (void)a;
    return DI_OK_;
}

static uint32_t v_unsupported(const uint32_t *a)
{
    (void)a;
    log_printf("dinput: a method the game wasn't seen to use was called");
    return DIERR_UNSUPPORTED_;
}

/* SetProperty(this, property, DIPROPHEADER *): DIPROP_RANGE by offset into DIJOYSTATE */
static uint32_t v_set_property(const uint32_t *a)
{
    uint32_t hdr = a[2], obj = get32(hdr + 8), how = get32(hdr + 12);
    if (a[1] == 4 && how == 1 && obj / 4 < 6) {   /* DIPROP_RANGE, DIPH_BYOFFSET */
        g_range_min[obj / 4] = (int32_t)get32(hdr + 16);
        g_range_max[obj / 4] = (int32_t)get32(hdr + 20);
    }
    return DI_OK_;
}

static int32_t axis(int i, int v)   /* -32768..32767 to the range set */
{
    int64_t span = (int64_t)g_range_max[i] - g_range_min[i];
    return (int32_t)(g_range_min[i] + ((int64_t)v + 32768) * span / 65535);
}

/* GetDeviceState(this, size, data) */
static uint32_t v_get_device_state(const uint32_t *a)
{
    uint8_t *out = HPTR(a[2]);
    if (kind_of(a[0]) == DEV_KEYBOARD) {
        if (a[1] < 256)
            return DIERR_INVALIDPARAM_;
        plat_key_state(out);
        return DI_OK_;
    }
    if (a[1] < 0x50)   /* DIJOYSTATE */
        return DIERR_INVALIDPARAM_;
    {
        static const WORD order[12] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, 0, 0,
                                        BTN_BACK, BTN_START, BTN_L3, BTN_R3 };
        pad_t p;
        int32_t v;
        uint32_t pov = 0xFFFFFFFFu;
        int k;
        memset(out, 0, a[1]);
        memset(&p, 0, sizeof p);
        input_sdl_snapshot(&p);
        v = axis(0, p.sThumbLX), memcpy(out, &v, 4);
        v = axis(1, -1 - p.sThumbLY), memcpy(out + 4, &v, 4);   /* DirectInput's Y grows downward */
        for (k = 2; k < 6; k++)
            v = axis(k, 0), memcpy(out + 4 * k, &v, 4);
        if (p.wButtons & (BTN_DPAD_UP | BTN_DPAD_DOWN | BTN_DPAD_LEFT | BTN_DPAD_RIGHT)) {
            int x = !!(p.wButtons & BTN_DPAD_RIGHT) - !!(p.wButtons & BTN_DPAD_LEFT);
            int y = !!(p.wButtons & BTN_DPAD_DOWN) - !!(p.wButtons & BTN_DPAD_UP);
            static const uint32_t hat[3][3] = { { 31500, 0, 4500 }, { 27000, 0xFFFFFFFFu, 9000 }, { 22500, 18000, 13500 } };
            pov = hat[y + 1][x + 1];
        }
        for (k = 0; k < 4; k++)
            memcpy(out + 0x20 + 4 * k, &pov, 4);
        for (k = 0; k < 12; k++) {
            int down = k == 6 ? p.bLeftTrigger > 30 : k == 7 ? p.bRightTrigger > 30 : (p.wButtons & order[k]) != 0;
            out[0x30 + k] = down ? 0x80 : 0;
        }
    }
    return DI_OK_;
}

/* ---- IDirectInputA ---- */

static uint32_t i_query_interface(const uint32_t *a)
{
    put32(a[2], 0);
    return E_NOINTERFACE_;
}

static uint32_t i_ref(const uint32_t *a)
{
    (void)a;
    return 1;
}

/* CreateDevice(this, guid, out, outer) */
static uint32_t i_create_device(const uint32_t *a)
{
    const uint8_t *g = HPTR(a[1]);
    int k = memcmp(g, GUID_SysKeyboard_, 16) == 0 ? DEV_KEYBOARD : memcmp(g, GUID_Pad_, 16) == 0 ? DEV_JOYSTICK : 0;
    if (!k) {
        put32(a[2], 0);
        return DIERR_DEVICENOTREG_;
    }
    if (!g_dev_obj[k])
        g_dev_obj[k] = new_object(g_vt_dev, (uint32_t)k);
    g_dev_refs[k]++;
    put32(a[2], g_dev_obj[k]);
    return DI_OK_;
}

/* EnumDevices(this, type, callback, ref, flags): our controller for joysticks (always there, as
   Xidi's virtual one was); the callback is game code */
static uint32_t i_enum_devices(const uint32_t *a)
{
    uint32_t inst, args[2];
    if (a[1] != 0 && a[1] != DIDEVTYPE_JOYSTICK_)
        return DI_OK_;
    inst = guest_alloc(0x244);   /* DIDEVICEINSTANCEA */
    if (!inst)
        return DI_OK_;
    put32(inst, 0x244);
    memcpy(HPTR(inst + 4), GUID_Pad_, 16);
    memcpy(HPTR(inst + 20), GUID_Pad_, 16);
    put32(inst + 36, DIDEVTYPE_JOYSTICK_ | (2u << 8));   /* gamepad */
    strcpy_s((char *)HPTR(inst + 40), 260, "Controller (SDL)");
    strcpy_s((char *)HPTR(inst + 300), 260, "Controller (SDL)");
    args[0] = inst, args[1] = a[3];   /* the instance, the caller's ref */
    recomp_call_stdcall(a[2], 2, args);
    guest_free(inst);
    return DI_OK_;
}

/* DirectInputCreateA(instance, version, out, outer) */
static uint32_t sh_direct_input_create_a(const uint32_t *a)
{
    if (!g_di_obj)
        g_di_obj = new_object(g_vt_di, 0);
    put32(a[2], g_di_obj);
    return DI_OK_;
}

static uint32_t vtable(const shim_fn *fn, const int *nargs, int n)
{
    uint32_t vt = guest_alloc(4 * (size_t)n);
    int k;
    for (k = 0; k < n && vt; k++)
        put32(vt + 4 * (uint32_t)k, shim_method(nargs[k], fn[k]));
    return vt;
}

int shim_dinput_install(void)
{
    /* QueryInterface AddRef Release CreateDevice EnumDevices GetDeviceStatus RunControlPanel Initialize */
    static const shim_fn di_fn[8] = { i_query_interface, i_ref, i_ref, i_create_device, i_enum_devices,
                                      v_ok, v_unsupported, v_ok };
    static const int di_n[8] = { 3, 1, 1, 4, 5, 2, 3, 3 };
    /* QueryInterface AddRef Release GetCapabilities EnumObjects GetProperty SetProperty Acquire
       Unacquire GetDeviceState GetDeviceData SetDataFormat SetEventNotification SetCooperativeLevel
       GetObjectInfo GetDeviceInfo RunControlPanel Initialize; Device2: CreateEffect EnumEffects
       GetEffectInfo GetForceFeedbackState SendForceFeedbackCommand EnumCreatedEffectObjects
       Escape Poll SendDeviceData */
    static const shim_fn dev_fn[27] = { v_query_interface, v_add_ref, v_release, v_unsupported, v_unsupported,
                                        v_unsupported, v_set_property, v_ok, v_ok, v_get_device_state,
                                        v_unsupported, v_ok, v_ok, v_ok, v_unsupported, v_unsupported, v_unsupported,
                                        v_ok, v_unsupported, v_unsupported, v_unsupported, v_unsupported,
                                        v_unsupported, v_unsupported, v_unsupported, v_ok, v_unsupported };
    static const int dev_n[27] = { 3, 1, 1, 2, 4, 3, 3, 1, 1, 3, 5, 2, 2, 3, 4, 2, 3, 4, 5, 4, 3, 2, 2, 4, 2, 1, 5 };
    if (!input_sdl_proc("SDL_Init"))   /* SDL3.dll loaded: input.c drives the controller */
        return 0;
    g_vt_di = vtable(di_fn, di_n, 8);
    g_vt_dev = vtable(dev_fn, dev_n, 27);
    return shim_add("DINPUT.dll", "DirectInputCreateA", 4, sh_direct_input_create_a);
}
