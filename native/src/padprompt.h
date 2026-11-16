#pragma once
/* Controller button prompts (issue #15): what our own screens call each button, for the
 * controller in use or the one picked in [controls] button_prompts (auto, xbox, ps3, ps4, ps5,
 * switch). Buttons are positional, as SDL3 reads them: PB_A is the bottom face button, which a
 * Switch controller labels B. The drawing (round face buttons, PlayStation symbols) is in
 * ui_style.c. */

enum { PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_L3, PB_R3, PB_BACK, PB_START, PB_COUNT };
enum { PF_XBOX, PF_PS3, PF_PS4, PF_PS5, PF_SWITCH };

/* A button in a text, replaced by its name when it is shown (pad_expand). Write them as separate
   string pieces ("hold " PT_BACK " to"): a hex escape would take letters that follow it. */
#define PT_A     "\x10"
#define PT_B     "\x11"
#define PT_X     "\x12"
#define PT_Y     "\x13"
#define PT_LB    "\x14"
#define PT_RB    "\x15"
#define PT_LT    "\x16"
#define PT_RT    "\x17"
#define PT_L3    "\x18"
#define PT_R3    "\x19"
#define PT_BACK  "\x1A"
#define PT_START "\x1B"
#define PT_FIRST 0x10

void pad_prompts_load(const char *ini);   /* reads [controls] button_prompts (again after a change) */
int pad_family(void);                     /* PF_*: the setting, or with auto the controller's type */
int pad_is_playstation(void);
const char *pad_cap(int b);    /* a short name for a button cap ("LB", "L1", "ZL", "-"); "" for a PS face button */
const char *pad_word(int b);   /* its name in a sentence ("Cross", "Options", "Minus") */
int pad_from_name(const char *xbox_name);   /* "A" .. "Start" (our Xbox names, also in mml.ini keys): PB_*, or -1 */
/* a text with its PT_* marks as the current names */
void pad_expand(const char *src, char *out, unsigned n);
