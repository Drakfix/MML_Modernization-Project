#pragma once
/* Which game program runs, and where the US addresses the sources use are in it.
 *
 * The mod is written against the US MEGAMAN.EXE. The Japanese rockman.exe (Rockman DASH) is the
 * same build moved around by small edits: the same sections at the same addresses, its code and
 * initialized data shifted by up to a few hundred bytes, the zeroed data (.bss: all the live game
 * state) where it is in the US program. tools/jp/map_exe.py lists the runs that moved by one
 * amount (addr_jp.c).
 *
 * Every fixed address of code or initialized data in the sources is written GA(0x...): it is the
 * address itself with the US program and the translated one with the Japanese. Addresses read
 * from game memory (pointers the game stored) are already the running program's own and must
 * not go through GA. .bss addresses (0x559000 and up) are the same in both and need no GA. */
#include <stdint.h>

typedef struct {
    uint32_t start, end;   /* US addresses */
    int32_t delta;         /* the Japanese address is the US one plus this */
} addr_seg;

enum { GAME_US, GAME_JP };

extern int addr_game;   /* GAME_US unless addr_detect found the Japanese program */

/* which program is loaded (its code bytes at a place they differ); logs it; returns addr_game,
   or -1 when neither matches (an unknown build, handled as the US one) */
int addr_detect(void);

/* US address -> running program's; 0 (and a log line) when the Japanese one has no counterpart */
uint32_t addr_xlat(uint32_t us);
/* the other way (a running program's address -> US), 0 when none */
uint32_t addr_us(uint32_t running);

#define GA(a) (addr_game == GAME_US ? (uint32_t)(a) : addr_xlat((uint32_t)(a)))

/* n expected (US) bytes at a running address, made to fit the running program: the distances of
   rel32 calls and jumps and the absolute addresses in them translated where that makes them
   match. Returns 1 when the bytes there then are those. */
int addr_expect(uint32_t at, const uint8_t *us_bytes, size_t n);
