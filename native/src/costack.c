/* costack.h on Windows, x86 (MSVC) and x64 (clang). A switch saves the callee-saved registers and
 * the FPU and SSE control words on the running stack, stores its stack pointer and loads the
 * other one. On Windows the thread block's stack bounds and exception-handler chain belong to the
 * stack too (exception dispatch checks frames against the bounds), so they move with it, as with
 * fibers. */
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

#ifdef _M_X64
/* cs_swap(save, sp) and cs_start, x64 (Windows' convention: RCX, RDX; callee-saved RBX RBP RDI
   RSI R12-R15 XMM6-XMM15, the x87 control word and MXCSR's control bits). The frame a switch
   leaves, from its stack pointer up: XMM6-15 (160 bytes), x87 CW, MXCSR, R15 R14 R13 R12 RSI RDI
   RBX RBP, the return address. */
void cs_swap(void **save, void *sp);
void cs_start(void);
__asm__(".intel_syntax noprefix\n"
        ".text\n"
        ".globl cs_swap\n"
        "cs_swap:\n"
        "  push rbp\n  push rbx\n  push rdi\n  push rsi\n"
        "  push r12\n  push r13\n  push r14\n  push r15\n"
        "  sub rsp, 168\n"
        "  movaps [rsp], xmm6\n  movaps [rsp + 16], xmm7\n  movaps [rsp + 32], xmm8\n"
        "  movaps [rsp + 48], xmm9\n  movaps [rsp + 64], xmm10\n  movaps [rsp + 80], xmm11\n"
        "  movaps [rsp + 96], xmm12\n  movaps [rsp + 112], xmm13\n  movaps [rsp + 128], xmm14\n"
        "  movaps [rsp + 144], xmm15\n"
        "  fnstcw [rsp + 160]\n  stmxcsr [rsp + 164]\n"
        "  mov [rcx], rsp\n"
        "  mov rsp, rdx\n"
        "  fldcw [rsp + 160]\n  ldmxcsr [rsp + 164]\n"
        "  movaps xmm6, [rsp]\n  movaps xmm7, [rsp + 16]\n  movaps xmm8, [rsp + 32]\n"
        "  movaps xmm9, [rsp + 48]\n  movaps xmm10, [rsp + 64]\n  movaps xmm11, [rsp + 80]\n"
        "  movaps xmm12, [rsp + 96]\n  movaps xmm13, [rsp + 112]\n  movaps xmm14, [rsp + 128]\n"
        "  movaps xmm15, [rsp + 144]\n"
        "  add rsp, 168\n"
        "  pop r15\n  pop r14\n  pop r13\n  pop r12\n"
        "  pop rsi\n  pop rdi\n  pop rbx\n  pop rbp\n"
        "  ret\n"
        /* a new stack's first "return": RBX = fn, RSI = arg */
        ".globl cs_start\n"
        "cs_start:\n"
        "  mov rcx, rsi\n"
        "  and rsp, -16\n"
        "  sub rsp, 32\n"   /* the callee's home space */
        "  call rbx\n"
        "  int3\n"          /* fn never returns */
        ".att_syntax prefix\n");
#else
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

#endif

costack *costack_new(size_t size, void (*fn)(void *arg), void *arg)
{
    costack *s = (costack *)calloc(1, sizeof *s);
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
#ifdef _M_X64
    {   /* what cs_swap pops (see above); XMM6-15 start as 0 */
        uint64_t *sp = (uint64_t *)((uintptr_t)(s->mem + size - 64) & ~(uintptr_t)15);
        uint32_t *cw;
        *--sp = (uint64_t)(uintptr_t)cs_start;
        *--sp = 0;                       /* RBP */
        *--sp = (uint64_t)(uintptr_t)fn; /* RBX */
        *--sp = 0;                       /* RDI */
        *--sp = (uint64_t)(uintptr_t)arg;   /* RSI */
        sp -= 4;                         /* R12-R15 */
        memset(sp, 0, 32);
        sp -= 21;                        /* XMM6-15, the control words */
        memset(sp, 0, 168);
        cw = (uint32_t *)((uint8_t *)sp + 160);
        cw[0] = 0x027F;                  /* x87 control word: Windows' default */
        cw[1] = 0x1F80;                  /* MXCSR: Windows' default */
        s->sp = sp;
    }
#else
    {   /* what cs_swap pops: control words, EDI, ESI = arg, EBX = fn, EBP, return address */
        uint32_t *sp = (uint32_t *)((uintptr_t)(s->mem + size - 64) & ~(uintptr_t)15);
        *--sp = (uint32_t)(uintptr_t)cs_start;
        *--sp = 0;                           /* EBP */
        *--sp = (uint32_t)(uintptr_t)fn;     /* EBX */
        *--sp = (uint32_t)(uintptr_t)arg;    /* ESI */
        *--sp = 0;                           /* EDI */
        *--sp = 0x1F80;                      /* MXCSR: Windows' default */
        *--sp = 0x027F;                      /* x87 control word: Windows' default */
        s->sp = sp;
    }
#endif
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
#ifdef _M_X64
    cs_swap(&from->sp, to->sp);
#else
    ((cs_swap_fn)(void *)cs_swap)(&from->sp, to->sp);
#endif
}

void costack_bounds(const costack *s, size_t *lo, size_t *hi)
{
    *lo = (size_t)(uintptr_t)s->mem;
    *hi = s->mem ? (size_t)(uintptr_t)(s->mem + s->size) : 0;
}
