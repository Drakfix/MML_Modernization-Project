/* Room ambience that loops (known issue of the PC port: the forest's wind and birds play once,
 * ~11 s after a room loads, then silence until the next room).
 *
 * Each room's sound comes from a per-stage table (0x53A180, read in FUN_004D8875): music IDs
 * have bit 31 set; the others are sound-bank IDs, and the only one any room uses is 0x108, the
 * forest ambience (stage 03 rooms 0-2 and a few rooms of stages 0E-12). A bank entry
 * (0x56220C + slot * 0x10 + bank * 0x808) is {part A, part B, buffer A, buffer B}: part A plays
 * once, part B plays looping (FUN_004FA12B / FUN_004FA2C9). The data gives 0x108 only a part A,
 * though the sample is an 11.3 s bed with no fade-out (made to loop). Before the room's sound
 * starts we move it to part B, so the game itself plays it looping; moving to a room with
 * another sound stops it with the game's stop function (FUN_004FA36B: nothing else would, the
 * music path only replaces the music). A stage change frees the bank and its buffers as before.
 * [general] ambience_loop=0 keeps the original behaviour. */
#include <windows.h>
#include <stdint.h>

#include "ambience.h"
#include "hook.h"
#include "log.h"

#define G_SITE_ROOM_SOUND   0x4D889Bu   /* FUN_004D8875: xor eax,eax; xor ebx,ebx; mov ecx,[edx+4d] */
#define G_ROOM_STATE        0x77E784u   /* +0x4D: stage (high byte), +0x4E: room (high byte) */
#define G_ROOM_SOUND_TABLE  0x53A180u   /* per stage: int32 sound per room */
#define G_SOUND_INFO        0x539914u   /* per bank ID - 0x80: {bank, slot, ?, channel} */
#define G_BANK_ID           0x562204u   /* per loaded bank (stride 0x808): which bank it holds */
#define G_BANK_ENTRIES      0x56220Cu   /* + slot * 0x10 + bank * 0x808 */
#define G_FN_SOUND_STOP     0x4FA36Bu   /* watcall EAX = entry */

static uint32_t g_looping_entry, g_looping_id, g_looping_part;

static uint32_t rd32(uint32_t a)
{
    return *(volatile uint32_t *)(uintptr_t)a;
}

static void wr32(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)a = v;
}

/* the bank entry of a sound-bank ID, or 0 when its bank isn't loaded */
static uint32_t bank_entry(uint32_t id)
{
    const uint8_t *info = (const uint8_t *)(uintptr_t)(G_SOUND_INFO + (id - 0x80) * 4);
    int b;
    if (id < 0x80 || id >= 0x80 + 0x200)
        return 0;
    for (b = 0; b < 5; b++)
        if (rd32(G_BANK_ID + 0x808u * (uint32_t)b) == info[0])
            return G_BANK_ENTRIES + 0x10u * info[1] + 0x808u * (uint32_t)b;
    return 0;
}

/* called as sound_stop_fn: cdecl (entry) -> watcall EAX = entry */
typedef void(__cdecl *sound_stop_fn)(uint32_t entry);
static __declspec(naked) void sound_stop_thunk(void)
{
    __asm {
        mov eax, [esp + 4]
        push esi
        mov esi, G_FN_SOUND_STOP
        call esi
        pop esi
        ret
    }
}

/* the room's sound is about to start (EDX = 0x77E784) */
static void on_room_sound(regs_t *r)
{
    uint32_t table, id = 0xFFFFFFFFu, e;
    int stage = (int8_t)(rd32(G_ROOM_STATE + 0x4D) >> 24), room = (int8_t)(rd32(G_ROOM_STATE + 0x4E) >> 24);
    (void)r;
    table = rd32(G_ROOM_SOUND_TABLE + 4u * (uint32_t)stage);
    if (table)
        id = rd32(table + 4u * (uint32_t)room);
    if (g_looping_entry && id != g_looping_id) {
        /* still that sample in that bank entry (a stage change frees and reloads banks) */
        if (bank_entry(g_looping_id) == g_looping_entry && rd32(g_looping_entry + 4) == g_looping_part &&
            rd32(g_looping_entry + 12))
            ((sound_stop_fn)(void *)sound_stop_thunk)(g_looping_entry);
        g_looping_entry = 0;
    }
    if (id & 0x80000000u)
        return;   /* music, or nothing */
    e = bank_entry(id);
    if (!e)
        return;
    if (rd32(e) && !rd32(e + 4) && !rd32(e + 8) && !rd32(e + 12)) {   /* part A only, no buffers yet */
        wr32(e + 4, rd32(e));
        wr32(e, 0);
        log_printf("ambience: sound %X loops (room %d of stage %02X)", id, room, stage);
    }
    if (rd32(e + 4) && !rd32(e)) {
        g_looping_entry = e;
        g_looping_id = id;
        g_looping_part = rd32(e + 4);
    }
}

int ambience_install(const char *ini)
{
    static const uint8_t site[7] = { 0x31, 0xC0, 0x31, 0xDB, 0x8B, 0x4A, 0x4D };
    if (!GetPrivateProfileIntA("general", "ambience_loop", 1, ini))
        return 0;
    if (!hook_mid(G_SITE_ROOM_SOUND, sizeof site, site, on_room_sound)) {
        log_printf("ambience: room sound hook failed, ambience plays once");
        return 0;
    }
    return 1;
}
