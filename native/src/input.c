/* Native controller input through SDL3.
 *
 * The game polls its pad once per frame in pad_poll (0x4F8E15): it starts
 * from 0xFFF9 (all PS1 buttons released, active low), clears bits for the
 * DirectInput joystick (left stick -> D-pad at half deflection, then 9
 * button bindings), then for the keyboard bindings.
 *
 * With SDL3.dll present we take over the joystick half:
 *   - the poll's "is there a joystick device" check reads our gate instead
 *     of the device pointer, so the DirectInput joystick (Xidi) is skipped;
 *   - at poll entry we read the SDL gamepad into an XInput-shaped pad, run
 *     the modern-controls filter on it (same as the old XInput wrapper did)
 *     and convert it to PS1 bits with the game's own joystick bindings;
 *   - right before the keyboard half those bits are cleared into the pad.
 * The keyboard half is untouched. Without SDL3.dll (or if SDL fails to
 * start) the gate mirrors the device pointer and the Xidi path runs as
 * before.
 *
 * SDL is loaded and started lazily on the game thread at the first poll
 * (never from DllMain). We never pump the window message queue: devices are
 * detected on SDL's joystick thread and SDL_UpdateGamepads refreshes state. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "controls.h"
#include "game.h"
#include "hook.h"
#include "input.h"
#include "log.h"

typedef unsigned char sdl_bool;   /* C99 bool, returned in AL */
typedef uint32_t sdl_joystick_id;
typedef struct SDL_Gamepad SDL_Gamepad;

enum {
    SDL_INIT_GAMEPAD_ = 0x2000,
    PAD_SOUTH = 0, PAD_EAST, PAD_WEST, PAD_NORTH, PAD_BACK, PAD_GUIDE, PAD_START,
    PAD_LSTICK, PAD_RSTICK, PAD_LSHOULDER, PAD_RSHOULDER,
    PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT,
    AX_LX = 0, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT
};

static struct {
    sdl_bool (__cdecl *SetHint)(const char *, const char *);
    sdl_bool (__cdecl *Init)(uint32_t);
    const char *(__cdecl *GetError)(void);
    sdl_joystick_id *(__cdecl *GetGamepads)(int *);
    void (__cdecl *free_)(void *);
    SDL_Gamepad *(__cdecl *OpenGamepad)(sdl_joystick_id);
    void (__cdecl *CloseGamepad)(SDL_Gamepad *);
    sdl_bool (__cdecl *GamepadConnected)(SDL_Gamepad *);
    void (__cdecl *UpdateGamepads)(void);
    sdl_bool (__cdecl *GetGamepadButton)(SDL_Gamepad *, int);
    int16_t (__cdecl *GetGamepadAxis)(SDL_Gamepad *, int);
    const char *(__cdecl *GetGamepadName)(SDL_Gamepad *);
    int (__cdecl *GetGamepadType)(SDL_Gamepad *);
    void (__cdecl *SetGamepadEventsEnabled)(sdl_bool);
    void (__cdecl *SetJoystickEventsEnabled)(sdl_bool);
    void (__cdecl *FlushEvents)(uint32_t, uint32_t);
} sdl;

#define TRIGGER_BUTTON  30      /* XINPUT_GAMEPAD_TRIGGER_THRESHOLD */
#define STICK_HALF      16384   /* the game's +-500 of +-1000 */
#define RESCAN_POLLS    60

static HMODULE g_sdl_dll;
static int g_active;            /* SDL path selected (gate patched) */
static int g_started, g_failed;
static volatile LONG g_ready;    /* SDL_Init finished: other threads may read */
static SDL_Gamepad *g_pad;
static pad_t g_raw;
static int g_connected;
static int g_rescan;
static uint16_t g_bits;         /* PS1 buttons to press this poll */
static volatile uint32_t g_gate;   /* stands in for the joystick device pointer */
static CRITICAL_SECTION g_cs;   /* game thread vs the intro-skip poller */

static int load_sdl(void)
{
    char path[MAX_PATH], *slash;
    GetModuleFileNameA(NULL, path, sizeof path);
    slash = strrchr(path, '\\');
    if (!slash)
        return 0;
    strcpy_s(slash + 1, sizeof path - (slash + 1 - path), "SDL3.dll");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return 0;
    g_sdl_dll = LoadLibraryA(path);
    if (!g_sdl_dll) {
        log_printf("input: SDL3.dll failed to load (error %lu)", GetLastError());
        return 0;
    }
#define GET(field, name) \
    if (!(*(FARPROC *)&sdl.field = GetProcAddress(g_sdl_dll, name))) { \
        log_printf("input: SDL3.dll is missing %s", name); \
        return 0; \
    }
    GET(SetHint, "SDL_SetHint")
    GET(Init, "SDL_Init")
    GET(GetError, "SDL_GetError")
    GET(GetGamepads, "SDL_GetGamepads")
    GET(free_, "SDL_free")
    GET(OpenGamepad, "SDL_OpenGamepad")
    GET(CloseGamepad, "SDL_CloseGamepad")
    GET(GamepadConnected, "SDL_GamepadConnected")
    GET(UpdateGamepads, "SDL_UpdateGamepads")
    GET(GetGamepadButton, "SDL_GetGamepadButton")
    GET(GetGamepadAxis, "SDL_GetGamepadAxis")
    GET(GetGamepadName, "SDL_GetGamepadName")
    GET(GetGamepadType, "SDL_GetGamepadType")
    GET(SetGamepadEventsEnabled, "SDL_SetGamepadEventsEnabled")
    GET(SetJoystickEventsEnabled, "SDL_SetJoystickEventsEnabled")
    GET(FlushEvents, "SDL_FlushEvents")
#undef GET
    return 1;
}

static void fall_back(const char *why)
{
    g_failed = 1;
    g_active = 0;
    log_printf("input: %s, falling back to the DirectInput joystick (Xidi)", why);
}

static void start_sdl(void)
{
    g_started = 1;
    /* device add/remove handled on SDL's own thread: we never pump messages here */
    sdl.SetHint("SDL_JOYSTICK_THREAD", "1");
    /* the game pauses itself when unfocused; don't let SDL second-guess focus */
    sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (!sdl.Init(SDL_INIT_GAMEPAD_)) {
        log_printf("input: SDL_Init failed: %s", sdl.GetError());
        fall_back("SDL could not start");
        return;
    }
    /* nobody drains SDL's event queue; keep it from filling up */
    sdl.SetGamepadEventsEnabled(0);
    sdl.SetJoystickEventsEnabled(0);
    InterlockedExchange(&g_ready, 1);
    log_printf("input: SDL3 started");
}

static const char *type_name(int t)
{
    static const char *names[] = { "unknown", "standard", "Xbox 360", "Xbox One", "PS3", "PS4", "PS5",
                                   "Switch Pro", "Joy-Con L", "Joy-Con R", "Joy-Con pair", "GameCube", "Steam" };
    return t >= 0 && t < (int)(sizeof names / sizeof names[0]) ? names[t] : "?";
}

static void track_gamepad(void)
{
    if (g_pad && !sdl.GamepadConnected(g_pad)) {
        log_printf("input: controller disconnected");
        sdl.CloseGamepad(g_pad);
        g_pad = NULL;
        g_rescan = 0;
    }
    if (!g_pad && g_rescan-- <= 0) {
        int n = 0;
        sdl_joystick_id *ids = sdl.GetGamepads(&n);
        g_rescan = RESCAN_POLLS;
        sdl.FlushEvents(0, 0xFFFF);
        if (ids) {
            if (n > 0) {
                g_pad = sdl.OpenGamepad(ids[0]);
                if (g_pad)
                    log_printf("input: controller \"%s\" (%s)", sdl.GetGamepadName(g_pad),
                               type_name(sdl.GetGamepadType(g_pad)));
                else
                    log_printf("input: SDL_OpenGamepad failed: %s", sdl.GetError());
            }
            sdl.free_(ids);
        }
    }
}

static SHORT flip(int16_t v) { return (SHORT)(v == -32768 ? 32767 : -v); }
static BYTE trig(int16_t v) { return (BYTE)(v <= 0 ? 0 : v >= 32767 ? 255 : (v * 255) / 32767); }

static void read_gamepad(void)
{
    static const struct { int sdl; WORD x; } map[] = {
        { PAD_SOUTH, BTN_A }, { PAD_EAST, BTN_B }, { PAD_WEST, BTN_X }, { PAD_NORTH, BTN_Y },
        { PAD_BACK, BTN_BACK }, { PAD_START, BTN_START }, { PAD_LSTICK, BTN_L3 }, { PAD_RSTICK, BTN_R3 },
        { PAD_LSHOULDER, BTN_LB }, { PAD_RSHOULDER, BTN_RB },
        { PAD_DUP, BTN_DPAD_UP }, { PAD_DDOWN, BTN_DPAD_DOWN }, { PAD_DLEFT, BTN_DPAD_LEFT },
        { PAD_DRIGHT, BTN_DPAD_RIGHT },
    };
    size_t i;
    memset(&g_raw, 0, sizeof g_raw);
    g_connected = 0;
    sdl.UpdateGamepads();
    track_gamepad();
    if (!g_pad)
        return;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (sdl.GetGamepadButton(g_pad, map[i].sdl))
            g_raw.wButtons |= map[i].x;
    g_raw.sThumbLX = sdl.GetGamepadAxis(g_pad, AX_LX);
    g_raw.sThumbLY = flip(sdl.GetGamepadAxis(g_pad, AX_LY));   /* SDL: down is +, XInput: up is + */
    g_raw.sThumbRX = sdl.GetGamepadAxis(g_pad, AX_RX);
    g_raw.sThumbRY = flip(sdl.GetGamepadAxis(g_pad, AX_RY));
    g_raw.bLeftTrigger = trig(sdl.GetGamepadAxis(g_pad, AX_LT));
    g_raw.bRightTrigger = trig(sdl.GetGamepadAxis(g_pad, AX_RT));
    g_connected = 1;
}

/* What the game's joystick half would have produced from this pad (as Xidi
 * presented it: buttons A B X Y LB RB LT RT Back Start L3 R3, left stick as
 * the X/Y axes). Returns the PS1 bits to clear. */
static uint16_t ps1_bits(const pad_t *p)
{
    static const WORD dinput_button[12] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, 0, 0,
                                            BTN_BACK, BTN_START, BTN_L3, BTN_R3 };
    const uint8_t *bind = (const uint8_t *)G_JOY_BINDINGS;
    const uint16_t *mask = (const uint16_t *)G_PAD_MASKS;
    uint16_t out = 0;
    int k;
    if (p->sThumbLX > STICK_HALF) out |= mask[3];
    if (p->sThumbLX < -STICK_HALF) out |= mask[2];
    if (p->sThumbLY > STICK_HALF) out |= mask[0];
    if (p->sThumbLY < -STICK_HALF) out |= mask[1];
    for (k = 0; k < G_JOY_BINDING_COUNT; k++) {
        int d = bind[k * 2], g = bind[k * 2 + 1], down;
        if (g >= 16)
            continue;
        if (d == 6)
            down = p->bLeftTrigger > TRIGGER_BUTTON;
        else if (d == 7)
            down = p->bRightTrigger > TRIGGER_BUTTON;
        else
            down = d < 12 && (p->wButtons & dinput_button[d]);
        if (down)
            out |= mask[g];
    }
    return out;
}

/* pad_poll entry */
static void on_poll_entry(regs_t *r)
{
    pad_t p;
    int conn;
    (void)r;
    if (!g_active) {
        g_gate = *(volatile uint32_t *)G_JOY_DEVICE;
        return;
    }
    if (!g_started)
        start_sdl();
    if (g_failed) {
        g_gate = *(volatile uint32_t *)G_JOY_DEVICE;
        g_bits = 0;
        return;
    }
    g_gate = 0;
    EnterCriticalSection(&g_cs);
    read_gamepad();
    p = g_raw;
    LeaveCriticalSection(&g_cs);
    conn = controls_debug_overlay(&p, g_connected);
    g_bits = conn ? (controls_filter(&p), ps1_bits(&p)) : 0;
}

/* pad_poll, right before the keyboard half */
static void on_poll_keyboard(regs_t *r)
{
    (void)r;
    if (g_active && g_bits) {
        uint16_t *pad = (uint16_t *)(uintptr_t)(*(volatile uint32_t *)G_PAD_BUF_PTR + 2);
        *pad &= (uint16_t)~g_bits;
    }
}

/* The game doesn't poll the pad while CAPCOM.AVI plays, so SDL is started
 * here instead (still the game thread, which owns SDL's helper windows) and
 * the intro-skip poller reads it through input_sdl_any_button. */
static void on_intro(regs_t *r)
{
    (void)r;
    if (g_active && !g_started)
        start_sdl();
}

int input_sdl_any_button(void)
{
    int down = 0;
    if (!input_sdl_active())
        return -1;
    EnterCriticalSection(&g_cs);
    read_gamepad();
    if (g_connected)
        down = g_raw.wButtons || g_raw.bLeftTrigger > 40 || g_raw.bRightTrigger > 40;
    LeaveCriticalSection(&g_cs);
    return down;
}

void *input_sdl_proc(const char *name)
{
    return g_sdl_dll ? (void *)GetProcAddress(g_sdl_dll, name) : NULL;
}

int input_sdl_active(void)
{
    return g_active && g_ready;
}

int input_sdl_pad(pad_t *out)
{
    *out = g_raw;
    return g_connected;
}

int input_install(void)
{
    static const uint8_t entry_bytes[] = { 0x53, 0x51, 0x52, 0x83, 0xEC, 0x50 };
    static const uint8_t kb_bytes[] = { 0x83, 0x3D, 0x3C, 0x1F, 0x55, 0x00, 0x00 };
    static const uint8_t gate_cmp[] = { 0x83, 0x3D, 0x40, 0x1F, 0x55, 0x00, 0x00 };
    static const uint8_t intro_call[] = { 0xE8, 0xB0, 0x29, 0x05, 0x00 };
    uint32_t gate_addr = (uint32_t)(uintptr_t)&g_gate;

    if (!load_sdl()) {
        log_printf("input: no SDL3.dll, controller goes through the DirectInput joystick (Xidi)");
        return 0;
    }
    if (!mem_expect(G_SITE_JOY_GATE, gate_cmp, sizeof gate_cmp) ||
        !hook_mid(G_FN_PAD_POLL, sizeof entry_bytes, entry_bytes, on_poll_entry) ||
        !hook_mid(G_SITE_PAD_POLL_KB, sizeof kb_bytes, kb_bytes, on_poll_keyboard)) {
        log_printf("input: pad poll hooks failed, SDL unused");
        return 0;
    }
    g_gate = *(volatile uint32_t *)G_JOY_DEVICE;
    if (!mem_write(G_SITE_JOY_GATE + 2, &gate_addr, 4)) {
        log_printf("input: gate patch failed, SDL unused");
        return 0;
    }
    InitializeCriticalSection(&g_cs);
    g_active = 1;
    /* absent when the intro is bypassed (the call is patched out) */
    if (mem_expect(G_SITE_INTRO_CALL, intro_call, sizeof intro_call))
        hook_mid(G_SITE_INTRO_CALL, sizeof intro_call, intro_call, on_intro);
    log_printf("input: SDL3 controller input (starts with the first pad poll)");
    return 1;
}
