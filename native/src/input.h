#pragma once
#include <windows.h>

/* XInput-shaped pad snapshot: the common currency between the controller
 * backends (SDL3 or XInput through Xidi) and the modern-controls filter. */
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} pad_t;

#define BTN_DPAD_UP    0x0001
#define BTN_DPAD_DOWN  0x0002
#define BTN_DPAD_LEFT  0x0004
#define BTN_DPAD_RIGHT 0x0008
#define BTN_START      0x0010
#define BTN_BACK       0x0020
#define BTN_L3         0x0040
#define BTN_R3         0x0080
#define BTN_LB         0x0100
#define BTN_RB         0x0200
#define BTN_A          0x1000
#define BTN_B          0x2000
#define BTN_X          0x4000
#define BTN_Y          0x8000

/* Native controller input through SDL3 (any controller SDL knows: Xbox,
 * DualShock 4, DualSense, Switch Pro, 8BitDo...). Loads SDL3.dll from the
 * game folder; returns 1 if the SDL path is active. On 0 the game keeps
 * reading the pad through its DirectInput joystick (Xidi). */
int input_install(void);

/* 1 while SDL drives the controller. */
int input_sdl_active(void);

/* Latest physical pad as read by SDL this frame (raw, before any remap).
 * Returns 1 if a controller is connected. */
int input_sdl_pad(pad_t *out);

/* For the intro-skip poller (any thread): 1 if any button or trigger is
 * down, 0 if not, -1 if SDL isn't running (use XInput instead). */
int input_sdl_any_button(void);
