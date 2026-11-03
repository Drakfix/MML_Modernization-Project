/* costack.h on 32-bit x86 (MSVC). A switch saves the callee-saved registers and the FPU and SSE
 * control words on the running stack, stores its stack pointer and loads the other one. On
 * Windows the thread block's stack bounds and exception-handler chain belong to the stack too
 * (exception dispatch checks frames against the bounds), so they move with it, as with fibers. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "costack.h"
#include "log.h"

struct costack {
    void *sp;   /* while it isn't running */
    uint8_t *mem;
    size_t size;
    void *tib_base, *tib_limit, *tib_except;
};

static costack g_main;

costack *costack_main(void)
{
    return &g_main;
}

/* called as cs_swap_fn (save, sp): push the callee-saved state, *save = ESP, ESP = sp, pop it */
typedef void(__cdecl *cs_swap_fn)(void **save, void *sp);
static __declspec(naked) void cs_swap(void)
{
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        sub esp, 8
        fnstcw [esp]
        stmxcsr [esp + 4]
        mov eax, [esp + 28]   ; save
        mov ecx, [esp + 32]   ; sp
        mov [eax], esp
        mov esp, ecx
        fldcw [esp]
        ldmxcsr [esp + 4]
        add esp, 8
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

/* a new stack's first "return": EBX = fn, ESI = arg */
static __declspec(naked) void cs_start(void)
{
    __asm {
        push esi
        call ebx
        int 3   ; fn never returns
    }
}

costack *costack_new(size_t size, void (*fn)(void *arg), void *arg)
{
    costack *s = (costack *)calloc(1, sizeof *s);
    uint32_t *sp;
    if (!s)
        return NULL;
    /* committed in full: nothing grows it on demand */
    s->mem = (uint8_t *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s->mem) {
        log_printf("costack: no memory for a %u KB stack", (unsigned)(size >> 10));
        free(s);
        return NULL;
    }
    s->size = size;
    s->tib_base = s->mem + size;
    s->tib_limit = s->mem;
    s->tib_except = (void *)(intptr_t)-1;
    /* what cs_swap pops: control words, EDI, ESI = arg, EBX = fn, EBP, return address */
    sp = (uint32_t *)((uintptr_t)(s->mem + size - 64) & ~(uintptr_t)15);
    *--sp = (uint32_t)(uintptr_t)cs_start;
    *--sp = 0;                           /* EBP */
    *--sp = (uint32_t)(uintptr_t)fn;     /* EBX */
    *--sp = (uint32_t)(uintptr_t)arg;    /* ESI */
    *--sp = 0;                           /* EDI */
    *--sp = 0x1F80;                      /* MXCSR: Windows' default */
    *--sp = 0x027F;                      /* x87 control word: Windows' default */
    s->sp = sp;
    return s;
}

void costack_free(costack *s)
{
    if (!s || s == &g_main)
        return;
    VirtualFree(s->mem, 0, MEM_RELEASE);
    free(s);
}

void costack_switch(costack *from, costack *to)
{
    NT_TIB *t = (NT_TIB *)NtCurrentTeb();
    from->tib_base = t->StackBase;
    from->tib_limit = t->StackLimit;
    from->tib_except = t->ExceptionList;
    t->StackBase = to->tib_base;
    t->StackLimit = to->tib_limit;
    t->ExceptionList = (struct _EXCEPTION_REGISTRATION_RECORD *)to->tib_except;
    ((cs_swap_fn)(void *)cs_swap)(&from->sp, to->sp);
}

void costack_bounds(const costack *s, size_t *lo, size_t *hi)
{
    *lo = (size_t)(uintptr_t)s->mem;
    *hi = s->mem ? (size_t)(uintptr_t)(s->mem + s->size) : 0;
}
