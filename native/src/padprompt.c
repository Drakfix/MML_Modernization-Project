/* Controller button prompts: the names our screens give each button (padprompt.h). */
#include <windows.h>
#include <string.h>

#include "input.h"
#include "padprompt.h"

enum { PS_AUTO = -1 };
static volatile int g_setting = PS_AUTO;

/* SDL_GamepadType (input.c type_name has the same order) */
enum { T_UNKNOWN, T_STANDARD, T_XBOX360, T_XBOXONE, T_PS3, T_PS4, T_PS5, T_SWITCH_PRO, T_JOYCON_L, T_JOYCON_R,
       T_JOYCON_PAIR, T_GAMECUBE };

static const char *const XBOX_NAMES[PB_COUNT] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3", "Back",
                                                  "Start" };

/* by family: the cap's text and the name in a sentence */
static const char *const CAPS[5][PB_COUNT] = {
    { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3", "Back", "Start" },
    /* (as printed on the controller: DualShock 3 and 4 in capitals, a DualSense has icons: ui_style.c) */
    { "", "", "", "", "L1", "R1", "L2", "R2", "L3", "R3", "SELECT", "START" },
    { "", "", "", "", "L1", "R1", "L2", "R2", "L3", "R3", "SHARE", "OPTIONS" },
    { "", "", "", "", "L1", "R1", "L2", "R2", "L3", "R3", "Create", "Options" },
    /* a Switch controller has B at the bottom, A on the right, Y on the left, X on top */
    { "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "LS", "RS", "-", "+" },
};
static const char *const WORDS[5][PB_COUNT] = {
    { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3", "Back", "Start" },
    { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Select", "Start" },
    { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Share", "Options" },
    { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Create", "Options" },
    { "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "LS", "RS", "Minus", "Plus" },
};

void pad_prompts_load(const char *ini)
{
    static const struct { const char *v; int f; } V[] = {
        { "auto", PS_AUTO }, { "xbox", PF_XBOX }, { "ps3", PF_PS3 }, { "ps4", PF_PS4 }, { "ps5", PF_PS5 },
        { "playstation", PF_PS4 }, { "switch", PF_SWITCH }, { "nintendo", PF_SWITCH },
    };
    char v[32];
    size_t i;
    if (!ini || !ini[0])
        return;
    GetPrivateProfileStringA("controls", "button_prompts", "auto", v, sizeof v, ini);
    for (i = 0; i < sizeof V / sizeof V[0]; i++)
        if (_stricmp(v, V[i].v) == 0) {
            g_setting = V[i].f;
            return;
        }
    g_setting = PS_AUTO;
}

int pad_family(void)
{
    int s = g_setting;
    if (s != PS_AUTO)
        return s;
    switch (input_pad_type()) {   /* (the controller connected now, or the last one) */
    case T_PS3: return PF_PS3;
    case T_PS4: return PF_PS4;
    case T_PS5: return PF_PS5;
    case T_SWITCH_PRO: case T_JOYCON_L: case T_JOYCON_R: case T_JOYCON_PAIR: return PF_SWITCH;
    default: return PF_XBOX;   /* Xbox, Steam Deck, GameCube, a generic pad, or none */
    }
}

int pad_is_playstation(void)
{
    int f = pad_family();
    return f == PF_PS3 || f == PF_PS4 || f == PF_PS5;
}

const char *pad_cap(int b)
{
    return b >= 0 && b < PB_COUNT ? CAPS[pad_family()][b] : "";
}

const char *pad_word(int b)
{
    return b >= 0 && b < PB_COUNT ? WORDS[pad_family()][b] : "";
}

int pad_from_name(const char *name)
{
    int b;
    if (!name)
        return -1;
    for (b = 0; b < PB_COUNT; b++)
        if (strcmp(name, XBOX_NAMES[b]) == 0)
            return b;
    return -1;
}

void pad_expand(const char *src, char *out, unsigned n)
{
    unsigned o = 0;
    if (!n)
        return;
    for (; *src && o + 1 < n; src++) {
        unsigned char c = (unsigned char)*src;
        if (c >= PT_FIRST && c < PT_FIRST + PB_COUNT) {
            const char *w = pad_word(c - PT_FIRST);
            while (*w && o + 1 < n)
                out[o++] = *w++;
        } else {
            out[o++] = *src;
        }
    }
    out[o] = 0;
}
