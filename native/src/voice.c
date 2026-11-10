/* Japanese voices ([general] voices=japanese): the game loads its voice banks from SPJ instead of
 * SPH. tools/voice/build_jp_voices.py builds SPJ from the player's own Japanese PC copy (Rockman
 * DASH) with the US banks' file names and clip numbering, so the game's own voice code plays them
 * unchanged.
 *
 * The bank file names are a table of pointers (0x511618) to strings "SPH\1806E.WBK" ... in the
 * game's data; FUN_0042C25D (EAX bank, EDX clip) opens one, reads the clip's two offsets and loads
 * the WAV between them (or streams it, above 512 KB). We change "SPH" to "SPJ" in those strings.
 * play_voice (FUN_004D8E42, EAX = bank << 16 | clip) stops the current voice, loads the new one
 * and plays it. The US dub often splits one Japanese line into two clips: SPJ then holds the
 * whole Japanese line in the first clip and an empty entry in the second, and an entry detour on
 * play_voice ignores empty entries, so the Japanese line plays on. Sound banks inside a few DAT
 * files hold voice samples too (the title screen's): DAJ has those files with the Japanese banks.
 *
 * The same entry detour tells the subtitles (subtitles.c) which clip starts, with either voices:
 * with the English ones ([general] subtitles=1 and no Japanese voices) it is all the detour does. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"
#include "hook.h"
#include "log.h"
#include "recomp.h"
#include "subtitles.h"
#include "voice.h"

#define G_FILE_NAMES    GA(0x5111F0u)   /* char *[]: "DAT\INIT_DAT.BIN" ... then BGM, then SPH */
#define G_VOICE_NAMES   GA(0x511618u)   /* char *[]: "SPH\1806E.WBK" ... "SPH\3701E.WBK" */
#define G_FN_PLAY_VOICE GA(0x4D8E42u)   /* watcall EAX = bank << 16 | clip */
#define MAX_BANKS 64
#define MAX_CLIPS 1024

static uint8_t g_empty[MAX_BANKS][MAX_CLIPS / 8];   /* SPJ entries with no audio */
static int g_nbanks;
static int g_active;   /* the Japanese voices */
static int g_subs;     /* the subtitles want to know the clips */
static int g_log;   /* [debug] voice_log=1: every voice the game plays */
static long g_skipped;
static char g_names[MAX_BANKS][16];
#ifndef MML_RUNNER
static volatile LONG g_handled;   /* (voice_detour) */
static uint8_t *g_trampoline;
#endif

/* the bank's offset table from SPJ: marks the empty entries; 0 if the file is missing or odd */
static int read_bank(const char *game_dir, const char *name, int bank)
{
    char path[MAX_PATH];
    FILE *f;
    uint32_t first, offs[MAX_CLIPS + 1];
    int n, i;
    snprintf(path, sizeof path, "%s\\SPJ\\%s", game_dir, name);
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return 0;
    n = 0;
    if (fread(&first, 4, 1, f) == 1 && first >= 8 && first % 4 == 0 && first / 4 <= MAX_CLIPS + 1) {
        n = (int)(first / 4);
        offs[0] = first;
        if (fread(offs + 1, 4, (size_t)n - 1, f) != (size_t)n - 1)
            n = 0;
    }
    fclose(f);
    for (i = 0; i + 1 < n; i++)
        if (offs[i + 1] == offs[i])
            g_empty[bank][i / 8] |= (uint8_t)(1u << (i % 8));
    return n > 1;
}

/* A few DAT files carry voice samples in their sound banks (the title screen, the shared bank):
   the builder writes DAJ\<name> with the Japanese banks for those. "DAT\..." -> "DAJ\..." for each
   one that exists; returns how many. */
static int redirect_dat(const char *game_dir)
{
    int i, n = 0;
    for (i = 0; i < 512; i++) {
        uint32_t p = GVAR(uint32_t, G_FILE_NAMES + 4u * (uint32_t)i);
        const char *s = p >= 0x509000u && p < 0x559000u ? (const char *)GUEST(p) : NULL;
        char path[MAX_PATH];
        if (!s || (memcmp(s, "DAT\\", 4) != 0 && memcmp(s, "BGM\\", 4) != 0))
            break;
        if (s[0] != 'D')
            continue;
        snprintf(path, sizeof path, "%s\\DAJ\\%s", game_dir, s + 4);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        GVAR(char, p + 2) = 'J';
        log_printf("voice: DAJ\\%s (Japanese sound bank)", s + 4);
        n++;
    }
    return n;
}

/* 1: the clip is an empty entry (leave the current voice playing) */
static int voice_skip(uint32_t id)
{
    uint32_t bank = id >> 16, clip = id & 0xFFFFu;
    int skip;
    if (bank >= (uint32_t)g_nbanks || clip >= MAX_CLIPS)
        return 0;
    skip = g_active && ((g_empty[bank][clip / 8] >> (clip % 8)) & 1);
    if (g_log)
        log_printf("voice: %s clip %u%s", g_names[bank], clip, skip ? " (empty entry: ignored)" : "");
    if (!skip && g_subs)
        subtitles_voice(g_names[bank], clip);
    g_skipped += skip;
    return skip;
}

#ifndef MML_RUNNER
/* entry detour on play_voice: nothing for an empty entry, else the original via the trampoline */
static __declspec(naked) void voice_detour(void)
{
    __asm {
        pushad
        push eax                     ; voice ID
        call voice_skip
        add esp, 4
        mov g_handled, eax
        popad
        cmp g_handled, 0
        jne skipped
        jmp dword ptr [g_trampoline]
    skipped:
        ret
    }
}
#endif

/* the same for the recompiled game (recomp.c): regs EAX ECX EDX EBX ESP EBP ESI EDI */
static int voice_detour_c(const uint32_t regs[8])
{
    return voice_skip(regs[0]);
}

int voice_install(const char *game_dir, int japanese, int subtitles, int log)
{
    static const uint8_t prologue[] = { 0x53, 0x51, 0x52, 0x56, 0x83, 0xEC, 0x04 };   /* push ebx/ecx/edx/esi; sub esp,4 */
    uint8_t jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    uint32_t names[MAX_BANKS];
    int32_t rel;
    void *routine;
    int i;
    if (!japanese && !subtitles)
        return 0;
    for (g_nbanks = 0; g_nbanks < MAX_BANKS; g_nbanks++) {
        uint32_t p = GVAR(uint32_t, G_VOICE_NAMES + 4u * (uint32_t)g_nbanks);
        const char *s = p >= 0x509000u && p < 0x559000u ? (const char *)GUEST(p) : NULL;
        if (!s || memcmp(s, "SPH\\", 4) != 0)
            break;
        names[g_nbanks] = p;
        snprintf(g_names[g_nbanks], sizeof g_names[0], "%s", s + 4);
    }
    g_log = log;
    if (g_nbanks < 2 || !mem_expect(G_FN_PLAY_VOICE, prologue, sizeof prologue)) {
        log_printf("voice: unexpected voice code or bank names, English voices");
        return 0;
    }
    for (i = 0; japanese && i < g_nbanks; i++)
        if (!read_bank(game_dir, (const char *)GUEST(names[i]) + 4, i)) {
            log_printf("voice: SPJ\\%s missing (build SPJ with tools/voice/build_jp_voices.py), English voices",
                       (const char *)GUEST(names[i]) + 4);
            memset(g_empty, 0, sizeof g_empty);
            japanese = 0;
            if (!subtitles)
                return 0;
        }
    g_subs = subtitles;
#ifndef MML_RUNNER
    g_trampoline = (uint8_t *)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline)
        return 0;
    memcpy(g_trampoline, prologue, sizeof prologue);
    g_trampoline[7] = 0xE9;
    rel = (int32_t)((G_FN_PLAY_VOICE + sizeof prologue) - ((uintptr_t)g_trampoline + 12));
    memcpy(g_trampoline + 8, &rel, 4);
    routine = (void *)voice_detour;
#else
    routine = (void *)voice_detour_c;   /* no machine code: the C version is all, known by this one */
#endif
    rel = (int32_t)(recomp_routine_address(routine) - (G_FN_PLAY_VOICE + 5));
    memcpy(jmp + 1, &rel, 4);
    if (!mem_write(G_FN_PLAY_VOICE, jmp, sizeof jmp))
        return 0;
    recomp_register_detour(routine, G_FN_PLAY_VOICE, voice_detour_c, 0);
    if (!japanese) {
        log_printf("voice: English voices (the clips they play go to the subtitles)");
        return 1;
    }
    for (i = 0; i < g_nbanks; i++)
        GVAR(char, names[i] + 2) = 'J';   /* "SPH\..." -> "SPJ\..." */
    redirect_dat(game_dir);
    g_active = 1;
    {
        int empty = 0, k;
        for (i = 0; i < g_nbanks; i++)
            for (k = 0; k < MAX_CLIPS; k++)
                empty += (g_empty[i][k / 8] >> (k % 8)) & 1;
        log_printf("voice: Japanese voices from SPJ (%d banks, %d empty entries)", g_nbanks, empty);
    }
    return 1;
}

long voice_skipped(void)
{
    return g_skipped;
}
