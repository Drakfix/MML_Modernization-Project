#pragma once
#include <stddef.h>
#include <stdint.h>

/* Register snapshot handed to mid-function hooks. Layout matches what
 * pushfd+pushad leave on the stack, so handlers can read AND modify registers
 * (changes are restored into the CPU when the hook returns; esp is ignored). */
typedef struct regs {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eflags;
} regs_t;

typedef void (*hook_fn)(regs_t *r);

/* Byte-compare game memory against what we expect to find there. */
int mem_expect(uintptr_t addr, const void *bytes, size_t n);

/* Overwrite game memory (code or data), handling page protection. */
int mem_write(uintptr_t addr, const void *bytes, size_t n);

/* every mem_write so far (the recompiler's test harness skips code we patched) */
size_t mem_patch_count(void);
void mem_patch_get(size_t i, uintptr_t *addr, size_t *n);
char mem_patch_kind(size_t i);   /* 'H' a hook_mid jump, 'P' any other patch */

/* the handler hook_mid installed at this site (recompiled code calls it directly) */
hook_fn hook_find(uintptr_t site);

/* Run fn before the instruction(s) at `site`, then replay them.
 * `steal` must cover whole instructions (>= 5 bytes). The stolen bytes must be
 * position-independent, or be exactly one E8/E9 rel32 call/jmp, which is
 * re-targeted. `expect` (steal bytes) guards against patching the wrong build.
 * Returns 1 on success. */
int hook_mid(uintptr_t site, size_t steal, const uint8_t *expect, hook_fn fn);
