/* Quick save ([debug] quick_save=1): F6 saves to the 10th slot from anywhere
 * in normal play, the same way talking to Data does, and the save loads from
 * the game's own Load Data screen.
 *
 * The save menu (state machine FUN_004A8760) ends in three calls:
 *   FUN_0042AE02  gather: area, Mega Man's position / facing / HP, items,
 *                 automap... into the save block 0x75164C (0x1F00 bytes);
 *                 reads the selected slot 0x55CD04 for a header field
 *   FUN_0042B162  seal: checksums over the block
 *   FUN_0042C3C8  write "DASH_xy.MCD" (x = slot / 5, y = slot % 5), a plain
 *                 fopen / fwrite of the block
 * We make the first two calls and write the file ourselves.
 *
 * The slot is never someone's real save by accident: after each quick save a
 * marker file next to it records a hash of what we wrote, and a slot whose
 * current content doesn't match (a save made through Data, or a slot used
 * before quick save existed) is left alone. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "controls.h"
#include "kbm.h"
#include "log.h"
#include "menu.h"
#include "quicksave.h"
#include "toast.h"

#define G_FN_SAVE_GATHER 0x42AE02u
#define G_FN_SAVE_SEAL   0x42B162u
#define G_SAVE_BLOCK     0x75164Cu
#define G_SAVE_SIZE      0x1F00u
#define G_SAVE_SLOT      0x55CD04u   /* byte: slot picked in the save menu, 0..9 */

/* the Load / Save Data lists: per slot a 13-character label (font tile codes)
 * copied from the area-name table 0x530085 by the save's area byte (+0x100).
 * Saves made away from the save points carry area 0, whose label is "No
 * data!", the same text an empty slot shows. */
#define G_SLOT_LABEL(i)  (0x52E6C4u + 0x1Fu * (i))
#define G_SLOT_HEADER(i) (0x55BDF0u + 0x180u * (i))   /* +0x109: 1 = the file couldn't be read */

#define QUICK_SLOT 9                    /* the 10th slot: DASH_14.MCD */
#define QUICK_KEY  VK_F6
#define STABLE_FRAMES 30             /* this long in normal play first (~1 s) */

static int g_on;
static char g_file[MAX_PATH], g_mark[MAX_PATH];
static int g_stable, g_key_was;

/* both take no arguments; Watcom code keeps every register but EAX, the
 * pushes are only belt and braces for the ones MSVC expects kept */
static void call_watcom0(uintptr_t fn)
{
    __asm {
        push ebx
        push esi
        push edi
        call dword ptr [fn]
        pop edi
        pop esi
        pop ebx
    }
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;   /* FNV-1a */
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

/* 1 if the slot is empty or still holds our last quick save */
static int slot_is_ours(void)
{
    static uint8_t buf[G_SAVE_SIZE];
    FILE *f;
    size_t n;
    unsigned long long want = 0;
    if (fopen_s(&f, g_file, "rb") != 0)
        return 1;                    /* no save there yet */
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (fopen_s(&f, g_mark, "r") != 0)
        return 0;                    /* a save we didn't make */
    if (fscanf_s(f, "%llx", &want) != 1)
        want = 0;
    fclose(f);
    return want != 0 && hash_bytes(buf, n) == want;
}

static void quick_save(void)
{
    volatile uint8_t *slot = (volatile uint8_t *)G_SAVE_SLOT;
    const uint8_t *block = (const uint8_t *)G_SAVE_BLOCK;
    uint8_t old_slot;
    FILE *f = NULL;

    if (!slot_is_ours()) {
        log_printf("quicksave: %s holds a save made elsewhere, not overwriting it", g_file);
        toast_post(TOAST_ICON_WARN, TOAST_BAD, "SLOT IN USE", NULL,
                   "Slot 10 holds a save made elsewhere, so it's left alone");
        return;
    }
    old_slot = *slot;
    *slot = QUICK_SLOT;
    call_watcom0(G_FN_SAVE_GATHER);
    call_watcom0(G_FN_SAVE_SEAL);
    *slot = old_slot;
    if (fopen_s(&f, g_file, "wb") != 0 || fwrite(block, 1, G_SAVE_SIZE, f) != G_SAVE_SIZE) {
        if (f)
            fclose(f);
        log_printf("quicksave: could not write %s", g_file);
        toast_post(TOAST_ICON_WARN, TOAST_BAD, "SAVE FAILED", NULL, "Couldn't write the save file (see mml.log)");
        return;
    }
    fclose(f);
    if (fopen_s(&f, g_mark, "w") == 0) {
        fprintf(f, "%016llx\n; written by the quick save (F6); delete this file to protect the slot\n",
                (unsigned long long)hash_bytes(block, G_SAVE_SIZE));
        fclose(f);
    }
    log_printf("quicksave: saved to %s", g_file);
    toast_post(TOAST_ICON_SAVE, TOAST_GOOD, "QUICK SAVED", NULL, "Slot 10 on the load screen");
}

void quicksave_install(const char *game_dir)
{
    _snprintf_s(g_file, sizeof g_file, _TRUNCATE, "%s\\DASH_%d%d.MCD", game_dir, QUICK_SLOT / 5, QUICK_SLOT % 5);
    _snprintf_s(g_mark, sizeof g_mark, _TRUNCATE, "%s.quicksave", g_file);
    g_on = 1;
    log_printf("quicksave: F6 or Back + RB saves to %s (the 10th slot)", g_file);
}

/* "Quick save" instead of "No data!" on a slot that does hold a save */
static void label_slot(void)
{
    static const uint8_t no_data[13] = { 0x22, 0x3D, 0x0A, 0x32, 0x2F, 0x42, 0x2F, 0x0D, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A };
    static const uint8_t quick[13] = { 0x25, 0x43, 0x37, 0x31, 0x39, 0x0A, 0x41, 0x2F, 0x44, 0x33, 0x0A, 0x0A, 0x0A };
    uint8_t *label = (uint8_t *)G_SLOT_LABEL(QUICK_SLOT);
    const uint8_t *hdr = (const uint8_t *)G_SLOT_HEADER(QUICK_SLOT);
    if (hdr[0x109] == 0 && hdr[0x100] == 0 && memcmp(label, no_data, sizeof no_data) == 0)
        memcpy(label, quick, sizeof quick);
}

void quicksave_frame(void)
{
    int key;
    if (!g_on)
        return;
    label_slot();
    g_stable = controls_in_gameplay() ? g_stable + 1 : 0;
    key = kbm_key(QUICK_KEY) != 0;
    if ((key && !g_key_was) | menu_take_quicksave()) {   /* F6, or Back + RB on the pad */
        if (g_stable >= STABLE_FRAMES)
            quick_save();
        else
            toast_post(TOAST_ICON_WARN, TOAST_BAD, "CAN'T SAVE NOW", NULL,
                       "Only while you're in control of Mega Man");
    }
    g_key_was = key;
}
