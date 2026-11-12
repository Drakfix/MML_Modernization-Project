/* In-game options menu (F1, or Back + Start on a controller), and the pad
 * chords (Back + Start, Back + RB for the quick save).
 *
 * Drawn by our renderer over the final picture (gfx_ui), so the game keeps
 * running underneath and every change shows at once: good for comparing
 * settings side by side. While it's open the game gets no input (pad, game
 * keys, mouse look). Each change is applied live and written to mml.ini right
 * away; the launcher reads those keys back, so both stay in step.
 *
 * Three pages (LB/RB or Q/E), one setting per row, with a line of help for the
 * selected one. The look borrows from the game's own HUD: Mega Man blue
 * panels with cut corners, yellow accents and green energy-cell gauges.
 *
 * Runs on the game thread: menu_frame() once per game frame (input),
 * menu_filter_buttons() from the pad poll, and the draw callback while each
 * presented frame is composed.
 *
 * The start menu (menu_start) is the same menu before the game starts, where there is no
 * launcher (Android, later everywhere): every setting the launcher has, the ones that only
 * count from the start too (renderer, controls, button mapping), on more pages, and Start to
 * play. It only writes mml.ini; the game reads it when it starts. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controls.h"
#include "ddshim.h"
#include "fog.h"
#include "frame_rec.h"
#include "gamefiles.h"
#include "gfx.h"
#include "gfx_ui.h"
#include "input.h"
#include "kbm.h"
#include "lockon.h"
#include "lod.h"
#include "log.h"
#include "menu.h"
#include "menu_music.h"
#include "plat.h"
#include "toast.h"
#include "texreplace.h"
#include "ui_style.h"
#include "updater.h"
#include "voice_build.h"

#define MAX_PRESETS 4096

enum {
    IT_FPS, IT_VSYNC, IT_RES, IT_MSAA, IT_TEXFILTER, IT_UPSCALE,
    IT_SHADER, IT_SHADER_RES, IT_FOG, IT_LOD, IT_HDTEX,
    IT_CAMSPEED, IT_INVERT_X, IT_INVERT_Y, IT_MOUSE, IT_RETICLE, IT_AIM, IT_CROSSHAIR,
    /* the keyboard's keys for the modern controls ([controls] key_*: two each), and putting them back */
    IT_KEY, IT_KEY_RESET,
    IT_SECTION,  /* a heading over the rows after it (the start menu's): not selectable
                    (keep it before IT_CHOICE: the rows it is told apart from) */
    IT_CHOICE,   /* one of a list, each choice a set of mml.ini values (the start menu's) */
    /* the start menu's game files: the version to play; the US and the Japanese copy (where they
       stand, A imports) */
    IT_GF_EDITION, IT_GF_US, IT_GF_JP,
    /* the US version's extras: the English subtitles (A downloads them), the Japanese voices (A
       builds them from the Japanese version); then on / off */
    IT_GF_SUBS, IT_GF_VOICES,
    /* the start menu's updates (updater.h): check / install; the versions (keep these last: menu.c
       tells them apart by being >= IT_UP_ACTION) */
    IT_UP_ACTION, IT_UP_VERSION, IT_UP_LATEST,
};

/* an IT_CHOICE's choice: "section.key=value;section.key=value" written when chosen (a choice
   that starts with "=" is the value of the item's own key); the first pair tells which choice
   mml.ini holds */
typedef struct {
    const char *label, *set;
} choice;

/* where an item is offered (0 = everywhere): Windows, everywhere else, Android only (the updater),
   Linux (with Windows: mouse and keyboard, vsync) */
#define PL_WIN 1
#define PL_OTHER 2
#define PL_ANDROID 4
#define PL_LINUX 8
#ifdef _WIN32
#define PL_HERE PL_WIN
#elif defined(__ANDROID__)
#define PL_HERE (PL_OTHER | PL_ANDROID)
#elif defined(__linux__)
#define PL_HERE (PL_OTHER | PL_LINUX)
#else
#define PL_HERE PL_OTHER
#endif

typedef struct {
    int page, kind;
    const char *label, *help;
    const choice *ch;   /* IT_CHOICE: its choices, how many, its own key, the default */
    int nch;
    int plat;
    const char *key;
    int def;
} item;

static const char *const GAME_PAGES[] = { "VIDEO", "PICTURE", "CONTROLS" };
#define AIM_HELP "Camera: firing turns Mega Man to where the camera looks. Character: shots go where he faces."
#define XH_HELP "With camera aim, a marker shows where your shots go."

static const item g_game_items[] = {
    { 0, IT_FPS, "Frame rate", "60 fps adds in-between frames for smooth motion. The game itself still runs at 30." },
    { 0, IT_VSYNC, "Vsync", "Locks frames to your monitor's refresh so the picture never tears. Adds a little lag." },
    { 0, IT_RES, "Resolution", "Display renders at your monitor's resolution. 1x to 4x are multiples of the game's 480 lines." },
    { 0, IT_MSAA, "Antialiasing", "Smooths jagged polygon edges. ReShade depth effects stop working while it's on." },
    { 0, IT_TEXFILTER, "Texture filter", "Anisotropic keeps floors and walls sharp when you look along them." },
    { 0, IT_UPSCALE, "Upscaling", "How a lower resolution is stretched to the screen. Nearest keeps the pixels crisp." },
    { 1, IT_SHADER, "Shader", "RetroArch shader presets (CRT looks and more) from the Modernization\\shaders folder." },
    { 1, IT_SHADER_RES, "Shader input", "What the shader works on. CRT presets look right at the game's own resolution." },
    { 1, IT_FOG, "Distance fog", "The original fog, a lighter one that lets you see further, or none at all." },
    { 1, IT_LOD, "Model detail", "Full detail keeps faraway models at their best instead of swapping in low-poly ones." },
    { 1, IT_HDTEX, "HD textures", "Replacement textures from the Modernization\\textures folder." },
    { 2, IT_CAMSPEED, "Camera speed", "How fast the right stick turns the camera." },
    { 2, IT_INVERT_X, "Invert camera X", "Flips left and right on the camera stick." },
    { 2, IT_INVERT_Y, "Invert camera Y", "Flips up and down on the camera stick and the mouse." },
    { 2, IT_MOUSE, "Mouse & keyboard", "Mouse look speed. Off turns the mouse and keyboard controls off." },
    { 2, IT_RETICLE, "Lock-on reticle", "Colour of the marker on the enemy you're locked on to, and of the crosshair." },
    { 2, IT_AIM, "Shots aim", AIM_HELP },
    { 2, IT_CROSSHAIR, "Crosshair", XH_HELP },
};

/* ---- the start menu's pages: everything the launcher has ----
   Names and descriptions say what each setting technically is and what it costs, as a PC game's
   settings do; the description is also where a newcomer finds out what it means. */
static const char *const START_PAGES[] = { "GAME", "DISPLAY", "GRAPHICS", "CONTROLS", "BUTTONS" };
#define PAGE_FILES 0   /* GAME: the versions (imported where there is an importer), language, launch, updates */
#define PG_DISPLAY 1
#define PG_GRAPHICS 2
#define PG_CONTROLS 3
#define PG_BUTTONS 4

static const choice CH_RENDERER[] = { { "Direct3D 11", "video.renderer=mml;video.backend=d3d11" },
                                      { "Vulkan", "video.renderer=mml;video.backend=vulkan" },
                                      { "DirectDraw (legacy)", "video.renderer=ddrawcompat" } };
static const choice CH_WINDOW[] = { { "Borderless window", "video.windowed=0;video.exclusive=0" },
                                    { "Exclusive fullscreen", "video.windowed=0;video.exclusive=1" },
                                    { "Windowed", "video.windowed=1;video.exclusive=0" } };
static const choice CH_ASPECT[] = { { "16:9 widescreen", "video.aspect=16:9" }, { "4:3 original", "video.aspect=4:3" } };
static const choice CH_ENGINE[] = { { "Original (32-bit)", "general.engine=classic" },
                                    { "Recompiled (64-bit)", "general.engine=standalone" } };
static const choice CH_INTRO[] = { { "Play", "general.intro=play" }, { "Skip", "general.intro=skip" } };
static const choice CH_STARTMENU[] = { { "Show", "general.start_menu=1" }, { "Skip", "general.start_menu=0" } };
static const choice CH_HINTS[] = { { "On", "general.title_hints=1" }, { "Off", "general.title_hints=0" } };
static const choice CH_CONTROLS[] = { { "Modern", "controls.mode=modern" }, { "Original (1997)", "controls.mode=vanilla" } };
static const choice CH_ORBIT[] = { { "Off", "controls.orbit_start=0" }, { "On", "controls.orbit_start=1" } };
static const choice CH_JPBTN[] = { { "US layout", "controls.jp_buttons=us" },
                                   { "Japanese layout", "controls.jp_buttons=japanese" } };
static const choice CH_INPUT[] = { { "SDL3", "controls.input=sdl" }, { "DirectInput (legacy)", "controls.input=legacy" } };
static const choice CH_DEADZONE[] = { { "12%", "controls.deadzone=12" }, { "24% (default)", "controls.deadzone=24" },
                                      { "32%", "controls.deadzone=32" } };
static const choice CH_TILT[] = { { "Walk", "controls.walk_tilt=55" }, { "Run", "controls.walk_tilt=0" } };
static const choice CH_ROLL[] = { { "Original", "controls.roll_speed=512;controls.roll_anim=0" },
                                  { "Fast", "controls.roll_speed=768;controls.roll_anim=50" },
                                  { "Snappy", "controls.roll_speed=896;controls.roll_anim=80" } };
static const choice CH_UPDATES[] = { { "On", "general.check_updates=1" }, { "Off", "general.check_updates=0" } };
static const choice CH_ACTION[] = { { "Jump", "=jump" }, { "Shoot", "=shoot" }, { "Special", "=special" },
                                    { "Talk / check", "=talk" }, { "Roll", "=roll" }, { "Aim / lock-on", "=aim" },
                                    { "Modern on/off", "=orbit" }, { "Menu", "=menu" }, { "Select", "=select" },
                                    { "Nothing", "=none" } };
#define NCH(a) ((int)(sizeof a / sizeof a[0]))
/* the confirm button in a help text: A with a controller, Space with the keyboard (button_words) */
#define OK_KEY "\x01"
#define BTN_HELP "What this button does with the modern controls. With the original controls the game uses its own " \
    "1997 layout."
#define KEY_HELP "This action's key with mouse and keyboard (modern controls): " OK_KEY " and then the new key or " \
    "mouse button (Esc or B cancels). A second key, such as the arrows for moving, stays as it is."
#define START_AIM_HELP "Camera: firing turns Mega Man toward where the camera looks, so shots go there, up and down " \
    "included. Character: shots go where Mega Man faces, as in the original."
#define START_XH_HELP "With Aim mode on Camera, a crosshair marks where your shots go. It appears with the first " \
    "shot and fades a few seconds after the last."
#define WL (PL_WIN | PL_LINUX)

static const item g_start_items[] = {
    /* ---- GAME ---- */
    { PAGE_FILES, IT_SECTION, "Versions" },
    { PAGE_FILES, IT_GF_EDITION, "Version to play", "Which version Play starts: Mega Man Legends (the US release) "
      "or Rockman DASH (the Japanese release). Each version keeps its own saves; settings and mods are shared." },
    { PAGE_FILES, IT_GF_US, "Mega Man Legends (US)", OK_KEY " imports the US PC release: copy its folder (the one "
      "with MEGAMAN.EXE) to this device, then pick it. The game is copied into the app's storage and patched there; "
      "the folder you pick isn't changed.", NULL, 0, PL_OTHER },
    { PAGE_FILES, IT_GF_JP, "Rockman DASH (JP)", OK_KEY " imports the Japanese PC release: copy its folder (the one "
      "with ROCKMAN.EXE) to this device, then pick it. It's kept apart from the US version, with its own saves.",
      NULL, 0, PL_OTHER },
    { PAGE_FILES, IT_GF_US, "Mega Man Legends (US)", OK_KEY " imports the US PC release: pick the folder with "
      "MEGAMAN.EXE. The game is copied into this app's folder and patched there; the folder you pick isn't "
      "changed.", NULL, 0, PL_WIN },
    { PAGE_FILES, IT_GF_JP, "Rockman DASH (JP)", OK_KEY " imports the Japanese PC release: pick the folder with "
      "ROCKMAN.EXE. It's copied into this app's folder, apart from the US version and with its own saves; the "
      "folder you pick isn't changed.", NULL, 0, PL_WIN },
    { PAGE_FILES, IT_SECTION, "Language" },
    { PAGE_FILES, IT_GF_SUBS, "English subtitles", "Subtitles for the cutscenes, which the original release never "
      "had: US version, with either voice language. They are the Rockman DASH PSP English project's work "
      "(github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher): the English voice acting, transcribed and timed. "
      OK_KEY " downloads them from there, then turns them on and off. Experimental." },
    { PAGE_FILES, IT_GF_VOICES, "Voice language", "The voice acting in the US version: English, or the original "
      "Japanese voices. " OK_KEY " builds the Japanese voices from your imported Rockman DASH (about 260 MB, once), "
      "then switches between the two. Experimental: lines only the Japanese release voices stay silent." },
    { PAGE_FILES, IT_SECTION, "Launch" },
    { PAGE_FILES, IT_CHOICE, "Start menu at launch", "Show opens this menu every time. Skip starts the game straight "
      "away with the saved settings. To come back here, hold Back (controller) or F2 (keyboard) on the game's "
      "title screen.", CH_STARTMENU, NCH(CH_STARTMENU) },
    { PAGE_FILES, IT_CHOICE, "Intro video", "Plays the project's intro video when the game starts. Any button skips "
      "it while it plays.", CH_INTRO, NCH(CH_INTRO) },
    { PAGE_FILES, IT_CHOICE, "Title screen hints", "A card on the title screen listing the shortcuts: the settings "
      "menu, quick save, the modern controls switch, and the way back to this menu.", CH_HINTS, NCH(CH_HINTS) },
    { PAGE_FILES, IT_SECTION, "Updates", NULL, NULL, 0, PL_ANDROID | PL_WIN },
    { PAGE_FILES, IT_UP_ACTION, "Update", "Looks for a newer release on GitHub, downloads its APK and hands it to "
      "Android's installer. Android may ask once to allow installs from this app, and to confirm.",
      NULL, 0, PL_ANDROID },
    { PAGE_FILES, IT_UP_ACTION, "Update", "Looks for a newer release on GitHub, downloads its MML-Setup.exe, checks "
      "it against the SHA-256 GitHub lists and runs it on this folder. This menu closes while Setup updates the "
      "files; imported games, settings and saves stay.", NULL, 0, PL_WIN },
    { PAGE_FILES, IT_UP_VERSION, "Installed version", "The version of the app you're running.", NULL, 0,
      PL_ANDROID | PL_WIN },
    { PAGE_FILES, IT_UP_LATEST, "Newest release", "The newest release on GitHub (pre-releases aren't counted).",
      NULL, 0, PL_ANDROID | PL_WIN },
    { PAGE_FILES, IT_CHOICE, "Check on launch", "Asks GitHub for the newest release when the app opens. Only the "
      "release list is requested; nothing about you or your system is sent.", CH_UPDATES, NCH(CH_UPDATES),
      PL_ANDROID | PL_WIN },

    /* ---- DISPLAY ---- */
    { PG_DISPLAY, IT_CHOICE, "Graphics API", "The API the project's renderer draws with. Direct3D 11 is the default "
      "and the most tested; Vulkan is the alternative. Both support every feature: widescreen, 60 fps, the in-game "
      "menu. DirectDraw (legacy) runs the game's own 1997 DirectDraw / Direct3D output through the DDrawCompat "
      "wrapper, without those features: a fallback for systems where the other two fail.",
      CH_RENDERER, NCH(CH_RENDERER), PL_WIN },
    { PG_DISPLAY, IT_CHOICE, "Display mode", "Borderless window covers the screen as a normal window: instant "
      "Alt+Tab, overlays and screen capture work. Exclusive fullscreen hands the display to the game: slightly "
      "lower latency and GPU use, slower Alt+Tab, limited capture. Windowed: a movable window, three quarters of "
      "the screen.", CH_WINDOW, NCH(CH_WINDOW), PL_WIN },
    { PG_DISPLAY, IT_RES, "Render resolution", "The resolution the 3D scene is rendered at before it's scaled to "
      "the screen. Native renders at your display's resolution. 1x to 4x are multiples of the game's original 480 "
      "lines: lower values keep the original pixelated look and cost less GPU time." },
    { PG_DISPLAY, IT_CHOICE, "Aspect ratio", "16:9 widescreen renders a wider field of view, places the HUD for a "
      "wide screen and stretches 4:3 backgrounds to fit. 4:3 original keeps the original framing, with bars on a "
      "wide screen.", CH_ASPECT, NCH(CH_ASPECT) },
    { PG_DISPLAY, IT_FPS, "Frame rate", "The game's logic always runs at 30 fps. At 60 fps the renderer draws an "
      "interpolated frame between every two game frames: smoother camera and motion, a little more GPU time, no "
      "change to game speed. 30 fps shows the original frames only." },
    { PG_DISPLAY, IT_VSYNC, "V-Sync", "Synchronizes frames with the monitor's refresh rate. On removes screen "
      "tearing and adds up to one frame of input latency. Off presents frames as soon as they're ready.",
      NULL, 0, WL },
    { PG_DISPLAY, IT_CHOICE, "Executable", "Original runs Capcom's 2001 MEGAMAN.EXE (32-bit) with the project's "
      "runtime, mml.dll, loaded into it. Recompiled runs the game's code translated to C and compiled into a "
      "native 64-bit program, mml_run64.exe: the original executable's code never runs, only its data is read. "
      "Recompiled doesn't support shader presets yet. DirectDraw (legacy) always uses Original.",
      CH_ENGINE, NCH(CH_ENGINE), PL_WIN },

    /* ---- GRAPHICS ---- */
    { PG_GRAPHICS, IT_MSAA, "Anti-aliasing", "Multisample anti-aliasing (MSAA) smooths jagged polygon edges by "
      "sampling each pixel 4 or 8 times. Costs GPU time and video memory, and turns off depth-based ReShade "
      "effects." },
    { PG_GRAPHICS, IT_TEXFILTER, "Texture filtering", "How textures are sampled. Bilinear blends neighbouring "
      "texels. Anisotropic 16x keeps surfaces sharp at steep viewing angles, such as floors and walls into the "
      "distance, at a small GPU cost." },
    { PG_GRAPHICS, IT_UPSCALE, "Upscaling filter", "How the rendered image is scaled to the screen when Render "
      "resolution is below your display's. Bilinear smooths the pixels; Nearest neighbor keeps them sharp and "
      "square." },
    { PG_GRAPHICS, IT_SHADER, "Shader preset", "A RetroArch shader preset (.slangp: CRT, scanlines, LCD and more) "
      "applied to the final image. Presets go in Modernization\\shaders; RetroArch's slang-shaders collection "
      "works. Original executable with Direct3D 11 only.", NULL, 0, PL_WIN },
    { PG_GRAPHICS, IT_SHADER_RES, "Shader input resolution", "The resolution the shader preset processes. Native "
      "(480p) feeds it the game's original resolution, which CRT presets are made for; Output feeds it the full "
      "render resolution.", NULL, 0, PL_WIN },
    { PG_GRAPHICS, IT_FOG, "Fog distance", "The game's distance fog. Original keeps it as released; Reduced pushes "
      "it further out, so more of the level is visible; Off removes it." },
    { PG_GRAPHICS, IT_LOD, "Level of detail", "Original swaps distant models for low-polygon versions, as the 2001 "
      "release did. Maximum keeps the full-detail models at any distance, at a small GPU cost." },
    { PG_GRAPHICS, IT_HDTEX, "Texture replacement", "Loads replacement textures (HD texture packs) from "
      "Modernization\\textures. Off uses the game's original textures." },

    /* ---- CONTROLS ---- */
    { PG_CONTROLS, IT_SECTION, "Scheme" },
    { PG_CONTROLS, IT_CHOICE, "Control scheme", "Modern: camera-relative movement, a free camera on the right "
      "stick, lock-on targeting and dodge rolls in any direction. Original (1997): the game's own tank-style "
      "controls and camera.", CH_CONTROLS, NCH(CH_CONTROLS) },
    { PG_CONTROLS, IT_CHOICE, "Modern controls on launch", "Whether the game starts with the modern controls "
      "active. In game, L3 or R3 (F5 on the keyboard) switches them on and off.", CH_ORBIT, NCH(CH_ORBIT), 0, NULL,
      1 },
    { PG_CONTROLS, IT_CHOICE, "Input API", "SDL3 reads Xbox, PlayStation, Switch and most other controllers, with "
      "the right button layouts. DirectInput (legacy) goes through the Xidi wrapper: use it only if SDL3 doesn't "
      "detect your controller.", CH_INPUT, NCH(CH_INPUT), PL_WIN },
    { PG_CONTROLS, IT_CHOICE, "Confirm button (Rockman DASH)", "Rockman DASH only. US layout: the bottom face "
      "button (A / Cross) confirms and the top one (Y / Triangle) goes back. Japanese layout: the right face "
      "button (B / Circle) confirms and the bottom one goes back.", CH_JPBTN, NCH(CH_JPBTN) },
    { PG_CONTROLS, IT_SECTION, "Camera" },
    { PG_CONTROLS, IT_CAMSPEED, "Camera sensitivity", "How fast the right stick turns the camera, relative to the "
      "default." },
    { PG_CONTROLS, IT_INVERT_X, "Invert camera X", "Reverses left and right on the camera stick." },
    { PG_CONTROLS, IT_INVERT_Y, "Invert camera Y", "Reverses up and down on the camera stick and the mouse." },
    { PG_CONTROLS, IT_SECTION, "Movement" },
    { PG_CONTROLS, IT_CHOICE, "Stick dead zone", "How far a stick must move before the game reacts, as a share of "
      "its travel. A larger dead zone hides stick drift; a smaller one reacts sooner.", CH_DEADZONE,
      NCH(CH_DEADZONE), 0, NULL, 1 },
    { PG_CONTROLS, IT_CHOICE, "Light stick tilt", "What a light push of the left stick does. Walk: a light push "
      "walks and a full push runs (analog movement). Run: any push runs, as in the original.", CH_TILT,
      NCH(CH_TILT) },
    { PG_CONTROLS, IT_CHOICE, "Dodge roll speed", "Original is the 1997 cartwheel. Fast and Snappy cover the "
      "distance sooner, with a faster animation.", CH_ROLL, NCH(CH_ROLL), 0, NULL, 1 },
    { PG_CONTROLS, IT_SECTION, "Aiming" },
    { PG_CONTROLS, IT_AIM, "Aim mode", START_AIM_HELP },
    { PG_CONTROLS, IT_CROSSHAIR, "Crosshair", START_XH_HELP },
    { PG_CONTROLS, IT_RETICLE, "Reticle colour", "Colour of the lock-on marker on the targeted enemy, and of the "
      "crosshair." },
    { PG_CONTROLS, IT_SECTION, "Mouse and keyboard", NULL, NULL, 0, WL },
    { PG_CONTROLS, IT_MOUSE, "Mouse sensitivity", "Mouse look speed for the camera, relative to the default. Off "
      "turns the mouse and keyboard controls off entirely.", NULL, 0, WL },

    /* ---- BUTTONS ---- */
    { PG_BUTTONS, IT_SECTION, "Controller" },
    { PG_BUTTONS, IT_CHOICE, "A", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_a", 0 },
    { PG_BUTTONS, IT_CHOICE, "B", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_b", 2 },
    { PG_BUTTONS, IT_CHOICE, "X", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_x", 4 },
    { PG_BUTTONS, IT_CHOICE, "Y", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_y", 3 },
    { PG_BUTTONS, IT_CHOICE, "LB", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_lb", 5 },
    { PG_BUTTONS, IT_CHOICE, "RB", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_rb", 9 },
    { PG_BUTTONS, IT_CHOICE, "LT", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_lt", 5 },
    { PG_BUTTONS, IT_CHOICE, "RT", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_rt", 1 },
    { PG_BUTTONS, IT_CHOICE, "L3 (left stick)", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_l3", 6 },
    { PG_BUTTONS, IT_CHOICE, "R3 (right stick)", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_r3", 6 },
    { PG_BUTTONS, IT_CHOICE, "Back", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_back", 8 },
    { PG_BUTTONS, IT_CHOICE, "Start", BTN_HELP, CH_ACTION, NCH(CH_ACTION), 0, "controls.btn_start", 7 },
    { PG_BUTTONS, IT_SECTION, "Keyboard and mouse", NULL, NULL, 0, WL },
    { PG_BUTTONS, IT_KEY, "Move forward", KEY_HELP, NULL, 0, WL, "key_up" },
    { PG_BUTTONS, IT_KEY, "Move back", KEY_HELP, NULL, 0, WL, "key_down" },
    { PG_BUTTONS, IT_KEY, "Move left", KEY_HELP, NULL, 0, WL, "key_left" },
    { PG_BUTTONS, IT_KEY, "Move right", KEY_HELP, NULL, 0, WL, "key_right" },
    { PG_BUTTONS, IT_KEY, "Walk (hold)", KEY_HELP, NULL, 0, WL, "key_walk" },
    { PG_BUTTONS, IT_KEY, "Jump", KEY_HELP, NULL, 0, WL, "key_jump" },
    { PG_BUTTONS, IT_KEY, "Shoot", KEY_HELP, NULL, 0, WL, "key_shoot" },
    { PG_BUTTONS, IT_KEY, "Aim / lock-on", KEY_HELP, NULL, 0, WL, "key_aim" },
    { PG_BUTTONS, IT_KEY, "Dodge roll", KEY_HELP, NULL, 0, WL, "key_roll" },
    { PG_BUTTONS, IT_KEY, "Talk / check", KEY_HELP, NULL, 0, WL, "key_talk" },
    { PG_BUTTONS, IT_KEY, "Special weapon", KEY_HELP, NULL, 0, WL, "key_special" },
    { PG_BUTTONS, IT_KEY, "Game menu", KEY_HELP, NULL, 0, WL, "key_menu" },
    { PG_BUTTONS, IT_KEY, "Select", KEY_HELP, NULL, 0, WL, "key_select" },
    { PG_BUTTONS, IT_KEY, "Modern controls on/off", KEY_HELP, NULL, 0, WL, "key_orbit" },
    { PG_BUTTONS, IT_KEY_RESET, "Reset keys", "Puts every key above back to its default: WASD, Space, the mouse "
      "buttons and the rest.", NULL, 0, WL },
};

/* the tables in use: the in-game menu's, or the start menu's before the game */
static const item *g_items = g_game_items;
static int NITEMS = (int)(sizeof g_game_items / sizeof g_game_items[0]);
static const char *const *PAGES = GAME_PAGES;
static int NPAGES = 3;
#define MAX_ITEMS 128
/* (the per-item state below is MAX_ITEMS long: a table that outgrows it stops the build) */
typedef char start_items_fit[sizeof g_start_items / sizeof g_start_items[0] <= MAX_ITEMS ? 1 : -1];
typedef char game_items_fit[sizeof g_game_items / sizeof g_game_items[0] <= MAX_ITEMS ? 1 : -1];

static const int SPEEDS[] = { 50, 75, 100, 125, 150, 200 };
static const int MOUSE[] = { 0, 50, 75, 100, 150, 200 };
static const struct { const char *name; unsigned rgb; } RETICLES[] = {
    { "Yellow", 0xFFD840 }, { "Orange", 0xFF9A2E }, { "Red", 0xFF4040 }, { "Pink", 0xFF6EC7 },
    { "Purple", 0xB47CFF }, { "Blue", 0x4AA3FF }, { "Cyan", 0x3EE0E0 }, { "Green", 0x5BE36B },
    { "White", 0xFFFFFF },
};

static struct {
    int ready, open, page, cursor, block;
    int cap, cap_armed;                     /* the IT_KEY row waiting for a key (+ 1; 0 none); keys let go */   /* block: input held back until everything is released */
    int start;                              /* the start menu (before the game) */
    int play, quit;                         /* the start menu: Play / close the app */
    int on_play;                            /* the start menu: the PLAY button has the focus */
    int welcome, welcome_sel;               /* the start menu: the first-run import prompt (nothing imported); its button */
    WORD pad_now;                           /* the controller's buttons this frame (menu_pad_buttons) */
    double back_at;                         /* in the game: the last Back press (twice quits) */
    /* the start menu's game files (gamefiles.h) */
    int gf_ready[2], gf_state, gf_jp;       /* each version there (US, Japanese); the one importing */
    int edition;                            /* the version to play (gamefiles_edition) */
    float gf_progress;
    char gf_line[200];
    /* the US version's extras (gamefiles_task_poll): the one running (IT_GF_SUBS / IT_GF_VOICES, 0) */
    int tk_kind, tk_state, subs_there, subs_on, voices_built, voices_jp;
    float tk_progress;
    char tk_line[200];
    /* the start menu's updates (updater.h) */
    int up_state, up_started, up_manual;
    float up_progress;
    char up_line[200], up_ver[48];
    int scroll;                            /* the first row shown (long pages) */
    int chv[MAX_ITEMS];                     /* IT_CHOICE values, by item */
    int pad_ui;                             /* button prompts for the pad (else the keyboard) */
    char ini[MAX_PATH], game_dir[MAX_PATH];
    /* values */
    int fps60, vsync, res, msaa, shader_game_res, nearest, aniso, fog, lod_full, hdtex;
    int cam_speed, inv_x, inv_y, mouse, reticle, aim_camera, crosshair;
    int shader;                         /* index into presets, -1 = off */
    char shader_custom[MAX_PATH];       /* a preset from the ini outside the shaders folder */
    char **presets;
    int npresets;
    char note[160];
    double note_until;
    /* input */
    int prev_keys;
    int held_dir;
    double held_since, last_repeat;
    /* animation (draw side) */
    double last_draw, page_since, kick_at;
    float shown, sel_y;
    int kick_dir, sel_page;
    LARGE_INTEGER qpf;
} M;

static int gf_busy(void)
{
    return M.gf_state == IMPORT_PICKING || M.gf_state == IMPORT_COPYING;
}

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)M.qpf.QuadPart;
}

static void note(const char *msg)
{
    strcpy_s(M.note, sizeof M.note, msg);
    M.note_until = now_s() + 4.0;
}

static void ini_int(const char *sec, const char *key, int v)
{
    char b[16];
    _snprintf_s(b, sizeof b, _TRUNCATE, "%d", v);
    WritePrivateProfileStringA(sec, key, b, M.ini);
}

static void ini_str(const char *sec, const char *key, const char *v)
{
    WritePrivateProfileStringA(sec, key, v, M.ini);
}

/* one "section.key=value" of a choice (a leading "=": the item's own key) */
static const char *choice_pair(const item *it, const char *p, char *sec, char *key, char *val, size_t n)
{
    const char *full = p, *eq, *end, *dot;
    char own[96];
    if (*p == '=') {
        _snprintf_s(own, sizeof own, _TRUNCATE, "%s%.*s", it->key ? it->key : "", (int)strcspn(p, ";"), p);
        full = own;
    }
    end = full + strcspn(full, ";");
    eq = memchr(full, '=', (size_t)(end - full));
    dot = memchr(full, '.', (size_t)(end - full));
    if (!eq || !dot || dot > eq)
        return NULL;
    _snprintf_s(sec, n, _TRUNCATE, "%.*s", (int)(dot - full), full);
    _snprintf_s(key, n, _TRUNCATE, "%.*s", (int)(eq - dot - 1), dot + 1);
    _snprintf_s(val, n, _TRUNCATE, "%.*s", (int)(end - eq - 1), eq + 1);
    p += strcspn(p, ";");
    return *p == ';' ? p + 1 : p;
}

/* which choice mml.ini holds (by each choice's first value), else the item's default */
/* ---- the keyboard's keys (IT_KEY rows) ---- */
static const struct { const char *key, *def; } KEY_DEFS[] = {   /* (mml_gui.py KEY_DEFAULTS, controls.c) */
    { "key_up", "W,UP" }, { "key_down", "S,DOWN" }, { "key_left", "A,LEFT" }, { "key_right", "D,RIGHT" },
    { "key_walk", "LCTRL" }, { "key_jump", "SPACE" }, { "key_shoot", "MOUSE1" }, { "key_aim", "MOUSE2" },
    { "key_roll", "LSHIFT" }, { "key_talk", "E" }, { "key_special", "Q" }, { "key_menu", "TAB" },
    { "key_select", "M" }, { "key_orbit", "F5" },
};

static const char *key_default(const char *key)
{
    size_t i;
    for (i = 0; i < sizeof KEY_DEFS / sizeof KEY_DEFS[0]; i++)
        if (_stricmp(KEY_DEFS[i].key, key) == 0)
            return KEY_DEFS[i].def;
    return "";
}

static void key_trim_upper(char *s)
{
    char *a = s, *b;
    while (*a == ' ' || *a == '\t')
        a++;
    memmove(s, a, strlen(a) + 1);
    for (b = s + strlen(s); b > s && (b[-1] == ' ' || b[-1] == '\t'); b--)
        b[-1] = 0;
    for (b = s; *b; b++)
        if (*b >= 'a' && *b <= 'z')
            *b = (char)(*b - 'a' + 'A');
}

/* an IT_KEY row's two keys (names as mml.ini has them; the second may be "") */
static void key_get(const item *it, char *a, char *b, size_t n)
{
    char v[96], *c;
    GetPrivateProfileStringA("controls", it->key, key_default(it->key), v, sizeof v, M.ini);
    c = strchr(v, ',');
    if (c)
        *c++ = 0;
    strcpy_s(a, n, v);
    strcpy_s(b, n, c ? c : "");
    key_trim_upper(a);
    key_trim_upper(b);
}

/* a key's name as the screen shows it (mml_gui.py KEY_LABELS) */
static const char *key_label(const char *name)
{
    static const struct { const char *name, *label; } L[] = {
        { "MOUSE1", "Left click" }, { "MOUSE2", "Right click" }, { "MOUSE3", "Middle click" },
        { "MOUSE4", "Mouse 4" }, { "MOUSE5", "Mouse 5" }, { "LSHIFT", "Left Shift" }, { "RSHIFT", "Right Shift" },
        { "LCTRL", "Left Ctrl" }, { "RCTRL", "Right Ctrl" }, { "LALT", "Left Alt" }, { "RALT", "Right Alt" },
        { "SPACE", "Space" }, { "TAB", "Tab" }, { "ENTER", "Enter" }, { "BACKSPACE", "Backspace" },
        { "CAPSLOCK", "Caps Lock" }, { "UP", "Up" }, { "DOWN", "Down" }, { "LEFT", "Left" }, { "RIGHT", "Right" },
        { "PGUP", "Page Up" }, { "PGDN", "Page Down" }, { "INSERT", "Insert" }, { "DELETE", "Delete" },
        { "HOME", "Home" }, { "END", "End" },
    };
    size_t i;
    for (i = 0; i < sizeof L / sizeof L[0]; i++)
        if (_stricmp(L[i].name, name) == 0)
            return L[i].label;
    return name;
}

/* while an IT_KEY row waits: the first key or mouse button pressed after everything was let go
   becomes its first key; Esc (or B) gives up */
static void key_capture(WORD buttons)
{
    const item *it = &g_items[M.cap - 1];
    int vk, any = buttons != 0;
#ifdef _WIN32
    if (!kbm_foreground())
        return;
#endif
    if (M.cap_armed && (buttons & BTN_B)) {
        M.cap = 0;
        M.block = 1;
        return;
    }
    for (vk = 1; vk < 255; vk++) {
        char name[32], a[32], b[32], v[80];
        if (!(GetAsyncKeyState(vk) & 0x8000))
            continue;
        any = 1;
        if (!M.cap_armed)
            break;
        if (vk == VK_ESCAPE) {
            M.cap = 0;
            M.block = 1;
            return;
        }
        if (!kbm_vk_name(vk, name, sizeof name))
            continue;   /* (Shift, Ctrl and Alt also show as their either-side codes) */
        key_get(it, a, b, sizeof a);
        if (b[0] && _stricmp(b, name) != 0)
            _snprintf_s(v, sizeof v, _TRUNCATE, "%s,%s", name, b);
        else
            strcpy_s(v, sizeof v, name);
        WritePrivateProfileStringA("controls", it->key, v, M.ini);
        log_printf("menu: %s = %s", it->key, v);
        M.cap = 0;
        M.block = 1;   /* (that key doesn't also act on the menu) */
        return;
    }
    if (!any)
        M.cap_armed = 1;
}

static int choice_read(const item *it)
{
    int c;
    for (c = 0; c < it->nch; c++) {
        char sec[48], key[48], val[48], cur[64];
        if (!choice_pair(it, it->ch[c].set, sec, key, val, sizeof sec))
            continue;
        GetPrivateProfileStringA(sec, key, "\x01", cur, sizeof cur, M.ini);
        if (_stricmp(cur, val) == 0)
            return c;
    }
    return it->def;
}

static void choice_write(const item *it, int c)
{
    const char *p = it->ch[c].set;
    char sec[48], key[48], val[48];
    while (p && *p && (p = choice_pair(it, p, sec, key, val, sizeof sec)) != NULL)
        ini_str(sec, key, val);
}

static int item_index(const item *it)
{
    return (int)(it - g_items);
}

/* ---- shader presets under Modernization\shaders ---- */

static int by_name(const void *a, const void *b)
{
    return _stricmp(*(char *const *)a, *(char *const *)b);
}

static void scan_presets(const char *dir, int depth)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", dir);
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        char path[MAX_PATH];
        size_t n = strlen(fd.cFileName);
        if (fd.cFileName[0] == '.')
            continue;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* slang-shaders keeps its internal passes (and helper presets that need
               shaders we may not have) in folders called "shaders"; only list the
               user-facing presets */
            if (depth < 6 && _stricmp(fd.cFileName, "shaders") != 0)
                scan_presets(path, depth + 1);
        } else if (n > 7 && _stricmp(fd.cFileName + n - 7, ".slangp") == 0 && M.npresets < MAX_PRESETS) {
            M.presets[M.npresets++] = _strdup(path);
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static void full_path(const char *in, char *out, size_t n)
{
    if (!in[0])
        out[0] = 0;
    else if (in[1] == ':' || (in[0] == '\\' && in[1] == '\\'))
        strcpy_s(out, n, in);
    else
        _snprintf_s(out, n, _TRUNCATE, "%s\\%s", M.game_dir, in);
}

static const char *shader_path(void)
{
    if (M.shader >= 0 && M.shader < M.npresets)
        return M.presets[M.shader];
    return M.shader_custom;
}

/* ---- applying ---- */

static int effective_scale(void)
{
    /* CRT presets want the game's own resolution (the launcher's rule too) */
    if (shader_path()[0] && M.res == 0)
        return M.shader_game_res ? 1 : 0;
    return M.res;
}

static void apply(int kind)
{
    switch (kind) {
    case IT_FPS:
        if (!M.start)
            rec_enable(M.fps60);
        ini_int("video", "interp", M.fps60);
        break;
    case IT_VSYNC:
        gfx_set_vsync(M.vsync);
        ini_int("video", "vsync", M.vsync);
        break;
    case IT_RES:
    case IT_MSAA:
    case IT_SHADER_RES:
        gfx_set_scene(effective_scale(), M.msaa);
        ini_int("video", "render_scale", M.res);
        ini_int("video", "msaa", M.msaa);
        ini_int("video", "shader_scale", M.shader_game_res);
        if (M.msaa > 1)
            note("Antialiasing hides depth from ReShade effects");
        break;
    case IT_SHADER: {
        const char *p = shader_path();
        char rel[MAX_PATH];
        size_t gl = strlen(M.game_dir);
        if (!gfx_set_shader(p) && p[0])
            note("That preset failed to load (see mml.log)");
        gfx_set_scene(effective_scale(), M.msaa);
        if (p[0] && _strnicmp(p, M.game_dir, gl) == 0 && p[gl] == '\\')
            strcpy_s(rel, sizeof rel, p + gl + 1);   /* inside the game folder: relative, like the launcher */
        else
            strcpy_s(rel, sizeof rel, p);
        ini_str("video", "shader", rel);
        break;
    }
    case IT_UPSCALE:
        gfx_set_nearest_upscale(M.nearest);
        ini_int("video", "nearest_upscale", M.nearest);
        break;
    case IT_TEXFILTER:
        gfx_set_anisotropy(M.aniso ? 16 : 0);
        ini_int("video", "anisotropy", M.aniso ? 16 : 0);
        break;
    case IT_FOG:
        if (!M.start)
            fog_set_mode(M.fog);
        ini_str("video", "fog", M.fog == FOG_OFF ? "off" : M.fog == FOG_LIGHT ? "light" : "original");
        break;
    case IT_LOD:
        if (!M.start)
            lod_set(M.lod_full, !GetPrivateProfileIntA("debug", "near_subdiv", 0, M.ini));
        ini_str("video", "lod", M.lod_full ? "full" : "original");
        break;
    case IT_HDTEX:
        if (!M.start) {
            texreplace_set_replace(M.hdtex);
            ddshim_reupload_textures();
        }
        ini_int("textures", "replace", M.hdtex);
        break;
    case IT_CAMSPEED:
    case IT_INVERT_X:
    case IT_INVERT_Y:
        if (!M.start)
            controls_camera(SPEEDS[M.cam_speed], SPEEDS[M.cam_speed], M.inv_x, M.inv_y, 0, 0);
        ini_int("controls", "camera_speed_x", SPEEDS[M.cam_speed]);
        ini_int("controls", "camera_speed_y", SPEEDS[M.cam_speed]);
        ini_int("controls", "invert_x", M.inv_x);
        ini_int("controls", "invert_y", M.inv_y);
        if (kind != IT_INVERT_Y)
            break;
        /* like the launcher, invert Y covers the mouse too */
        ini_int("controls", "mouse_invert_y", M.inv_y);
        /* fall through */
    case IT_MOUSE:
        if (!M.start)
            controls_mouse(MOUSE[M.mouse] > 0, MOUSE[M.mouse] > 0 ? MOUSE[M.mouse] : 100, M.inv_y);
        ini_int("controls", "mouse_keyboard", MOUSE[M.mouse] > 0);
        if (MOUSE[M.mouse] > 0)
            ini_int("controls", "mouse_speed", MOUSE[M.mouse]);
        break;
    case IT_RETICLE: {
        char hex[8];
        if (!M.start)
            lockon_set_color(RETICLES[M.reticle].rgb);
        _snprintf_s(hex, sizeof hex, _TRUNCATE, "%06X", RETICLES[M.reticle].rgb);
        ini_str("controls", "reticle_color", hex);
        break;
    }
    case IT_AIM:
    case IT_CROSSHAIR:
        if (!M.start)
            controls_aim(M.aim_camera, M.crosshair);
        ini_str("controls", "aim", M.aim_camera ? "camera" : "character");
        ini_int("controls", "crosshair", M.crosshair);
        break;
    default:
        break;
    }
}

static int cycle(int v, int n, int dir)
{
    return (v + dir + n) % n;
}

static int nearest_index(const int *list, int n, int v)
{
    int i, best = 0;
    for (i = 0; i < n; i++)
        if (abs(list[i] - v) < abs(list[best] - v))
            best = i;
    return best;
}

static void change_item(const item *it, int dir);

static void change(int kind, int dir)
{
    switch (kind) {
    case IT_FPS: M.fps60 = !M.fps60; break;
    case IT_VSYNC: M.vsync = !M.vsync; break;
    case IT_RES: M.res = cycle(M.res, 5, dir); break;
    case IT_MSAA: M.msaa = M.msaa == 1 ? (dir > 0 ? 4 : 8) : M.msaa == 4 ? (dir > 0 ? 8 : 1) : (dir > 0 ? 1 : 4); break;
    case IT_SHADER:
        /* -1 = off, then every preset found */
        M.shader = M.npresets ? cycle(M.shader + 1, M.npresets + 1, dir) - 1 : -1;
        M.shader_custom[0] = 0;
        break;
    case IT_SHADER_RES: M.shader_game_res = !M.shader_game_res; break;
    case IT_UPSCALE: M.nearest = !M.nearest; break;
    case IT_TEXFILTER: M.aniso = !M.aniso; break;
    case IT_FOG: M.fog = cycle(M.fog, 3, dir); break;
    case IT_LOD: M.lod_full = !M.lod_full; break;
    case IT_HDTEX: M.hdtex = !M.hdtex; break;
    case IT_CAMSPEED: M.cam_speed = cycle(M.cam_speed, 6, dir); break;
    case IT_INVERT_X: M.inv_x = !M.inv_x; break;
    case IT_INVERT_Y: M.inv_y = !M.inv_y; break;
    case IT_MOUSE: M.mouse = cycle(M.mouse, 6, dir); break;
    case IT_RETICLE: M.reticle = cycle(M.reticle, (int)(sizeof RETICLES / sizeof RETICLES[0]), dir); break;
    case IT_AIM: M.aim_camera = !M.aim_camera; break;
    case IT_CROSSHAIR: M.crosshair = !M.crosshair; break;
    default: return;
    }
    M.kick_dir = dir;
    M.kick_at = now_s();
    apply(kind);
}

static void change_item(const item *it, int dir)
{
    if (it->kind == IT_GF_EDITION) {
        M.edition = !M.edition;
        WritePrivateProfileStringA("general", "edition", M.edition ? "japanese" : "us", M.ini);
        M.kick_dir = dir;
        M.kick_at = now_s();
        return;
    }
    if (it->kind == IT_GF_SUBS || it->kind == IT_GF_VOICES) {
        int subs = it->kind == IT_GF_SUBS;
        if (M.tk_state == TASK_RUNNING || gf_busy())
            return;
        if (subs ? M.subs_there : M.voices_built) {   /* there: on / off */
            if (subs) {
                M.subs_on = !M.subs_on;
                WritePrivateProfileStringA("general", "subtitles", M.subs_on ? "1" : "0", M.ini);
            } else {
                M.voices_jp = !M.voices_jp;
                WritePrivateProfileStringA("general", "voices", M.voices_jp ? "japanese" : "english", M.ini);
            }
            M.kick_dir = dir;
            M.kick_at = now_s();
        } else if (dir > 0 && !M.gf_ready[0]) {
            note("Import the US version first");
        } else if (dir > 0 && !subs && !M.gf_ready[1]) {
            note("Import the Japanese version first: the voices are built from it");
        } else if (dir > 0) {
            M.tk_kind = it->kind;
            M.tk_state = TASK_RUNNING;
            M.tk_progress = 0;
            M.tk_line[0] = 0;
            if (subs) {
                gamefiles_subtitles_begin(M.game_dir);
            } else {
                char jp[MAX_PATH];
                gamefiles_dir(M.game_dir, 1, jp, sizeof jp);
                gamefiles_voices_begin(M.game_dir, jp);
            }
        }
        return;
    }
    if (it->kind == IT_GF_US || it->kind == IT_GF_JP) {
        if (!gf_busy() && M.tk_state != TASK_RUNNING && dir > 0) {
            char d[MAX_PATH];
            M.gf_jp = it->kind == IT_GF_JP;
            M.gf_state = IMPORT_PICKING;
            M.gf_line[0] = 0;
            gamefiles_dir(M.game_dir, M.gf_jp, d, sizeof d);
            gamefiles_import_begin(d, M.gf_jp);
        }
        return;
    }
    if (it->kind == IT_UP_VERSION || it->kind == IT_UP_LATEST)
        return;
    if (it->kind == IT_UP_ACTION) {
        if (dir <= 0)
            return;
        if (M.up_state == UPD_AVAILABLE || M.up_state == UPD_READY) {
            update_install_begin();
        } else if (M.up_state != UPD_CHECKING && M.up_state != UPD_DOWNLOADING && M.up_state != UPD_INSTALLING) {
            M.up_manual = 1;
            update_check_begin();
        }
        M.up_started = 1;
        return;
    }
    if (it->kind == IT_KEY) {
        if (dir > 0) {
            M.cap = item_index(it) + 1;
            M.cap_armed = 0;
        }
        return;
    }
    if (it->kind == IT_KEY_RESET) {
        if (dir > 0) {
            size_t k;
            for (k = 0; k < sizeof KEY_DEFS / sizeof KEY_DEFS[0]; k++)
                WritePrivateProfileStringA("controls", KEY_DEFS[k].key, KEY_DEFS[k].def, M.ini);
            note("Every key is back to its default");
        }
        return;
    }
    if (it->kind != IT_CHOICE) {
        change(it->kind, dir);
        return;
    }
    M.chv[item_index(it)] = cycle(M.chv[item_index(it)], it->nch, dir);
    choice_write(it, M.chv[item_index(it)]);
    M.kick_dir = dir;
    M.kick_at = now_s();
}

static void value_text(int kind, char *out, size_t n)
{
    static const char *RES[] = { "Native", "1x (480p)", "2x (960p)", "3x (1440p)", "4x (1920p)" };
    static const char *FOG[] = { "Original", "Reduced", "Off" };
    switch (kind) {
    case IT_FPS: strcpy_s(out, n, M.fps60 ? "60 fps (interpolated)" : "30 fps (original)"); break;
    case IT_VSYNC: strcpy_s(out, n, M.vsync ? "On" : "Off"); break;
    case IT_RES: strcpy_s(out, n, RES[M.res]); break;
    case IT_MSAA: strcpy_s(out, n, M.msaa > 1 ? (M.msaa == 4 ? "MSAA 4x" : "MSAA 8x") : "Off"); break;
    case IT_SHADER: {
        const char *p = shader_path(), *b = strrchr(p, '\\');
        if (!p[0]) {
            strcpy_s(out, n, "Off");
            break;
        }
        strcpy_s(out, n, b ? b + 1 : p);
        if (strlen(out) > 7)
            out[strlen(out) - 7] = 0;   /* drop .slangp */
        break;
    }
    case IT_SHADER_RES: strcpy_s(out, n, M.shader_game_res ? "Native (480p)" : "Output"); break;
    case IT_UPSCALE: strcpy_s(out, n, M.nearest ? "Nearest neighbor" : "Bilinear"); break;
    case IT_TEXFILTER: strcpy_s(out, n, M.aniso ? "Anisotropic 16x" : "Bilinear"); break;
    case IT_FOG: strcpy_s(out, n, FOG[M.fog]); break;
    case IT_LOD: strcpy_s(out, n, M.lod_full ? "Maximum" : "Original"); break;
    case IT_HDTEX: strcpy_s(out, n, M.hdtex ? "On" : "Off"); break;
    case IT_CAMSPEED: _snprintf_s(out, n, _TRUNCATE, "%d%%", SPEEDS[M.cam_speed]); break;
    case IT_INVERT_X: strcpy_s(out, n, M.inv_x ? "On" : "Off"); break;
    case IT_INVERT_Y: strcpy_s(out, n, M.inv_y ? "On" : "Off"); break;
    case IT_MOUSE:
        if (MOUSE[M.mouse])
            _snprintf_s(out, n, _TRUNCATE, "%d%%", MOUSE[M.mouse]);
        else
            strcpy_s(out, n, "Off");
        break;
    case IT_RETICLE: strcpy_s(out, n, RETICLES[M.reticle].name); break;
    case IT_AIM: strcpy_s(out, n, M.aim_camera ? "Camera" : "Character"); break;
    case IT_CROSSHAIR: strcpy_s(out, n, M.crosshair ? "On" : "Off"); break;
    case IT_GF_EDITION: strcpy_s(out, n, M.edition ? "Rockman DASH (JP)" : "Mega Man Legends (US)"); break;
    case IT_KEY_RESET: strcpy_s(out, n, "Reset"); break;
    case IT_GF_US:
    case IT_GF_JP: {
        int k = kind == IT_GF_JP;
        if (gf_busy() && M.gf_jp == k && M.gf_state == IMPORT_COPYING)
            _snprintf_s(out, n, _TRUNCATE, "Copying %d%%", (int)(M.gf_progress * 100));
        else if (gf_busy() && M.gf_jp == k)
            strcpy_s(out, n, "Choosing a folder");
        else
            strcpy_s(out, n, M.gf_ready[k] ? "Ready" : "Not imported");
        break;
    }
    case IT_GF_SUBS:
        if (M.tk_state == TASK_RUNNING && M.tk_kind == kind)
            strcpy_s(out, n, "Downloading");
        else
            strcpy_s(out, n, !M.subs_there ? "Not downloaded" : M.subs_on ? "On" : "Off");
        break;
    case IT_GF_VOICES:
        if (M.tk_state == TASK_RUNNING && M.tk_kind == kind)
            _snprintf_s(out, n, _TRUNCATE, "Building %d%%", (int)(M.tk_progress * 100));
        else
            strcpy_s(out, n, !M.voices_built ? "Not built" : M.voices_jp ? "Japanese" : "English");
        break;
    case IT_UP_ACTION:
        switch (M.up_state) {
        case UPD_AVAILABLE: _snprintf_s(out, n, _TRUNCATE, "Install %s", M.up_ver); break;
        case UPD_READY: strcpy_s(out, n, "Install"); break;
        case UPD_UPTODATE: strcpy_s(out, n, "Check again"); break;
        case UPD_FAILED: strcpy_s(out, n, "Try again"); break;
        case UPD_CHECKING:
        case UPD_DOWNLOADING:
        case UPD_INSTALLING: strcpy_s(out, n, "Busy"); break;
        default: strcpy_s(out, n, "Check now"); break;
        }
        break;
    case IT_UP_VERSION: strcpy_s(out, n, update_current()); break;
    case IT_UP_LATEST:
        switch (M.up_state) {
        case UPD_CHECKING: strcpy_s(out, n, "Checking"); break;
        case UPD_UPTODATE: strcpy_s(out, n, "Up to date"); break;
        case UPD_AVAILABLE:
        case UPD_READY: strcpy_s(out, n, M.up_ver); break;
        case UPD_DOWNLOADING: _snprintf_s(out, n, _TRUNCATE, "Downloading %d%%", (int)(M.up_progress * 100)); break;
        case UPD_INSTALLING: strcpy_s(out, n, "Installing"); break;
        case UPD_FAILED: strcpy_s(out, n, "Failed"); break;
        default: strcpy_s(out, n, "Not checked"); break;
        }
        break;
    default: out[0] = 0; break;
    }
}

/* on/off rows get a status light; stepped ones an energy gauge */
static int toggle_state(int kind)
{
    switch (kind) {
    case IT_GF_US:
    case IT_GF_JP: return gf_busy() && M.gf_jp == (kind == IT_GF_JP) ? -1 : M.gf_ready[kind == IT_GF_JP];
    case IT_GF_SUBS: return M.tk_state == TASK_RUNNING && M.tk_kind == kind ? -1 : M.subs_there ? M.subs_on : 0;
    case IT_GF_VOICES: return M.tk_state == TASK_RUNNING && M.tk_kind == kind ? -1 : M.voices_built && M.voices_jp;
    case IT_VSYNC: return M.vsync;
    case IT_HDTEX: return M.hdtex;
    case IT_INVERT_X: return M.inv_x;
    case IT_INVERT_Y: return M.inv_y;
    default: return -1;
    }
}

static int gauge(int kind, int *total)
{
    switch (kind) {
    case IT_CAMSPEED: *total = 6; return M.cam_speed + 1;
    case IT_MOUSE: *total = 5; return M.mouse;
    case IT_MSAA: *total = 2; return M.msaa == 1 ? 0 : M.msaa == 4 ? 1 : 2;
    case IT_GF_US:
    case IT_GF_JP:
        if (M.gf_state != IMPORT_COPYING || M.gf_jp != (kind == IT_GF_JP))
            return -1;
        *total = 10;
        return (int)(M.gf_progress * 10 + 0.5f);
    case IT_GF_VOICES:
        if (M.tk_state != TASK_RUNNING || M.tk_kind != kind)
            return -1;
        *total = 10;
        return (int)(M.tk_progress * 10 + 0.5f);
    case IT_UP_LATEST:
        if (M.up_state != UPD_DOWNLOADING)
            return -1;
        *total = 10;
        return (int)(M.up_progress * 10 + 0.5f);
    default: return -1;
    }
}

/* ---- drawing ---- */

/* a pad button: round face with its letter */
static float pad_button(float x, float cy, float r, const char *label, uint32_t face)
{
    float tw = gfx_ui_font_width(UI_FONT_BODY, label, 0.62f);
    gfx_ui_circle(x + r, cy, r, ui_col(0xFF000000 | (face & 0xFFFFFF) ));
    gfx_ui_ring(x + r, cy, r, r * 0.16f, ui_col(0x60FFFFFF));
    ui_text(x + r - tw / 2, cy - gfx_ui_font_line(UI_FONT_BODY, 0.62f) / 2, label, 0xFFFFFFFF, 0.62f);
    return r * 2;
}

static float dpad_glyph(float x, float cy, float s)
{
    float t = s * 0.36f;
    gfx_ui_rect(x + (s - t) / 2, cy - s / 2, t, s, ui_col(0xFF2B3A50));
    gfx_ui_rect(x, cy - t / 2, s, t, ui_col(0xFF2B3A50));
    gfx_ui_rect(x + (s - t) / 2 + t * 0.25f, cy - s / 2 + t * 0.15f, t * 0.5f, t * 0.6f, ui_col(0xFF9FB3C8));
    gfx_ui_rect(x + (s - t) / 2 + t * 0.25f, cy + s / 2 - t * 0.75f, t * 0.5f, t * 0.6f, ui_col(0xFF9FB3C8));
    return s;
}

/* word-wrapped help text (draw 0: only measured); returns the height used */
static float wrap_text(float x, float y, float maxw, const char *s, uint32_t col, float scale, int draw)
{
    char buf[256];
    float lh = gfx_ui_font_line(UI_FONT_BODY, scale) * 1.05f, yy = y;
    while (*s) {
        int n = 0, last_space = -1, last_soft = -1;
        while (s[n]) {
            buf[n] = s[n];
            buf[n + 1] = 0;
            if (s[n] == ' ')
                last_space = n;
            if (gfx_ui_font_width(UI_FONT_BODY, buf, scale) > maxw && n > 0) {
                /* at the last space; a word wider than the line (a web address): after its last / or -,
                   else where it runs out */
                n = last_space > 0 ? last_space : last_soft > 0 ? last_soft + 1 : n;
                break;
            }
            if (s[n] == '/' || s[n] == '-')
                last_soft = n;
            if (++n >= (int)sizeof buf - 2)
                break;
        }
        buf[n] = 0;
        if (draw)
            ui_text(x, yy, buf, col, scale);
        yy += lh;
        s += n;
        while (*s == ' ')
            s++;
    }
    return yy - y;
}

static float g_help_h = 1e9f;   /* the room the help has (set where it is drawn) */

/* help text, smaller where it would run past its room */
static float wrapped(float x, float y, float maxw, const char *s, uint32_t col, float scale)
{
    while (scale > 0.5f && wrap_text(x, y, maxw, s, col, scale, 0) > g_help_h)
        scale -= 0.03f;
    return wrap_text(x, y, maxw, s, col, scale, 1);
}

/* the items a page shows here, in order (list may be NULL); their count */
/* a help text with its OK_KEY marks as the button the player uses now */
static void button_words(const char *src, char *out, size_t n)
{
    const char *key = M.pad_ui ? "A" : "Space";
    size_t o = 0, k;
    for (; *src && o + 1 < n; src++) {
        if (*src == OK_KEY[0]) {
            for (k = 0; key[k] && o + 1 < n; k++)
                out[o++] = key[k];
        } else
            out[o++] = *src;
    }
    out[o] = 0;
}

static int page_list(int page, int *list)
{
    int i, n = 0;
    for (i = 0; i < NITEMS; i++)
        if (g_items[i].page == page && (!g_items[i].plat || (g_items[i].plat & PL_HERE))) {
            if (list)
                list[n] = i;
            n++;
        }
    return n;
}

static int list_pos(const int *list, int n, int item)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == item)
            return i;
    return 0;
}

static float g_value_room = 9;   /* the widest a value gets, in body lines (the wide start menu: more) */

static void draw_value(const item *it, float vx, float cy, float u, int selected, double t)
{
    char v[MAX_PATH];
    float sc = 0.86f, th = gfx_ui_font_line(UI_FONT_BODY, sc), x = vx, kick = 0, aw = u * 0.42f;
    int on = toggle_state(it->kind), total = 0, filled = gauge(it->kind, &total);
    if (it->kind == IT_CHOICE)
        strcpy_s(v, sizeof v, it->ch[M.chv[item_index(it)]].label);
    else if (it->kind == IT_KEY && M.cap == item_index(it) + 1)
        strcpy_s(v, sizeof v, "Press a key");   /* (the help line says how to give up) */
    else if (it->kind == IT_KEY) {   /* its key(s): "W / Up" */
        char a[32], b[32];
        key_get(it, a, b, sizeof a);
        if (b[0])
            _snprintf_s(v, sizeof v, _TRUNCATE, "%s / %s", key_label(a), key_label(b));
        else
            strcpy_s(v, sizeof v, a[0] ? key_label(a) : "None");
    } else
        value_text(it->kind, v, sizeof v);
    if (selected) {
        double since = t - M.kick_at;
        if (since < 0.18)
            kick = (float)(1 - since / 0.18) * u * 0.22f;
        ui_arrow(x - aw / 2 + (M.kick_dir > 0 ? kick : 0), cy, aw, 1, COL_YELLOW);
        x -= aw + u * 0.35f;
    }
    if (filled >= 0) {   /* energy cells, filled from the left */
        float cw = u * 0.36f, ch = u * 0.62f, gap = u * 0.1f, gx;
        int i;
        x -= total * (cw + gap) - gap;
        gx = x;
        for (i = 0; i < total; i++) {
            float sx = gx + i * (cw + gap);
            if (i < filled) {
                ui_slant(sx, cy - ch / 2, cw, ch, ch * 0.25f, COL_GREEN_HI, COL_GREEN_LO);
            } else {
                ui_slant(sx, cy - ch / 2, cw, ch, ch * 0.25f, 0xFF2A3E5A, 0xFF1C2C44);
            }
        }
        x -= u * 0.4f;
    }
    if (on >= 0) {   /* status light */
        float r = u * 0.2f;
        x -= r * 2;
        if (on) {
            gfx_ui_circle(x + r, cy, r * 2.0f, ui_col(0x3062F06E));
            gfx_ui_circle(x + r, cy, r, ui_col(0xFF62F06E));
            gfx_ui_circle(x + r * 0.75f, cy - r * 0.3f, r * 0.35f, ui_col(0xC0FFFFFF));
        } else {
            gfx_ui_circle(x + r, cy, r, ui_col(0xFF3A4656));
        }
        x -= u * 0.3f;
    }
    if (it->kind == IT_RETICLE) {   /* a little lock-on marker in that colour */
        float r = u * 0.3f;
        uint32_t c = 0xFF000000 | RETICLES[M.reticle].rgb;
        x -= r * 2;
        gfx_ui_ring(x + r, cy, r, r * 0.25f, ui_col(c));
        ui_line(x + r, cy - r * 1.5f, x + r, cy - r * 0.6f, r * 0.25f, c);
        ui_line(x + r, cy + r * 0.6f, x + r, cy + r * 1.5f, r * 0.25f, c);
        ui_line(x - r * 0.5f, cy, x + r * 0.4f, cy, r * 0.25f, c);
        ui_line(x + r * 1.6f, cy, x + r * 2.5f, cy, r * 0.25f, c);
        x -= u * 0.45f;
    }
    if (it->kind == IT_SHADER && M.shader >= 0) {   /* preset number, small */
        char cnt[32];
        _snprintf_s(cnt, sizeof cnt, _TRUNCATE, "%d/%d", M.shader + 1, M.npresets);
        x -= gfx_ui_font_width(UI_FONT_BODY, cnt, 0.6f);
        ui_text(x, cy - gfx_ui_font_line(UI_FONT_BODY, 0.6f) / 2 + u * 0.04f, cnt, COL_DIM, 0.6f);
        x -= u * 0.3f;
    }
    {
        /* long preset names: trim from the end so they fit */
        float maxw = u * g_value_room;
        size_t n = strlen(v);
        while (n > 4 && gfx_ui_font_width(UI_FONT_BODY, v, sc) > maxw) {
            v[--n] = 0;
            v[n - 1] = v[n - 2] = '.';
        }
    }
    x -= gfx_ui_font_width(UI_FONT_BODY, v, sc);
    ui_text(x, cy - th / 2, v, selected ? 0xFFFFFFFF : (on == 0 ? COL_DIM : COL_TEXT), sc);
    if (selected)
        ui_arrow(x - u * 0.35f - aw / 2 - (M.kick_dir < 0 ? kick : 0), cy, aw, -1, COL_YELLOW);
}

static void draw_start_backdrop(float w, float h, float u, double t, int title);
static int selectable(int item);

/* ---- the start menu on a wide screen ----
   A small title at the top left, the page tabs across, the settings in a box on the left and the
   selected one's details in a box on the right (its full description, its choices, what a task is
   doing), the PLAY button under the details. Narrow screens keep the one-panel layout (draw). */

/* a box in the menu's style: the panel, its edges, the yellow brackets on two corners */
static void start_box(float x, float y, float w, float h, float u, float cut)
{
    float b = u * 0.6f, th = u * 0.1f, o = u * 0.02f;
    int i;
    ui_panel(x + u * 0.2f, y + u * 0.25f, w, h, cut, 0x60000000, 0x60000000);   /* drop shadow */
    ui_panel(x, y, w, h, cut, COL_PANEL_TOP, COL_PANEL_BOT);
    for (i = 0; i < (int)(h / 3); i++)   /* faint scanlines */
        gfx_ui_rect(x + (i * 3 > h - cut ? cut * (1 - (h - i * 3) / cut) : 0), y + i * 3,
                    w - (i * 3 < cut ? cut - i * 3 : 0) - (i * 3 > h - cut ? cut * (1 - (h - i * 3) / cut) : 0),
                    1, ui_col(0x0C000000));
    ui_panel_edge(x, y, w, h, cut, u * 0.08f, COL_EDGE);
    ui_panel_edge(x + u * 0.16f, y + u * 0.16f, w - u * 0.32f, h - u * 0.32f, cut * 0.85f, 1.0f, 0x405CC8FF);
    ui_line(x - o, y + b, x - o, y - o - th / 2, th, COL_YELLOW);
    ui_line(x - o - th / 2, y - o, x + b, y - o, th, COL_YELLOW);
    ui_line(x + w + o, y + h - b, x + w + o, y + h + o + th / 2, th, COL_YELLOW);
    ui_line(x + w + o + th / 2, y + h + o, x + w - b, y + h + o, th, COL_YELLOW);
}

/* the title at the top left: a Mega Man blue band with a yellow rule, the project's name beside */
static float start_title(float x, float y, float u, double t)
{
    const char *title = "MEGA MAN LEGENDS", *sub = "MODERNIZATION PROJECT";
    float ts = 0.8f, tw = gfx_ui_font_width(UI_FONT_DISPLAY, title, ts), band = u * 1.7f, bw = tw + u * 1.6f;
    float sweep = (float)fmod(t, 4.0) / 0.7f;
    ui_slant(x, y, bw, band, band * 0.4f, COL_BLUE, COL_BLUE_DARK);
    if (sweep < 1)   /* a glint across the band now and then */
        ui_slant(x + sweep * bw, y, band * 0.5f, band, band * 0.4f, 0x00FFFFFF, 0x40FFFFFF);
    gfx_ui_rect(x, y + band - u * 0.09f, bw - band * 0.3f, u * 0.09f, ui_col(COL_YELLOW));
    ui_text_outlined(UI_FONT_DISPLAY, x + u * 0.6f, y + (band - gfx_ui_font_line(UI_FONT_DISPLAY, ts)) / 2 - u * 0.05f,
                     title, 0xFFFFFFFF, 0xFF9EE2FF, ts, u * 0.06f, 0xFF061226);
    gfx_ui_font_text(UI_FONT_DISPLAY, x + bw + u * 0.3f, y + band - gfx_ui_font_line(UI_FONT_DISPLAY, 0.42f) - u * 0.12f,
                     sub, ui_col(0xFF8FD3FF), ui_col(0xFF8FD3FF), 0.42f);
    return band;
}

/* a section heading in the settings list */
static void start_section(float x, float y, float w, float rowh, float u, const char *label)
{
    float ls = 0.48f, lh = gfx_ui_font_line(UI_FONT_DISPLAY, ls), lw = gfx_ui_font_width(UI_FONT_DISPLAY, label, ls);
    float cy = y + rowh * 0.62f;
    gfx_ui_font_text(UI_FONT_DISPLAY, x, cy - lh / 2, label, ui_col(COL_YELLOW), ui_col(COL_YELLOW_LO), ls);
    gfx_ui_hgrad(x + lw + u * 0.4f, cy, w - lw - u * 0.4f, 1.5f, ui_col(0x805CC8FF), ui_col(0x005CC8FF));
}

/* the PLAY button (Start, or A with the focus on it) */
static void start_play(float x, float y, float w, float h, float u, double t)
{
    float pulse = 0.5f + 0.5f * sinf((float)t * 5.0f), lw = gfx_ui_font_width(UI_FONT_DISPLAY, "PLAY", 0.8f);
    float lh = gfx_ui_font_line(UI_FONT_DISPLAY, 0.8f), kh = u * 0.62f, gap = u * 0.6f, capw, lx;
    const char *cap = M.pad_ui ? "Start" : "Enter";
    /* the label and its key cap, centred together */
    capw = ui_cap_width(kh, cap);
    lx = x + (w - lw - gap - capw) / 2;
    if (M.on_play) {
        ui_slant(x - u * 0.12f, y - u * 0.12f, w + u * 0.24f, h + u * 0.24f, h * 0.45f,
                 ui_with_alpha(COL_YELLOW, 0.35f + 0.35f * pulse), 0x00FFD23A);
        ui_slant(x, y, w, h, h * 0.4f, COL_YELLOW, COL_YELLOW_LO);
        ui_arrow(x - u * 0.45f + sinf((float)t * 6.0f) * u * 0.08f, y + h / 2, u * 0.45f, 1, COL_YELLOW);
        gfx_ui_font_text(UI_FONT_DISPLAY, lx, y + (h - lh) / 2, "PLAY", ui_col(COL_NAVY),
                         ui_col(COL_NAVY), 0.8f);
    } else {
        ui_slant(x, y, w, h, h * 0.4f, COL_BLUE, COL_BLUE_DARK);
        gfx_ui_font_text(UI_FONT_DISPLAY, lx, y + (h - lh) / 2, "PLAY", ui_col(COL_YELLOW),
                         ui_col(COL_YELLOW_LO), 0.8f);
    }
    ui_key_cap(lx + lw + gap, y + h / 2, kh, cap);
}

/* a progress bar (0..1) */
static void start_bar(float x, float y, float w, float u, float f)
{
    if (f < 0)
        f = 0;
    if (f > 1)
        f = 1;
    gfx_ui_rect(x, y, w, u * 0.22f, ui_col(0xFF16304F));
    gfx_ui_rect(x, y, w * f, u * 0.22f, ui_col(COL_YELLOW));
}

static void draw_start(int w, int h, float u, double t, float dt)
{
    float m = u * 1.1f, x0 = m, x1 = (float)w - m, pad = u * 0.8f, cut = u * 0.8f;
    float tab_h = u * 1.35f, foot_h = u * 1.4f, rowh = u * 1.45f;
    float title_h, tabs_y, body_y, body_b, lx, lw, dx, dw, bh, rows_y, play_h = u * 1.9f;
    int list[MAX_ITEMS], n, i, shown_n, pos;
    draw_start_backdrop((float)w, (float)h, u, t, 0);
    title_h = start_title(x0, u * 0.7f, u, t);
    /* tabs */
    tabs_y = u * 0.7f + title_h + u * 0.6f;
    {
        float cy = tabs_y + tab_h / 2, kh = u * 0.62f, slant = tab_h * 0.35f, tw, tx, capw;
        capw = ui_key_cap(x0, cy, kh, M.pad_ui ? "LB" : "Q");
        tx = x0 + capw + u * 0.35f;
        tw = (x1 - tx - capw - u * 0.35f) / NPAGES;
        if (tw > u * 7.5f)
            tw = u * 7.5f;
        for (i = 0; i < NPAGES; i++) {
            float ls = 0.5f, room = tw - slant - u * 0.4f, sx = tx + i * tw, lw2, lh;
            lw2 = gfx_ui_font_width(UI_FONT_DISPLAY, PAGES[i], ls);
            if (lw2 > room && lw2 > 0) {
                ls *= room / lw2;
                lw2 = room;
            }
            lh = gfx_ui_font_line(UI_FONT_DISPLAY, ls);
            if (i == M.page)
                ui_slant(sx, tabs_y, tw - u * 0.15f, tab_h, slant, COL_YELLOW, COL_YELLOW_LO);
            else
                ui_slant(sx, tabs_y, tw - u * 0.15f, tab_h, slant, 0xFF183052, 0xFF122440);
            gfx_ui_font_text(UI_FONT_DISPLAY, sx + (tw + slant - lw2) / 2 - u * 0.1f, cy - lh / 2, PAGES[i],
                             ui_col(i == M.page ? COL_NAVY : COL_DIM), ui_col(i == M.page ? COL_NAVY : COL_DIM), ls);
            if (i == PAGE_FILES && (M.up_state == UPD_AVAILABLE || M.up_state == UPD_READY))   /* an update waits */
                gfx_ui_circle(sx + tw - u * 0.5f, tabs_y + u * 0.3f, u * 0.16f,
                              ui_col(i == M.page ? COL_NAVY : COL_YELLOW));
        }
        ui_key_cap(tx + NPAGES * tw + u * 0.2f, cy, kh, M.pad_ui ? "RB" : "E");
    }
    /* the two boxes */
    body_y = tabs_y + tab_h + u * 0.55f;
    body_b = (float)h - foot_h - u * 0.75f;
    bh = body_b - body_y;
    lx = x0;
    lw = (x1 - x0) * 0.6f;
    dx = lx + lw + u * 0.7f;
    dw = x1 - dx;
    start_box(lx, body_y, lw, bh, u, cut);
    start_box(dx, body_y, dw, bh, u, cut);

    /* the settings */
    rows_y = body_y + u * 0.45f;
    n = page_list(M.page, list);
    shown_n = (int)((bh - u * 0.9f) / rowh);
    if (shown_n > n)
        shown_n = n;
    pos = list_pos(list, n, M.cursor);
    if (pos < M.scroll)
        M.scroll = pos;
    if (pos >= M.scroll + shown_n)
        M.scroll = pos - shown_n + 1;
    if (M.scroll > 0 && M.scroll == pos && !selectable(list[pos - 1]))
        M.scroll--;   /* (the heading over the first row stays in view) */
    if (M.scroll > n - shown_n)
        M.scroll = n - shown_n;
    if (M.scroll < 0)
        M.scroll = 0;
    {
        float target = rows_y + (pos - M.scroll) * rowh, fade_in = ui_ease_out((float)((t - M.page_since) / 0.16));
        if (M.sel_page != M.page || M.sel_y <= 0) {
            M.sel_y = target;
            M.sel_page = M.page;
        }
        M.sel_y += (target - M.sel_y) * (dt * 20 > 1 ? 1 : dt * 20);
        if (!M.on_play) {
            float sy = M.sel_y, bob = sinf((float)t * 6.0f) * u * 0.08f, pulse = 0.75f + 0.25f * sinf((float)t * 4.0f);
            ui_slant(lx + pad * 0.6f, sy + u * 0.08f, lw - pad * 1.4f, rowh - u * 0.16f, u * 0.25f,
                     ui_with_alpha(0xFF2F8FE8, 0.85f * pulse), 0x302F8FE8);
            gfx_ui_rect(lx + pad * 0.6f, sy + u * 0.08f, u * 0.09f, rowh - u * 0.16f, ui_col(COL_YELLOW));
            ui_arrow(lx + pad * 0.35f + bob, sy + rowh / 2, u * 0.38f, 1, COL_YELLOW);
        }
        if (M.scroll > 0) {
            float ax = lx + lw / 2, ay = rows_y - u * 0.1f, s = u * 0.22f;
            gfx_ui_tri(ax, ay - s, ax + s, ay, ax - s, ay, ui_col(COL_DIM));
        }
        if (M.scroll + shown_n < n) {
            float ax = lx + lw / 2, ay = rows_y + shown_n * rowh + u * 0.05f, s = u * 0.22f;
            gfx_ui_tri(ax - s, ay, ax + s, ay, ax, ay + s, ui_col(COL_DIM));
        }
        g_value_room = lw * 0.5f / u;
        for (i = 0; i < shown_n; i++) {
            const item *it = &g_items[list[M.scroll + i]];
            float ry = rows_y + i * rowh + (1 - fade_in) * u * 0.3f * (i + 1), cy = ry + rowh / 2, save = ui_alpha;
            int sel = list[M.scroll + i] == M.cursor && !M.on_play;
            ui_alpha *= fade_in;
            if (it->kind == IT_SECTION) {
                start_section(lx + pad * 1.4f, ry, lw - pad * 2.6f, rowh, u, it->label);
            } else {
                if (i > 0 && g_items[list[M.scroll + i - 1]].kind != IT_SECTION)
                    gfx_ui_rect(lx + pad * 1.4f, ry, lw - pad * 2.6f, 1, ui_col(0x14FFFFFF));
                ui_text(lx + pad * 1.8f, cy - gfx_ui_font_line(UI_FONT_BODY, 1.0f) / 2, it->label,
                        sel ? 0xFFFFFFFF : 0xFFC4D3E2, 1.0f);
                draw_value(it, lx + lw - pad * 1.3f, cy, u, sel, t);
            }
            ui_alpha = save;
        }
        g_value_room = 9;
    }

    /* the details */
    {
        float ix = dx + pad, iw = dw - pad * 2, y = body_y + pad, play_y = body_b - pad - play_h, low = play_y - u * 1.0f;
        const item *it = M.cursor >= 0 && !M.on_play ? &g_items[M.cursor] : NULL;
        const char *name = it ? it->label : "PLAY";
        float ns = 0.7f, status_h = 0;
        char help[700], line[300] = "";
        uint32_t line_col = 0xFFB4C6D8;
        float progress = -1;
        /* what a task on this row is doing (the import, an extra, the update), or a warning */
        if (M.note[0] && t < M.note_until) {
            strcpy_s(line, sizeof line, M.note);
            line_col = COL_WARN;
        } else if (it && M.page == PAGE_FILES && it->kind >= IT_UP_ACTION && M.up_line[0]) {
            strcpy_s(line, sizeof line, M.up_line);
            line_col = M.up_state == UPD_FAILED ? COL_WARN : line_col;
            if (M.up_state == UPD_DOWNLOADING)
                progress = M.up_progress;
        } else if (M.page == PAGE_FILES && M.gf_line[0] &&
                   (gf_busy() || (it && it->kind == (M.gf_jp ? IT_GF_JP : IT_GF_US)))) {
            strcpy_s(line, sizeof line, M.gf_line);
            line_col = M.gf_state == IMPORT_FAILED ? COL_WARN : line_col;
            if (M.gf_state == IMPORT_COPYING)
                progress = M.gf_progress;
        } else if (it && M.page == PAGE_FILES && M.tk_line[0] && it->kind == M.tk_kind) {
            strcpy_s(line, sizeof line, M.tk_line);
            line_col = M.tk_state == TASK_FAILED ? COL_WARN : line_col;
            if (M.tk_state == TASK_RUNNING && M.tk_kind == IT_GF_VOICES)
                progress = M.tk_progress;
        }
        /* the name */
        if (gfx_ui_font_width(UI_FONT_DISPLAY, name, ns) > iw)
            ns *= iw / gfx_ui_font_width(UI_FONT_DISPLAY, name, ns);
        ui_text_outlined(UI_FONT_DISPLAY, ix, y, name, 0xFFFFFFFF, 0xFF9EE2FF, ns, u * 0.05f, 0xFF061226);
        y += gfx_ui_font_line(UI_FONT_DISPLAY, ns) + u * 0.25f;
        gfx_ui_hgrad(ix, y, iw, 1.5f, ui_col(0xA05CC8FF), ui_col(0x005CC8FF));
        y += u * 0.4f;
        /* the status line, above PLAY */
        if (line[0]) {
            float th = wrap_text(ix, 0, iw, line, line_col, 0.8f, 0);   /* (its own height: no shrinking) */
            status_h = th + (progress >= 0 ? u * 0.5f : 0);
            g_help_h = th + 1;
            {
                float sy = low - status_h;
                gfx_ui_rect(ix, sy - u * 0.25f, iw, 1, ui_col(0x24FFFFFF));
                wrapped(ix, sy, iw, line, line_col, 0.8f);
                if (progress >= 0)
                    start_bar(ix, low - u * 0.35f, iw, u, progress);
            }
            low -= status_h + u * 0.4f;
        }
        /* the choices (a short list): every one, the current one marked */
        if (it && ((it->kind == IT_CHOICE && it->nch <= 6) || it->kind == IT_GF_EDITION)) {
            static const char *const EDITIONS[] = { "Mega Man Legends (US)", "Rockman DASH (JP)" };
            int nch = it->kind == IT_CHOICE ? it->nch : 2;
            float ch_h = u * 0.95f, cy0 = low - nch * ch_h;
            int cur = it->kind == IT_CHOICE ? M.chv[item_index(it)] : M.edition, c;
            for (c = 0; c < nch; c++) {
                float cy = cy0 + c * ch_h;
                if (c == cur) {
                    gfx_ui_rect(ix, cy + u * 0.08f, iw, ch_h - u * 0.16f, ui_col(0x402F8FE8));
                    gfx_ui_rect(ix, cy + u * 0.08f, u * 0.08f, ch_h - u * 0.16f, ui_col(COL_YELLOW));
                }
                ui_text(ix + u * 0.4f, cy + (ch_h - gfx_ui_font_line(UI_FONT_BODY, 0.82f)) / 2,
                        it->kind == IT_CHOICE ? it->ch[c].label : EDITIONS[c],
                        c == cur ? COL_YELLOW : 0xFFC4D3E2, 0.82f);
            }
            low = cy0 - u * 0.4f;
        }
        /* the description: in what room is left (wrapped() shrinks it to fit) */
        g_help_h = low - y;
        if (it) {
            button_words(it->help ? it->help : "", help, sizeof help);
            wrapped(ix, y, iw, help, 0xFFD2DEEA, 0.86f);
        } else {
            wrapped(ix, y, iw, "Starts the game with these settings. They're saved, and the in-game menu "
                    "(Back + Start, or F1) changes most of them while you play.", 0xFFD2DEEA, 0.86f);
        }
        start_play(ix + u * 0.3f, play_y, iw - u * 0.6f, play_h, u, t);
    }

    /* button prompts, along the bottom */
    {
        float fy = (float)h - foot_h / 2 - u * 0.45f, fx = x0 + u * 0.2f, r = u * 0.36f, sc = 0.72f;
        float lh = gfx_ui_font_line(UI_FONT_BODY, sc);
        if (M.pad_ui) {
            fx += dpad_glyph(fx, fy, r * 2) + u * 0.3f;
            fx += ui_text(fx, fy - lh / 2, "Choose", COL_DIM, sc) + u * 1.0f;
            fx += pad_button(fx, fy, r, "A", 0xFF3FA34D) + u * 0.3f;
            fx += ui_text(fx, fy - lh / 2, "Change", COL_DIM, sc) + u * 1.0f;
            fx += ui_key_cap(fx, fy, r * 1.9f, "LB") + u * 0.2f;
            fx += ui_key_cap(fx, fy, r * 1.9f, "RB") + u * 0.3f;
            fx += ui_text(fx, fy - lh / 2, "Page", COL_DIM, sc) + u * 1.0f;
            fx += ui_key_cap(fx, fy, r * 1.9f, "Start") + u * 0.3f;
            fx += ui_text(fx, fy - lh / 2, "Play", COL_YELLOW, sc) + u * 1.0f;
            fx += pad_button(fx, fy, r, "B", 0xFFD8443C) + u * 0.3f;
            ui_text(fx, fy - lh / 2, "Quit", COL_DIM, sc);
        } else {
            static const char *const K[] = { "Arrows", "Choose", "Space", "Change", "Q / E", "Page", "Enter", "Play",
                                             "Esc", "Quit" };
            int k;
            for (k = 0; k < 5; k++) {
                fx += ui_key_cap(fx, fy, r * 1.9f, K[2 * k]) + u * 0.3f;
                fx += ui_text(fx, fy - lh / 2, K[2 * k + 1], k == 3 ? COL_YELLOW : COL_DIM, sc) + u * 1.0f;
            }
        }
    }
}

static void draw_welcome(int w, int h);
static void draw_menu(void *user, int w, int h);

static void draw(void *user, int w, int h)
{
    draw_menu(user, w, h);
    if (M.start && M.welcome && M.shown > 0)
        draw_welcome(w, h);
}

static void draw_menu(void *user, int w, int h)
{
    float u, pad, pw, ph, x, y, cut, rowh, head_h, tab_h, help_h, foot_h, rows_y, slide, dt;
    int list[MAX_ITEMS], n, i, maxrows = 0, fit;
    double t = now_s();
    (void)user;
    dt = M.last_draw > 0 ? (float)(t - M.last_draw) : 0;
    if (dt > 0.1f)
        dt = 0.1f;
    M.last_draw = t;
    /* open/close animation */
    if (M.open)
        M.shown = M.shown + dt / 0.18f > 1 ? 1 : M.shown + dt / 0.18f;
    else
        M.shown = M.shown - dt / 0.12f < 0 ? 0 : M.shown - dt / 0.12f;
    if (M.shown <= 0)
        return;
    ui_alpha = ui_ease_out(M.shown);

    u = gfx_ui_line(1.0f);
    if (M.start && w >= u * 34 && h >= u * 20) {   /* a wide screen: the two-box layout */
        draw_start(w, h, u, t, dt);
        return;
    }
    pad = u * 0.75f;
    cut = u * 0.9f;
    rowh = u * 1.3f;
    head_h = u * 2.3f;
    tab_h = u * 1.25f;
    help_h = u * 2.5f;
    foot_h = u * 1.4f;
    for (i = 0; i < NPAGES; i++) {
        n = page_list(i, NULL);
        if (n > maxrows)
            maxrows = n;
    }
    /* as many rows as the screen holds (the start menu leaves room for its title); longer
       pages scroll */
    /* (the start menu: the title above, 4.6 lines, the PLAY button and the stripes below) */
    fit = (int)((h - (M.start ? u * 4.6f + u * 2.0f + u * 0.8f : u * 1.0f) -
                 (head_h + tab_h + u * 0.9f + help_h + foot_h + pad)) / rowh);
    if (fit < 4)
        fit = 4;
    if (maxrows > fit)
        maxrows = fit;
    pw = u * 19;
    if (pw > w - 2 * u)
        pw = w - 2 * u;
    ph = head_h + tab_h + u * 0.5f + maxrows * rowh + u * 0.4f + help_h + foot_h + pad +
         (M.start ? u * 2.0f : 0);   /* (the start menu's PLAY button under the rows) */
    slide = (1 - ui_alpha) * u * 4;
    if (M.start) {   /* centred, under the title */
        draw_start_backdrop((float)w, (float)h, u, t, 1);
        x = (w - pw) / 2;
        y = u * 4.6f + (h - u * 4.6f - u * 0.6f - ph) / 2;
    } else {
        x = u * 1.2f - slide;
        y = (h - ph) / 2;
        /* dim the game behind the panel so the text reads, but keep the right side clear for comparing */
        gfx_ui_hgrad(0, 0, w * 0.62f, (float)h, ui_col(0x99030812), ui_col(0x00030812));
    }
    if (y < u * 0.5f)
        y = u * 0.5f;

    /* body */
    ui_panel(x + u * 0.25f, y + u * 0.3f, pw, ph, cut, 0x60000000, 0x60000000);   /* drop shadow */
    ui_panel(x, y, pw, ph, cut, COL_PANEL_TOP, COL_PANEL_BOT);
    for (i = 0; i < (int)(ph / 3); i++)   /* faint scanlines */
        gfx_ui_rect(x + (i * 3 > ph - cut ? cut * (1 - (ph - i * 3) / cut) : 0), y + i * 3,
                    pw - (i * 3 < cut ? cut - i * 3 : 0) - (i * 3 > ph - cut ? cut * (1 - (ph - i * 3) / cut) : 0),
                    1, ui_col(0x0C000000));
    ui_panel_edge(x, y, pw, ph, cut, u * 0.08f, COL_EDGE);
    ui_panel_edge(x + u * 0.18f, y + u * 0.18f, pw - u * 0.36f, ph - u * 0.36f, cut * 0.85f, 1.0f, 0x405CC8FF);
    {   /* yellow brackets on the two square corners */
        float b = u * 0.7f, th = u * 0.12f, o = u * 0.02f;
        ui_line(x - o, y + b, x - o, y - o - th / 2, th, COL_YELLOW);
        ui_line(x - o - th / 2, y - o, x + b, y - o, th, COL_YELLOW);
        ui_line(x + pw + o, y + ph - b, x + pw + o, y + ph + o + th / 2, th, COL_YELLOW);
        ui_line(x + pw + o + th / 2, y + ph + o, x + pw - b, y + ph + o, th, COL_YELLOW);
    }

    /* header: a Mega Man blue band with the title, and a strip of hazard stripes */
    {
        float bx = x + u * 0.18f, by = y + u * 0.18f, bw = pw * 0.66f, bh = head_h - u * 0.55f;
        float ts = 0.62f, tx = x + pad, ty;
        float xy[8], sweep;
        uint32_t c[4];
        /* square on the left (inside the frame), cut on the right */
        xy[0] = bx; xy[1] = by;
        xy[2] = bx + bw; xy[3] = by;
        xy[4] = bx + bw - bh * 0.5f; xy[5] = by + bh;
        xy[6] = bx; xy[7] = by + bh;
        c[0] = ui_col(COL_BLUE); c[1] = ui_col(COL_BLUE_DARK); c[2] = ui_col(COL_BLUE_DARK); c[3] = ui_col(0xFF1854B8);
        gfx_ui_quad(xy, c);
        /* a glint that runs across the band every few seconds */
        sweep = (float)fmod(t, 3.5) / 0.6f;
        if (sweep < 1) {
            float gx = bx + sweep * (bw + bh), gw = bh * 0.5f;
            if (gx + gw < bx + bw - bh * 0.5f)
                ui_slant(gx, by, gw, bh, bh * 0.5f, 0x00FFFFFF, 0x50FFFFFF);
        }
        gfx_ui_rect(bx, by + bh - u * 0.08f, bw - bh * 0.5f, u * 0.08f, ui_col(COL_YELLOW));
        ty = by + (bh - gfx_ui_font_line(UI_FONT_DISPLAY, ts)) / 2;
        ui_text_outlined(UI_FONT_DISPLAY, tx, ty, M.start ? "OPTIONS" : "SETTINGS", 0xFFFFFFFF, 0xFF9EE2FF, ts,
                         u * 0.06f, 0xFF061226);
        /* hazard stripes */
        {
            float sx0 = bx + bw + u * 0.2f, sx1 = x + pw - cut - u * 0.1f, sh = u * 0.32f, sy = by + bh - sh;
            float step = sh * 1.6f, sxx;
            for (sxx = sx0; sxx + step <= sx1; sxx += step)
                ui_slant(sxx, sy, step * 0.5f, sh, sh, COL_YELLOW, COL_YELLOW_LO);
            ui_text(sx0, by + u * 0.05f, M.start ? "saved for the game" : "changes apply live", COL_DIM, 0.58f);
        }
    }

    /* page tabs, LB / RB on either side */
    {
        float ty = y + head_h, cy = ty + tab_h / 2, capw, slant = tab_h * 0.35f, tx, tw;
        float kh = u * 0.62f;
        capw = M.pad_ui ? ui_key_cap(x + pad, cy, kh, "LB") : ui_key_cap(x + pad, cy, kh, "Q");
        if (M.pad_ui)
            ui_key_cap(x + pw - pad - capw, cy, kh, "RB");
        else
            ui_key_cap(x + pw - pad - capw, cy, kh, "E");
        tx = x + pad + capw + u * 0.35f;
        tw = (pw - 2 * pad - 2 * capw - u * 0.7f - slant) / NPAGES;
        for (i = 0; i < NPAGES; i++) {
            float ls = 0.42f, room = tw - slant - u * 0.3f, sx = tx + i * tw, lw, lh;
            lw = gfx_ui_font_width(UI_FONT_DISPLAY, PAGES[i], ls);
            if (lw > room && lw > 0) {   /* a wide font: the label shrinks to fit its tab */
                ls *= room / lw;
                lw = room;
            }
            lh = gfx_ui_font_line(UI_FONT_DISPLAY, ls);
            if (i == M.page) {
                ui_slant(sx, ty + u * 0.12f, tw - u * 0.12f, tab_h - u * 0.24f, slant, COL_YELLOW, COL_YELLOW_LO);
                gfx_ui_font_text(UI_FONT_DISPLAY, sx + (tw + slant - lw) / 2 - u * 0.06f, cy - lh / 2, PAGES[i],
                                 ui_col(COL_NAVY), ui_col(COL_NAVY), ls);
            } else {
                ui_slant(sx, ty + u * 0.12f, tw - u * 0.12f, tab_h - u * 0.24f, slant, 0xFF183052, 0xFF122440);
                gfx_ui_font_text(UI_FONT_DISPLAY, sx + (tw + slant - lw) / 2 - u * 0.06f, cy - lh / 2, PAGES[i],
                                 ui_col(COL_DIM), ui_col(COL_DIM), ls);
            }
            if (M.start && i == PAGE_FILES && (M.up_state == UPD_AVAILABLE || M.up_state == UPD_READY))   /* an update waits */
                gfx_ui_circle(sx + tw - u * 0.3f, ty + u * 0.3f, u * 0.16f, ui_col(i == M.page ? COL_NAVY : COL_YELLOW));
        }
    }

    /* rows */
    rows_y = y + head_h + tab_h + u * 0.5f;
    n = page_list(M.page, list);
    {
        int pos = list_pos(list, n, M.cursor), shown_n = n < maxrows ? n : maxrows;
        float target, fade_in;
        /* keep the selection in view */
        if (pos < M.scroll)
            M.scroll = pos;
        if (pos >= M.scroll + shown_n)
            M.scroll = pos - shown_n + 1;
        if (M.scroll > n - shown_n)
            M.scroll = n - shown_n;
        if (M.scroll < 0)
            M.scroll = 0;
        target = rows_y + (pos - M.scroll) * rowh;
        if (M.sel_page != M.page || M.sel_y <= 0) {
            M.sel_y = target;
            M.sel_page = M.page;
        }
        M.sel_y += (target - M.sel_y) * (dt * 20 > 1 ? 1 : dt * 20);
        fade_in = ui_ease_out((float)((t - M.page_since) / 0.16));
        /* selection: a slanted light bar with the yellow pointer at its left */
        if (!(M.start && M.on_play)) {
            float sy = M.sel_y, bob = sinf((float)t * 6.0f) * u * 0.08f;
            float pulse = 0.75f + 0.25f * sinf((float)t * 4.0f);
            ui_slant(x + pad * 0.6f, sy + u * 0.08f, pw - pad * 1.4f, rowh - u * 0.16f, u * 0.25f,
                      ui_with_alpha(0xFF2F8FE8, 0.85f * pulse), 0x302F8FE8);
            gfx_ui_rect(x + pad * 0.6f, sy + u * 0.08f, u * 0.09f, rowh - u * 0.16f, ui_col(COL_YELLOW));
            ui_arrow(x + pad * 0.35f + bob, sy + rowh / 2, u * 0.38f, 1, COL_YELLOW);
        }
        if (M.scroll > 0) {   /* more above / below */
            float ax = x + pw / 2, ay = rows_y - u * 0.05f, s = u * 0.22f;
            gfx_ui_tri(ax, ay - s, ax + s, ay, ax - s, ay, ui_col(COL_DIM));
        }
        if (M.scroll + shown_n < n) {
            float ax = x + pw / 2, ay = rows_y + shown_n * rowh + u * 0.05f, s = u * 0.22f;
            gfx_ui_tri(ax - s, ay, ax + s, ay, ax, ay + s, ui_col(COL_DIM));
        }
        for (i = 0; i < shown_n; i++) {
            const item *it = &g_items[list[M.scroll + i]];
            float ry = rows_y + i * rowh + (1 - fade_in) * u * 0.3f * (i + 1), cy = ry + rowh / 2;
            float save = ui_alpha;
            int sel = list[M.scroll + i] == M.cursor && !(M.start && M.on_play);
            ui_alpha *= fade_in;
            if (it->kind == IT_SECTION) {
                start_section(x + pad * 1.2f, ry, pw - pad * 2.4f, rowh, u, it->label);
            } else {
                if (i > 0 && g_items[list[M.scroll + i - 1]].kind != IT_SECTION)
                    gfx_ui_rect(x + pad * 1.2f, ry, pw - pad * 2.2f, 1, ui_col(0x14FFFFFF));
                ui_text(x + pad * 1.6f, cy - gfx_ui_font_line(UI_FONT_BODY, 0.9f) / 2, it->label,
                        sel ? 0xFFFFFFFF : 0xFFC4D3E2, 0.9f);
                draw_value(it, x + pw - pad * 1.2f, cy, u, sel, t);
            }
            ui_alpha = save;
        }
    }

    if (M.start) {   /* the PLAY button: Start, or A with the focus on it */
        float bh = u * 1.5f, bw = pw * 0.5f, bx = x + (pw - bw) / 2, by = rows_y + maxrows * rowh + u * 0.35f;
        float pulse = 0.5f + 0.5f * sinf((float)t * 5.0f), lw = gfx_ui_font_width(UI_FONT_DISPLAY, "PLAY", 0.75f);
        float lh = gfx_ui_font_line(UI_FONT_DISPLAY, 0.75f);
        if (M.on_play) {
            ui_slant(bx - u * 0.12f, by - u * 0.12f, bw + u * 0.24f, bh + u * 0.24f, bh * 0.45f,
                     ui_with_alpha(COL_YELLOW, 0.35f + 0.35f * pulse), 0x00FFD23A);
            ui_slant(bx, by, bw, bh, bh * 0.4f, COL_YELLOW, COL_YELLOW_LO);
            ui_arrow(bx - u * 0.45f + sinf((float)t * 6.0f) * u * 0.08f, by + bh / 2, u * 0.45f, 1, COL_YELLOW);
            gfx_ui_font_text(UI_FONT_DISPLAY, bx + (bw - lw) / 2 - u * 0.5f, by + (bh - lh) / 2, "PLAY",
                             ui_col(COL_NAVY), ui_col(COL_NAVY), 0.75f);
        } else {
            ui_slant(bx, by, bw, bh, bh * 0.4f, COL_BLUE, COL_BLUE_DARK);
            gfx_ui_font_text(UI_FONT_DISPLAY, bx + (bw - lw) / 2 - u * 0.5f, by + (bh - lh) / 2, "PLAY",
                             ui_col(COL_YELLOW), ui_col(COL_YELLOW_LO), 0.75f);
        }
        if (M.pad_ui)
            ui_key_cap(bx + (bw + lw) / 2 - u * 0.2f, by + bh / 2, u * 0.6f, "Start");
        else
            ui_key_cap(bx + (bw + lw) / 2 - u * 0.2f, by + bh / 2, u * 0.6f, "Enter");
        rows_y += u * 2.0f;   /* (the help goes below it) */
    }

    /* help for the selected setting (or a warning) */
    {
        float hy = rows_y + maxrows * rowh + u * 0.4f, hx = x + pad * 1.2f, hw = pw - pad * 2.4f;
        gfx_ui_hgrad(hx, hy, hw, 1.5f, ui_col(0xA05CC8FF), ui_col(0x005CC8FF));
        hy += u * 0.35f;
        g_help_h = (y + ph - foot_h - pad * 0.5f) - hy - u * 0.1f;   /* (down to the button prompts' line) */
        if (M.note[0] && t < M.note_until) {
            float s = u * 0.55f;
            gfx_ui_tri(hx + s / 2, hy + u * 0.05f, hx + s, hy + s + u * 0.05f, hx, hy + s + u * 0.05f, ui_col(COL_WARN));
            ui_text(hx + s * 0.38f, hy + s * 0.18f, "!", COL_NAVY, 0.5f);
            wrapped(hx + s + u * 0.3f, hy, hw - s - u * 0.3f, M.note, COL_WARN, 0.72f);
        } else if (M.start && M.page == PAGE_FILES && M.up_line[0] && !M.on_play && M.cursor >= 0 &&
                   g_items[M.cursor].kind >= IT_UP_ACTION) {
            /* (the update's progress or outcome, on its rows) */
            wrapped(hx, hy, hw, M.up_line, M.up_state == UPD_FAILED ? COL_WARN : 0xFFB4C6D8, 0.72f);
        } else if (M.start && M.page == PAGE_FILES && M.gf_line[0] && !M.on_play &&
                   (gf_busy() || (M.cursor >= 0 && g_items[M.cursor].kind == (M.gf_jp ? IT_GF_JP : IT_GF_US)))) {
            /* (the import's progress or outcome, on its version's row) */
            wrapped(hx, hy, hw, M.gf_line, M.gf_state == IMPORT_FAILED ? COL_WARN : 0xFFB4C6D8, 0.72f);
        } else if (M.start && M.page == PAGE_FILES && M.tk_line[0] && !M.on_play && M.cursor >= 0 &&
                   g_items[M.cursor].kind == M.tk_kind) {
            /* (an extra's progress or outcome, on its row) */
            wrapped(hx, hy, hw, M.tk_line, M.tk_state == TASK_FAILED ? COL_WARN : 0xFFB4C6D8, 0.72f);
        } else if (M.start && M.on_play) {
            wrapped(hx, hy, hw, "Starts the game with these settings. They're saved, and the in-game menu "
                    "(Back + Start) changes most of them while you play.", 0xFFB4C6D8, 0.72f);
        } else if (M.cursor >= 0) {
            char help[600];
            button_words(g_items[M.cursor].help, help, sizeof help);
            wrapped(hx, hy, hw, help, 0xFFB4C6D8, 0.72f);
        }
    }

    /* button prompts */
    {
        float fy = y + ph - foot_h / 2 - pad * 0.5f, fx = x + pad * 1.2f, r = u * 0.34f, sc = 0.68f;
        float lh = gfx_ui_font_line(UI_FONT_BODY, sc);
        gfx_ui_rect(x + pad * 1.2f, fy - foot_h / 2, pw - pad * 2.4f, 1, ui_col(0x14FFFFFF));
        if (M.pad_ui) {
            fx += dpad_glyph(fx, fy, r * 2) + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Choose", COL_DIM, sc) + u * 0.7f;
            fx += pad_button(fx, fy, r, "A", 0xFF3FA34D) + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Change", COL_DIM, sc) + u * 0.7f;
            if (M.start) {
                fx += ui_key_cap(fx, fy, r * 1.9f, "Start") + u * 0.25f;
                fx += ui_text(fx, fy - lh / 2, "Play", COL_YELLOW, sc) + u * 0.7f;
                fx += pad_button(fx, fy, r, "B", 0xFFD8443C) + u * 0.25f;
                fx += ui_text(fx, fy - lh / 2, "Quit", COL_DIM, sc) + u * 0.7f;
            } else {
                fx += pad_button(fx, fy, r, "B", 0xFFD8443C) + u * 0.25f;
                fx += ui_text(fx, fy - lh / 2, "Close", COL_DIM, sc) + u * 0.7f;
            }
        } else {   /* the keyboard (start menu: Space is A, Enter is Start, Esc is B) */
            static const char *const START_KEYS[] = { "Arrows", "Choose", "Space", "Change", "Enter", "Play",
                                                      "Esc", "Quit" };
            static const char *const GAME_KEYS[] = { "Arrows", "Choose", "Enter", "Change", "Esc", "Close" };
            const char *const *kv = M.start ? START_KEYS : GAME_KEYS;
            int pairs = M.start ? 4 : 3, k;
            float caps = 0, rest = 0, gap = u * 0.7f, room = pw - pad * 2.4f;
            for (k = 0; k < pairs; k++) {   /* a wide system font: the labels and gaps give way (caps stay) */
                caps += ui_cap_width(r * 1.9f, kv[2 * k]) + u * 0.25f;
                rest += gfx_ui_font_width(UI_FONT_BODY, kv[2 * k + 1], sc) + (k + 1 < pairs ? gap : 0);
            }
            if (caps + rest > room && rest > 0 && room > caps) {
                float shrink = (room - caps) / rest;
                sc *= shrink;
                gap *= shrink;
                lh = gfx_ui_font_line(UI_FONT_BODY, sc);
            }
            for (k = 0; k < pairs; k++) {
                fx += ui_key_cap(fx, fy, r * 1.9f, kv[2 * k]) + u * 0.25f;
                fx += ui_text(fx, fy - lh / 2, kv[2 * k + 1], M.start && k == 2 ? COL_YELLOW : COL_DIM, sc) + gap;
            }
        }
    }
}

/* ---- input ---- */

enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_OK = 16, K_BACK = 32, K_TOGGLE = 64,
       K_PREV = 128, K_NEXT = 256, K_PLAY = 512 };

static int read_keys(WORD *buttons)
{
    pad_t p;
    int conn, k = 0, kb = 0;
    memset(&p, 0, sizeof p);
    conn = input_sdl_snapshot(&p);
    conn = controls_debug_overlay(&p, conn);
    *buttons = conn ? p.wButtons : 0;
    if (conn) {
        if ((p.wButtons & BTN_DPAD_UP) || p.sThumbLY > 16000) k |= K_UP;
        if ((p.wButtons & BTN_DPAD_DOWN) || p.sThumbLY < -16000) k |= K_DOWN;
        if ((p.wButtons & BTN_DPAD_LEFT) || p.sThumbLX < -16000) k |= K_LEFT;
        if ((p.wButtons & BTN_DPAD_RIGHT) || p.sThumbLX > 16000) k |= K_RIGHT;
        if (p.wButtons & BTN_A) k |= K_OK;
        if (p.wButtons & BTN_B) k |= K_BACK;
        if (p.wButtons & BTN_LB) k |= K_PREV;
        if (p.wButtons & BTN_RB) k |= K_NEXT;
        if ((p.wButtons & (BTN_BACK | BTN_START)) == (BTN_BACK | BTN_START)) k |= K_TOGGLE;
        else if (M.start && (p.wButtons & BTN_START)) k |= K_PLAY;
        if (k)
            M.pad_ui = 1;
    }
#ifdef _WIN32
    if (kbm_foreground()) {
#else
    {   /* (SDL keeps the keyboard's state only while its window has the focus) */
#endif
#define KEY(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
        if (KEY(VK_UP)) kb |= K_UP;
        if (KEY(VK_DOWN)) kb |= K_DOWN;
        if (KEY(VK_LEFT)) kb |= K_LEFT;
        if (KEY(VK_RIGHT)) kb |= K_RIGHT;
        if (M.start && KEY(VK_RETURN)) kb |= K_PLAY;
        else if (KEY(VK_RETURN) || KEY(VK_SPACE)) kb |= K_OK;
        if (KEY(VK_ESCAPE) || KEY(VK_BACK)) kb |= K_BACK;
        if (KEY('Q') || KEY(VK_PRIOR)) kb |= K_PREV;
        if (KEY('E') || KEY(VK_NEXT)) kb |= K_NEXT;
        if (KEY(VK_F1)) kb |= K_TOGGLE;
#undef KEY
        if (kb)
            M.pad_ui = 0;
        k |= kb;
    }
    return k;
}

/* a row the cursor can be on (section headings are not) */
static int selectable(int item)
{
    return g_items[item].kind != IT_SECTION;
}

/* the list position of the first (dir > 0) or last (dir < 0) selectable row; -1 if none */
static int end_row(const int *list, int n, int dir)
{
    int k;
    for (k = 0; k < n; k++) {
        int pos = dir > 0 ? k : n - 1 - k;
        if (selectable(list[pos]))
            return pos;
    }
    return -1;
}

static void set_page(int page)
{
    int list[MAX_ITEMS], n, first;
    M.on_play = 0;
    M.page = (page + NPAGES) % NPAGES;
    n = page_list(M.page, list);
    first = end_row(list, n, 1);
    M.cursor = first >= 0 ? list[first] : 0;
    M.scroll = 0;
    M.page_since = now_s();
}

static void move_cursor(int dir)
{
    int list[MAX_ITEMS], n = page_list(M.page, list), pos, first = end_row(list, n, 1), last = end_row(list, n, -1);
    if (first < 0)
        return;
    pos = list_pos(list, n, M.cursor);
    if (M.start) {   /* rows, then PLAY, then round again */
        if (M.on_play) {
            M.on_play = 0;
            M.cursor = list[dir > 0 ? first : last];
            return;
        }
        if ((dir > 0 && pos >= last) || (dir < 0 && pos <= first)) {
            M.on_play = 1;
            return;
        }
    }
    do   /* (over the headings) */
        pos = (pos + dir + n) % n;
    while (!selectable(list[pos]));
    M.cursor = list[pos];
}

static void set_open(int open)
{
    M.open = open;
    M.block = 1;   /* until every key/button is let go: nothing leaks into the game */
    if (open) {
        M.page_since = now_s();
        M.sel_y = 0;
    }
    log_printf("menu: %s", open ? "open" : "closed");
}

/* ---- the first run: nothing imported yet ----
   Before anything else, a prompt over the menu says what the app needs (the player's own copy of
   the game) and offers the two imports, or Later. It comes up whenever no version is there, so an
   updated app, or a folder Setup patched in place, never sees it. */

static const char *const WELCOME_BTN[] = { "Import Mega Man Legends (US)", "Import Rockman DASH (JP)", "Later" };

/* the GAME page's row of this kind shown here (the importers have a row per platform) */
static int start_row(int kind)
{
    int list[MAX_ITEMS], n = page_list(PAGE_FILES, list), i;
    for (i = 0; i < n; i++)
        if (g_items[list[i]].kind == kind)
            return list[i];
    return -1;
}

static void welcome_input(int pressed)
{
    int row;
    if (plat_take_back() || (pressed & K_BACK)) {
        M.welcome_sel = 2;
        pressed |= K_OK;
    }
    if (pressed & (K_UP | K_LEFT))
        M.welcome_sel = (M.welcome_sel + 2) % 3;
    if (pressed & (K_DOWN | K_RIGHT))
        M.welcome_sel = (M.welcome_sel + 1) % 3;
    if (!(pressed & (K_OK | K_PLAY)))
        return;
    M.welcome = 0;
    log_printf("menu: first run: %s", WELCOME_BTN[M.welcome_sel]);
    set_page(PAGE_FILES);
    if (M.welcome_sel == 2)
        return;
    row = start_row(M.welcome_sel ? IT_GF_JP : IT_GF_US);
    if (row >= 0) {   /* (its row shows the import's progress from here) */
        M.cursor = row;
        M.on_play = 0;
        change_item(&g_items[row], 1);
    }
}

static void draw_welcome(int w, int h)
{
    float u = gfx_ui_line(1.0f), pw = u * 24, ph, x, y, pad = u * 1.1f, ts = 0.7f, bh = u * 1.6f, ty;
    double t = now_s();
    const char *body = "The project runs on your own copy of the PC game. Import it to start: pick the folder "
                       "that has MEGAMAN.EXE (Mega Man Legends, the US release) or ROCKMAN.EXE (Rockman DASH, the "
                       "Japanese one). Its files are copied into this app's folder; the folder you pick isn't "
                       "changed. The other version can be imported any time on the GAME page.";
    int i;
    float save = ui_alpha;
    if (pw > w - u * 2)
        pw = w - u * 2;
    g_help_h = 1e9f;
    ph = pad + gfx_ui_font_line(UI_FONT_DISPLAY, ts) + u * 0.7f + wrap_text(0, 0, pw - pad * 2, body, 0, 0.9f, 0) +
         u * 0.8f + 3 * (bh + u * 0.35f) + u * 0.3f + u * 1.3f + pad * 0.6f;
    x = (w - pw) / 2;
    y = (h - ph) / 2;
    if (y < u * 0.5f)
        y = u * 0.5f;
    gfx_ui_rect(0, 0, (float)w, (float)h, ui_col(0xB0020610));   /* the menu behind, dimmed */
    ui_panel(x + u * 0.25f, y + u * 0.3f, pw, ph, u * 0.9f, 0x60000000, 0x60000000);
    ui_panel(x, y, pw, ph, u * 0.9f, COL_PANEL_TOP, COL_PANEL_BOT);
    ui_panel_edge(x, y, pw, ph, u * 0.9f, u * 0.08f, COL_EDGE);
    {
        float b = u * 0.7f, th = u * 0.12f;
        ui_line(x, y + b, x, y - th / 2, th, COL_YELLOW);
        ui_line(x - th / 2, y, x + b, y, th, COL_YELLOW);
        ui_line(x + pw, y + ph - b, x + pw, y + ph + th / 2, th, COL_YELLOW);
        ui_line(x + pw + th / 2, y + ph, x + pw - b, y + ph, th, COL_YELLOW);
    }
    ty = y + pad;
    ui_text_outlined(UI_FONT_DISPLAY, x + pad, ty, "IMPORT YOUR GAME", 0xFFFFFFFF, 0xFF9EE2FF, ts, u * 0.06f,
                     0xFF061226);
    ty += gfx_ui_font_line(UI_FONT_DISPLAY, ts) + u * 0.25f;
    gfx_ui_hgrad(x + pad, ty, pw - pad * 2, 1.5f, ui_col(0xA05CC8FF), ui_col(0x005CC8FF));
    ty += u * 0.45f;
    ty += wrap_text(x + pad, ty, pw - pad * 2, body, 0xFFD2DEEA, 0.9f, 1) + u * 0.8f;
    for (i = 0; i < 3; i++) {
        float bx = x + pad, bw = pw - pad * 2, lw = gfx_ui_font_width(UI_FONT_BODY, WELCOME_BTN[i], 0.95f);
        float lh = gfx_ui_font_line(UI_FONT_BODY, 0.95f);
        int sel = i == M.welcome_sel;
        if (sel) {
            float pulse = 0.5f + 0.5f * sinf((float)t * 5.0f);
            ui_slant(bx - u * 0.1f, ty - u * 0.1f, bw + u * 0.2f, bh + u * 0.2f, bh * 0.4f,
                     ui_with_alpha(COL_YELLOW, 0.3f + 0.3f * pulse), 0x00FFD23A);
            ui_slant(bx, ty, bw, bh, bh * 0.35f, COL_YELLOW, COL_YELLOW_LO);
            ui_arrow(bx - u * 0.4f + sinf((float)t * 6.0f) * u * 0.08f, ty + bh / 2, u * 0.4f, 1, COL_YELLOW);
        } else {
            ui_slant(bx, ty, bw, bh, bh * 0.35f, i < 2 ? COL_BLUE : 0xFF183052, i < 2 ? COL_BLUE_DARK : 0xFF122440);
        }
        ui_text(bx + (bw - lw) / 2, ty + (bh - lh) / 2, WELCOME_BTN[i], sel ? COL_NAVY : (i < 2 ? 0xFFFFFFFF : COL_DIM),
                0.95f);
        ty += bh + u * 0.35f;
    }
    {   /* the prompts */
        float fy = ty + u * 0.65f, fx = x + pad, r = u * 0.34f, sc = 0.7f, lh = gfx_ui_font_line(UI_FONT_BODY, sc);
        gfx_ui_rect(x + pad, ty + u * 0.05f, pw - pad * 2, 1, ui_col(0x14FFFFFF));
        if (M.pad_ui) {
            fx += dpad_glyph(fx, fy, r * 2) + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Choose", COL_DIM, sc) + u * 0.7f;
            fx += pad_button(fx, fy, r, "A", 0xFF3FA34D) + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Select", COL_DIM, sc) + u * 0.7f;
            fx += pad_button(fx, fy, r, "B", 0xFFD8443C) + u * 0.25f;
            ui_text(fx, fy - lh / 2, "Later", COL_DIM, sc);
        } else {
            fx += ui_key_cap(fx, fy, r * 1.9f, "Arrows") + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Choose", COL_DIM, sc) + u * 0.7f;
            fx += ui_key_cap(fx, fy, r * 1.9f, "Enter") + u * 0.25f;
            fx += ui_text(fx, fy - lh / 2, "Select", COL_DIM, sc) + u * 0.7f;
            fx += ui_key_cap(fx, fy, r * 1.9f, "Esc") + u * 0.25f;
            ui_text(fx, fy - lh / 2, "Later", COL_DIM, sc);
        }
    }
    ui_alpha = save;
}

int menu_pad_buttons(void)
{
    return M.ready ? M.pad_now : 0;
}

void menu_frame(void)
{
    WORD buttons;
    int keys, pressed, dir = 0;
    double t;
    if (!M.ready)
        return;
    keys = read_keys(&buttons);
    M.pad_now = buttons;
    pressed = keys & ~M.prev_keys;
    M.prev_keys = keys;
    if (M.cap) {   /* a key row waits for its new key: nothing else counts */
        key_capture(buttons);
        return;
    }
    if (M.block && !keys && !buttons)
        M.block = 0;
    if (M.start) {
        if (gf_busy()) {   /* the import goes on */
            char line[200], d[MAX_PATH];
            int st = gamefiles_import_poll(line, sizeof line, &M.gf_progress);
            /* The folder picker takes the pad while it is open: the A (or B) that closes it is
               still down when we are back, and would count as a press here (A on the row would
               open the picker again, B would close the app). Nothing counts until all is let go. */
            M.block = 1;
            if (line[0])
                strcpy_s(M.gf_line, sizeof M.gf_line, line);
            if (st == IMPORT_DONE) {
                M.gf_state = IMPORT_IDLE;
                strcpy_s(M.gf_line, sizeof M.gf_line, "Checking the game...");
                gamefiles_dir(M.game_dir, M.gf_jp, d, sizeof d);
                if (gamefiles_finish(d, line, sizeof line)) {
                    M.gf_ready[M.gf_jp] = 1;
                    {   /* (subtitles and voices can come with the imported folder) */
                        char sp[MAX_PATH];
                        _snprintf_s(sp, sizeof sp, _TRUNCATE, "%s\\Modernization\\subtitles_en.txt", M.game_dir);
                        M.subs_there = GetFileAttributesA(sp) != INVALID_FILE_ATTRIBUTES;
                        M.voices_built = voice_built(M.game_dir);
                    }
                    gamefiles_defaults(M.game_dir);   /* (the intro, CAPCOM.AVI) */
                    M.edition = M.gf_jp;   /* (the one just imported plays) */
                    WritePrivateProfileStringA("general", "edition", M.edition ? "japanese" : "us", M.ini);
                    strcpy_s(M.gf_line, sizeof M.gf_line, M.pad_ui ? "Ready. Press Start to play." : "Ready. Press Enter to play.");
                    log_printf("menu: game files imported (the %s version)", M.gf_jp ? "Japanese" : "US");
                } else {
                    M.gf_ready[M.gf_jp] = 0;
                    M.gf_state = IMPORT_FAILED;
                    strcpy_s(M.gf_line, sizeof M.gf_line, line);
                }
            } else if (st == IMPORT_FAILED || st == IMPORT_CANCELLED || st == IMPORT_IDLE) {
                M.gf_state = st;
                log_printf("menu: game files: %s", M.gf_line);
            } else if (st == IMPORT_COPYING)   /* (the row shows the progress from now on) */
                M.gf_state = st;
        }
        if (M.tk_state == TASK_RUNNING) {   /* an extra goes on (the menu stays in use) */
            char line[200];
            int st = gamefiles_task_poll(line, sizeof line, &M.tk_progress);
            if (line[0])
                strcpy_s(M.tk_line, sizeof M.tk_line, line);
            if (st == TASK_DONE && M.tk_kind == IT_GF_SUBS) {
                M.subs_there = M.subs_on = 1;
                WritePrivateProfileStringA("general", "subtitles", "1", M.ini);
                strcpy_s(M.tk_line, sizeof M.tk_line, "Subtitles downloaded and on. The Rockman DASH PSP English "
                         "project made them: github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher");
            } else if (st == TASK_DONE) {
                M.voices_built = M.voices_jp = 1;
                WritePrivateProfileStringA("general", "voices", "japanese", M.ini);
                strcpy_s(M.tk_line, sizeof M.tk_line, "Japanese voices built and on.");
            }
            if (st != TASK_RUNNING) {
                M.tk_state = st == TASK_DONE ? TASK_DONE : TASK_FAILED;
                log_printf("menu: %s", M.tk_line);
            }
        }
        if (update_can() && M.up_started) {   /* the update goes on in its own thread */
            char line[200], ver[48];
            int st = update_poll(line, sizeof line, ver, sizeof ver, &M.up_progress);
            strcpy_s(M.up_line, sizeof M.up_line, line);
            strcpy_s(M.up_ver, sizeof M.up_ver, ver);
            if (st != M.up_state) {
                log_printf("menu: update %d -> %d (%s)", M.up_state, st, line);
                if (st == UPD_AVAILABLE && M.up_state == UPD_CHECKING && !M.up_manual)
                    note("A new version is out: see the GAME page");
            }
            M.up_state = st;
#ifdef _WIN32
            if (st == UPD_INSTALLING) {   /* Setup runs: it replaces this program's files */
                M.quit = 1;
                return;
            }
#endif
        }
        if (M.block) {   /* (see above) */
            plat_take_back();
            return;
        }
        if (M.welcome) {
            welcome_input(pressed);
            return;
        }
        if (plat_take_back() || (pressed & K_BACK)) {   /* B or Android's Back: close the app */
            M.quit = 1;
            return;
        }
        if ((pressed & K_PLAY) || (M.on_play && (pressed & K_OK))) {
            if (M.tk_state == TASK_RUNNING) {   /* (the game reads what is being written) */
                set_page(PAGE_FILES);
                note(M.tk_kind == IT_GF_SUBS ? "Wait for the subtitles to finish downloading"
                                             : "Wait for the voices to finish building");
                return;
            }
            if (gamefiles_can_import() && !M.gf_ready[M.edition]) {   /* nothing to play yet */
                set_page(PAGE_FILES);
                note(M.gf_ready[!M.edition] ? (M.edition ? "Import the Japanese version first, or play the US one"
                                                         : "Import the US version first, or play the Japanese one")
                                            : "Import your game files first");
                return;
            }
            M.play = 1;
            return;
        }
    } else if (plat_take_back()) {   /* Android's Back: closes the menu, else twice quits the game */
        double now = now_s();
        if (M.open) {
            set_open(0);
        } else if (now - M.back_at < 2.5) {
            log_printf("menu: Back twice, the game closes");
            plat_quit();
        } else {
            M.back_at = now;
            toast_post(TOAST_ICON_WARN, TOAST_OFF, "PRESS BACK AGAIN", NULL, "to close the game");
        }
    }
    if (pressed & K_TOGGLE && !M.start) {
        set_open(!M.open);
        return;
    }
    if (!M.open)
        return;
    if ((pressed & K_BACK) && !M.start) {
        set_open(0);
        return;
    }
    if (pressed & K_PREV)
        set_page(M.page - 1);
    if (pressed & K_NEXT)
        set_page(M.page + 1);
    /* direction with hold-to-repeat */
    t = now_s();
    if (keys & K_UP) dir = K_UP;
    else if (keys & K_DOWN) dir = K_DOWN;
    else if (keys & K_LEFT) dir = K_LEFT;
    else if (keys & K_RIGHT) dir = K_RIGHT;
    if (dir != M.held_dir) {
        M.held_dir = dir;
        M.held_since = M.last_repeat = t;
        if (!dir)
            return;
    } else if (!dir || t - M.held_since < 0.35 || t - M.last_repeat < 0.07) {
        dir = 0;
    } else {
        M.last_repeat = t;
    }
    if (dir == K_UP)
        move_cursor(-1);
    else if (dir == K_DOWN)
        move_cursor(1);
    else if (M.start && M.on_play)
        ;   /* (left / right: nothing on the PLAY button) */
    else if (dir == K_LEFT)
        change_item(&g_items[M.cursor], -1);
    else if (dir == K_RIGHT)
        change_item(&g_items[M.cursor], 1);
    if ((pressed & K_OK) && !(M.start && M.on_play))
        change_item(&g_items[M.cursor], 1);
}

/* Pad chords with Back: Back + Start opens the menu, Back + RB quick saves.
 * Back and Start also mean something to the game (Start pauses), and people
 * never press two buttons on the same frame, so a lone Back or Start is held
 * back for a moment: if the partner follows, it was a chord and the game never
 * sees either; if not, the game gets it late (held) or as a short tap (let go
 * in time). RB isn't held back (nothing is bound to it by default). */
#define CHORD_S 0.12

static struct {
    WORD prev, pending, tap, swallow;
    int tap_polls;
    double since;
} F;
static volatile LONG g_save_req;

static void chord(unsigned short *buttons, WORD which)
{
    F.swallow = which;   /* keep these from the game until they're let go */
    F.pending = F.tap = 0;
    F.tap_polls = 0;
    *buttons &= (WORD)~which;
}

void menu_filter_buttons(unsigned short *buttons)
{
    const WORD HELD = BTN_BACK | BTN_START, MENU = BTN_BACK | BTN_START, SAVE = BTN_BACK | BTN_RB;
    WORD cur, newly;
    if (!M.ready)
        return;
    cur = (WORD)(*buttons & (HELD | BTN_RB));
    newly = (WORD)(cur & HELD & ~F.prev);
    F.prev = (WORD)(cur & HELD);
    if (F.swallow) {
        WORD sw = F.swallow;
        if (!(cur & sw))
            F.swallow = 0;
        *buttons &= (WORD)~sw;
        return;
    }
    if ((cur & MENU) == MENU) {   /* menu_frame sees it too and opens / closes the menu */
        chord(buttons, MENU);
        return;
    }
    if ((cur & SAVE) == SAVE) {
        chord(buttons, SAVE);
        if (!M.open)
            InterlockedExchange(&g_save_req, 1);
        return;
    }
    if (F.pending) {
        if (cur & F.pending) {
            if (now_s() - F.since < CHORD_S)
                *buttons &= (WORD)~F.pending;   /* still waiting for a partner */
            else
                F.pending = 0;                  /* a plain hold: pass it on */
        } else {
            F.tap = F.pending;                  /* let go in time: replay it as a tap */
            F.tap_polls = 2;
            F.pending = 0;
        }
    } else if (newly) {
        F.pending = newly;
        F.since = now_s();
        *buttons &= (WORD)~newly;
    }
    if (F.tap_polls > 0) {
        *buttons |= F.tap;
        F.tap_polls--;
    }
}

int menu_take_quicksave(void)
{
    return InterlockedExchange(&g_save_req, 0) != 0;
}

int menu_blocks_input(void)
{
    return M.open || M.block;
}

/* every value from mml.ini */
static void load_values(const char *ini, const char *game_dir)
{
    char s[MAX_PATH], full[MAX_PATH], dir[MAX_PATH];
    int i;
    QueryPerformanceFrequency(&M.qpf);
    strcpy_s(M.ini, sizeof M.ini, ini);
    strcpy_s(M.game_dir, sizeof M.game_dir, game_dir);
    M.fps60 = GetPrivateProfileIntA("video", "interp", 1, ini) != 0;
    M.vsync = GetPrivateProfileIntA("video", "vsync", 0, ini) != 0;
    M.res = GetPrivateProfileIntA("video", "render_scale", 0, ini);
    if (M.res < 0 || M.res > 4)
        M.res = 0;
    M.msaa = GetPrivateProfileIntA("video", "msaa", 1, ini);
    if (M.msaa != 4 && M.msaa != 8)
        M.msaa = 1;
    M.shader_game_res = GetPrivateProfileIntA("video", "shader_scale", 1, ini) != 0;
    M.nearest = GetPrivateProfileIntA("video", "nearest_upscale", 0, ini) != 0;
    M.aniso = GetPrivateProfileIntA("video", "anisotropy", 16, ini) > 1;
    GetPrivateProfileStringA("video", "fog", "original", s, sizeof s, ini);
    M.fog = _stricmp(s, "off") == 0 ? FOG_OFF : _stricmp(s, "light") == 0 ? FOG_LIGHT : FOG_ORIGINAL;
    GetPrivateProfileStringA("video", "lod", "full", s, sizeof s, ini);
    M.lod_full = _stricmp(s, "original") != 0;
    M.hdtex = GetPrivateProfileIntA("textures", "replace", 1, ini) != 0;
    M.cam_speed = nearest_index(SPEEDS, 6, GetPrivateProfileIntA("controls", "camera_speed_x", 100, ini));
    M.inv_x = GetPrivateProfileIntA("controls", "invert_x", 0, ini) != 0;
    M.inv_y = GetPrivateProfileIntA("controls", "invert_y", 0, ini) != 0;
    M.mouse = GetPrivateProfileIntA("controls", "mouse_keyboard", 1, ini)
                  ? nearest_index(MOUSE + 1, 5, GetPrivateProfileIntA("controls", "mouse_speed", 100, ini)) + 1 : 0;
    GetPrivateProfileStringA("controls", "reticle_color", "FFD840", s, sizeof s, ini);
    {
        unsigned rgb = (unsigned)strtoul(s, NULL, 16);
        M.reticle = 0;
        for (i = 0; i < (int)(sizeof RETICLES / sizeof RETICLES[0]); i++)
            if (RETICLES[i].rgb == rgb)
                M.reticle = i;
    }
    GetPrivateProfileStringA("controls", "aim", "character", s, sizeof s, ini);
    M.aim_camera = _stricmp(s, "camera") == 0;
    M.crosshair = GetPrivateProfileIntA("controls", "crosshair", 1, ini) != 0;
    /* presets */
    if (!M.presets && (M.presets = (char **)calloc(MAX_PRESETS, sizeof *M.presets)) != NULL) {   /* (once) */
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\Modernization\\shaders", game_dir);
        scan_presets(dir, 0);
        qsort(M.presets, (size_t)M.npresets, sizeof *M.presets, by_name);
    }
    GetPrivateProfileStringA("video", "shader", "", s, sizeof s, ini);
    full_path(s, full, sizeof full);
    M.shader = -1;
    M.shader_custom[0] = 0;
    for (i = 0; full[0] && i < M.npresets; i++)
        if (_stricmp(M.presets[i], full) == 0)
            M.shader = i;
    if (full[0] && M.shader < 0)
        strcpy_s(M.shader_custom, sizeof M.shader_custom, full);
    for (i = 0; i < NITEMS && i < MAX_ITEMS; i++)
        if (g_items[i].kind == IT_CHOICE)
            M.chv[i] = choice_read(&g_items[i]);
    {
        pad_t p;
        M.pad_ui = input_sdl_snapshot(&p);
    }
}

static void add_draw_once(void)
{
    static int added;
    if (!added++)
        gfx_ui_add_draw(draw, NULL);
}

void menu_install(const char *ini, const char *game_dir)
{
    load_values(ini, game_dir);
    set_page(0);
    add_draw_once();
    M.ready = 1;
    log_printf("menu: F1 or Back + Start (%d shader presets found)", M.npresets);
}

/* ---- the start menu ---- */

/* behind the start menu's panel: the project's colours, a slow sweep of light, the title */
static void draw_start_backdrop(float w, float h, float u, double t, int with_title)
{
    float band = u * 2.6f, ty, tw, sweep;
    const char *title = "MEGA MAN LEGENDS", *sub = "MODERNIZATION PROJECT";
    gfx_ui_vgrad(0, 0, w, h, ui_col(0xFF0E2452), ui_col(0xFF040A18));
    sweep = (float)fmod(t * 0.07, 1.0);
    ui_slant(-w * 0.3f + sweep * w * 1.6f, 0, w * 0.18f, h, h * 0.35f, 0x00FFFFFF, 0x0CFFFFFF);
    {   /* hazard stripes along the bottom */
        float sh = u * 0.35f, step = sh * 1.6f, sx;
        for (sx = -sh; sx < w; sx += step)
            ui_slant(sx, h - sh, step * 0.5f, sh, sh, 0x50FFD23A, 0x50FFA81C);
    }
    if (!with_title)
        return;
    tw = gfx_ui_font_width(UI_FONT_DISPLAY, title, 1.15f);
    ty = u * 0.7f;
    ui_slant((w - tw) / 2 - u * 1.2f, ty - u * 0.1f, tw + u * 2.4f, band, band * 0.4f, COL_BLUE, COL_BLUE_DARK);
    gfx_ui_rect((w - tw) / 2 - u * 1.2f, ty + band - u * 0.2f, tw + u * 1.6f, u * 0.1f, ui_col(COL_YELLOW));
    ui_text_outlined(UI_FONT_DISPLAY, (w - tw) / 2, ty + (band - gfx_ui_font_line(UI_FONT_DISPLAY, 1.15f)) / 2 - u * 0.1f,
                     title, 0xFFFFFFFF, 0xFF9EE2FF, 1.15f, u * 0.08f, 0xFF061226);
    tw = gfx_ui_font_width(UI_FONT_DISPLAY, sub, 0.5f);
    gfx_ui_font_text(UI_FONT_DISPLAY, (w - tw) / 2, ty + band + u * 0.15f, sub, ui_col(0xFF8FD3FF),
                     ui_col(0xFF8FD3FF), 0.5f);
}

int menu_start(const char *ini, const char *game_dir)
{
    g_items = g_start_items;
    NITEMS = (int)(sizeof g_start_items / sizeof g_start_items[0]);
    PAGES = START_PAGES;
    NPAGES = (int)(sizeof START_PAGES / sizeof START_PAGES[0]);
    M.start = 1;
    load_values(ini, game_dir);
    {
        char jp[MAX_PATH], music[MAX_PATH];
        gamefiles_dir(game_dir, 1, jp, sizeof jp);
        M.gf_ready[0] = gamefiles_version_ready(game_dir, 0);
        M.gf_ready[1] = gamefiles_can_import() && gamefiles_version_ready(game_dir, 1);
        M.welcome = gamefiles_can_import() && !M.gf_ready[0] && !M.gf_ready[1];
        M.welcome_sel = 0;
        M.edition = gamefiles_edition(game_dir);
        {
            char p[MAX_PATH];
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\Modernization\\subtitles_en.txt", game_dir);
            M.subs_there = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
            M.subs_on = GetPrivateProfileIntA("general", "subtitles", 1, ini) != 0;
            M.voices_built = voice_built(game_dir);
            GetPrivateProfileStringA("general", "voices", "english", p, sizeof p, ini);
            M.voices_jp = _stricmp(p, "japanese") == 0;
        }
        if (update_can() && GetPrivateProfileIntA("general", "check_updates", 1, ini)) {   /* (Check on start) */
            M.up_started = 1;
            update_check_begin();
        }
        set_page(gamefiles_can_import() && !M.gf_ready[M.edition] ? PAGE_FILES : 0);
        add_draw_once();
        M.ready = M.open = 1;
        M.shown = 1;
        log_printf("menu: the start menu (US version %s, Japanese version %s; the %s one plays)",
                   M.gf_ready[0] ? "there" : "not there", M.gf_ready[1] ? "there" : "not there",
                   M.edition ? "Japanese" : "US");
        gamefiles_dir(game_dir, M.edition, music, sizeof music);   /* (its BGM folder) */
        menu_music_start(music, ini);
    }
    while (!M.play) {
        if (!plat_pump() || M.quit) {
            menu_music_stop();
            log_printf("menu: closed from the start menu");
            return 0;
        }
        menu_frame();
        menu_music_frame(1.0f);
        gfx_clear(NULL, 1, 0xFF000000u, 1, 1.0f);
        gfx_present();
    }
    {   /* the music fades and the menu slides away (half a second) */
        double t0 = now_s(), f;
        M.open = 0;
        while ((f = (now_s() - t0) / 0.5) < 1.0) {
            plat_pump();
            menu_music_frame((float)(1.0 - f));
            gfx_clear(NULL, 1, 0xFF000000u, 1, 1.0f);
            gfx_present();
        }
        menu_music_stop();
    }
    /* the game's own menu from here (menu_install) */
    M.start = M.play = M.open = M.ready = 0;
    M.shown = 0;
    M.block = 1;   /* (the Start that started the game doesn't reach it) */
    g_items = g_game_items;
    NITEMS = (int)(sizeof g_game_items / sizeof g_game_items[0]);
    PAGES = GAME_PAGES;
    NPAGES = 3;
    log_printf("menu: play");
    return 1;
}
