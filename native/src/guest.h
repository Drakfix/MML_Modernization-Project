#pragma once
/* The game's memory, as our code sees it. A game address (what the game's code computes and
 * stores: its globals, its heap, the pointers in its structures and registers) is a 32-bit
 * offset into the guest memory block that starts at G_MEMBASE. In the DLL inside the 32-bit game
 * process the block is the process itself (G_MEMBASE 0, so these cost nothing); the standalone
 * runtime reserves a block of its own (M3 item 4). recomp_rt.h's HPTR is the same thing.
 *
 *   GUEST(a)          host pointer to game address a (any integer type)
 *   GVAR(type, a)    the game variable of that type at a (volatile: the game changes it)
 *   GADDR(p)         game address of a host pointer into guest memory (what we hand the game) */
#include <stddef.h>
#include <stdint.h>

#include "addr.h"   /* GA(): fixed addresses of the US program, translated to the running one */

#ifdef MML_RUNNER
extern uintptr_t g_membase;   /* guest_mem.c */
#define G_MEMBASE g_membase
#endif
#ifndef G_MEMBASE
#define G_MEMBASE ((uintptr_t)0)
#endif

#define GUEST(a) ((void *)(G_MEMBASE + (uintptr_t)(uint32_t)(a)))
#define GVAR(type, a) (*(volatile type *)GUEST(a))
#define GADDR(p) ((uint32_t)((uintptr_t)(p) - G_MEMBASE))

/* memory the game can address, for what we hand it (strings, structures, display packets),
   zeroed; as a game address. In the DLL any host memory is; the standalone runtime takes it from
   the guest block. (guest_mem.c) */
uint32_t guest_alloc(size_t n);
void guest_free(uint32_t g);
/* a page region of n bytes at game address `at` (0: anywhere), committed if asked */
uint32_t guest_pages(uint32_t at, size_t n, int commit);
#ifdef MML_RUNNER
int guest_init(void);   /* reserve the block (the runner's first step) */
/* the game's VirtualAlloc / VirtualFree / VirtualQuery on its own memory */
uint32_t guest_virtual_alloc(uint32_t at, uint32_t n, uint32_t type);
int guest_virtual_free(uint32_t at, uint32_t n, uint32_t type);
uint32_t guest_virtual_query(uint32_t at, uint32_t out, uint32_t len);
void guest_dump(void);   /* log the regions (crash reports) */
#endif
