#pragma once
/* Host stacks to switch between: the recompiled game's cooperative threads each run their C
 * code (the generated functions call each other on the C stack) on a stack of their own. A
 * small stack switch replaces Windows' fibers so the same code runs where there are none
 * (Android): x86 here, arm64 with the portable runtime. */
#include <stddef.h>

typedef struct costack costack;

/* the context running now (the thread's own stack), to switch away from and back to */
costack *costack_main(void);
/* a new stack of `size` bytes that starts by running fn(arg); fn must never return (it switches
   away for good instead) */
costack *costack_new(size_t size, void (*fn)(void *arg), void *arg);
void costack_free(costack *s);   /* not the one running */
/* save the running context into `from` and continue `to` (where it switched away, or its fn) */
void costack_switch(costack *from, costack *to);
/* the stack's bounds (0, 0 for the main one) */
void costack_bounds(const costack *s, size_t *lo, size_t *hi);

#ifndef _WIN32
/* fn(arg) on a new thread whose stack is one region of `pool + host` bytes: fn's own frames use
   the top `host` bytes and every costack_new afterwards is carved from the rest, so all of them
   lie inside the thread's stack as the system knows it. Android's Java runtime refuses a call
   into Java (the window, the surface) made with the stack pointer below the thread's stack, as
   on a stack from anywhere else. Waits for fn; 0 if the thread could not start. */
int costack_thread(size_t pool, size_t host, void (*fn)(void *arg), void *arg);
#endif
