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

#ifndef G_MEMBASE
#define G_MEMBASE ((uintptr_t)0)
#endif

#define GUEST(a) ((void *)(G_MEMBASE + (uintptr_t)(uint32_t)(a)))
#define GVAR(type, a) (*(volatile type *)GUEST(a))
#define GADDR(p) ((uint32_t)((uintptr_t)(p) - G_MEMBASE))

/* memory the game can address, for what we hand it (strings, structures, display packets),
   zeroed; as a game address. In the DLL any host memory is; the standalone runtime takes it from
   the guest block. (shim.c) */
uint32_t guest_alloc(size_t n);
void guest_free(uint32_t g);
