/* Every difficulty from the start ([general] all_difficulties=1).
 *
 * How the game unlocks them: the title menu (game_state 4, FUN_00437B78; NEW GAME, CONTINUE,
 * OPTION) gets a fourth row, the difficulty, only while the clear count G_CLEARS is not 0, and
 * Left / Right reach Easy only while G_EASY_OK is not 0 (otherwise they cycle Normal and Hard).
 * Both are per save: the save's +0x130 and +0x132, copied in when a save starts (FUN_0042B672)
 * and out when the game saves (FUN_0042AE02). The ending (FUN_00499BA9) adds one to the clear
 * count and sets G_EASY_OK when the game was cleared on Hard or in under three hours (play time
 * 0x77E790 < 324000 frames at 30 per second). The clear save made after the credits takes the
 * player back to the title with those values in memory, which is where the row appears. Nothing
 * else reads either value: the boot (FUN_004A0A5C) zeroes them, the title menu, the save and the
 * ending use them, and that is all. NEW GAME keeps them (its reset, FUN_0042BDB2, doesn't touch
 * them) and the difficulty chosen, G_DIFF, which is the only value the game itself plays by
 * (enemy damage tables, Hard's extra attacks) and which the save stores as well (+0x19F).
 *
 * Here: on every title menu frame, before the menu reads them, the two values are lent (the clear
 * count at least 1, Easy unlocked); the game's own ones are kept. When anything starts from the
 * title (NEW GAME, a loaded save, the attract demo: the start of coroutine_49F4F7) they are given
 * back before the game runs, so a save never stores them: a new game picked this way is the same
 * as one started from a clear save, with the player's real clear count. The difficulty picked stays,
 * as it does in the game. Turned off at the title, the values go back at once and a difficulty the
 * real ones wouldn't offer is put back to the one before.
 *
 * The same in the Japanese program (the functions' bytes match; .bss is shared) and in the
 * recompiled game (both hook sites are in tools/recomp/patch_sites.txt). */
#include <windows.h>
#include <stdint.h>

#include "addr.h"
#include "difficulty.h"
#include "guest.h"
#include "hook.h"
#include "log.h"

#define G_CLEARS  0x77E79Cu   /* s16: times cleared (-1 = no more counting); the difficulty row needs != 0 */
#define G_EASY_OK 0x77E79Eu   /* s16: Easy unlocked */
#define G_DIFF    0x77E7DFu   /* u8: 0 Easy, 1 Normal, 2 Hard */
#define G_MENU_ROW 0x6DD9C8u  /* u8: the title menu's cursor (game_state + 0xC; 3 = the difficulty) */

#define G_FN_TITLE_MENU GA(0x437B78u)   /* game_state 4's handler: the title menu, every frame */
#define G_FN_GAME_START GA(0x49F534u)   /* coroutine_49F4F7's first state: new game, loaded save, demo */

static volatile int g_on;
static int g_lent;               /* the values in memory are ours; the game's are below */
static int16_t g_clears, g_easy;
static uint8_t g_diff;           /* the difficulty when they were lent */

static void give_back(int at_title)
{
    GVAR(int16_t, G_CLEARS) = g_clears;
    GVAR(int16_t, G_EASY_OK) = g_easy;
    if (at_title) {   /* (turned off on the title menu: keep only what the game's values offer) */
        uint8_t d = GVAR(uint8_t, G_DIFF);
        if (d != g_diff && (g_clears == 0 || (d == 0 && g_easy == 0)))
            GVAR(uint8_t, G_DIFF) = g_diff;
        if (g_clears == 0 && GVAR(uint8_t, G_MENU_ROW) == 3)   /* (the row is gone: confirm there would
                                                                 still cycle the difficulty) */
            GVAR(uint8_t, G_MENU_ROW) = 2;
    }
    g_lent = 0;
}

static void on_title_menu(regs_t *r)
{
    (void)r;
    if (!g_on) {
        if (g_lent) {
            give_back(1);
            log_printf("difficulty: off, the game's own unlocks are back");
        }
        return;
    }
    if (!g_lent) {
        g_clears = GVAR(int16_t, G_CLEARS);
        g_easy = GVAR(int16_t, G_EASY_OK);
        g_diff = GVAR(uint8_t, G_DIFF);
        g_lent = 1;
        log_printf("difficulty: all offered on the title menu (the game's: cleared %d, easy %d, difficulty %u)",
                   g_clears, g_easy, (unsigned)g_diff);
    }
    if (GVAR(int16_t, G_CLEARS) == 0)
        GVAR(int16_t, G_CLEARS) = 1;
    GVAR(int16_t, G_EASY_OK) = 1;
}

static void on_game_start(regs_t *r)
{
    (void)r;
    if (g_lent) {
        give_back(0);   /* (a loaded save's values replace them next, FUN_0042B672) */
        log_printf("difficulty: the game's unlocks back as a game starts (difficulty %s)",
                   GVAR(uint8_t, G_DIFF) == 0 ? "Easy" : GVAR(uint8_t, G_DIFF) == 2 ? "Hard" : "Normal");
    }
}

void difficulty_load(const char *ini)
{
    g_on = GetPrivateProfileIntA("general", "all_difficulties", 0, ini) != 0;
}

int difficulty_install(const char *ini)
{
    /* push ebx; push ecx; push edx; push esi; mov ecx,eax */
    static const uint8_t title_menu[6] = { 0x53, 0x51, 0x52, 0x56, 0x89, 0xC1 };
    /* push ecx; push edx; mov edx,eax; mov al,[eax+0x82] */
    static const uint8_t game_start[10] = { 0x51, 0x52, 0x89, 0xC2, 0x8A, 0x80, 0x82, 0x00, 0x00, 0x00 };
    difficulty_load(ini);
    /* both hooks always: the setting changes while the game runs */
    if (!hook_mid(G_FN_GAME_START, sizeof game_start, game_start, on_game_start)) {
        log_printf("difficulty: game start hook failed, difficulties as in the game");
        return 0;
    }
    if (!hook_mid(G_FN_TITLE_MENU, sizeof title_menu, title_menu, on_title_menu)) {
        log_printf("difficulty: title menu hook failed, difficulties as in the game");
        return 0;
    }
    return 1;
}
