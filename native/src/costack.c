/* costack.h on Windows, x86 (MSVC) and x64 (clang), and on arm64 (Android). A switch saves the callee-saved registers and
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

#ifdef _WIN32
static uint8_t *stack_alloc(size_t size)
{
    return (uint8_t *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void stack_release(uint8_t *mem, size_t size)
{
    (void)size;
    VirtualFree(mem, 0, MEM_RELEASE);
}
#else
#include <pthread.h>
#include <sys/mman.h>

/* Stacks carved from the region of costack_thread's thread, downward from the top of its pool,
   each with an inaccessible gap below it (an overflow faults instead of running into the next).
   A freed stack is kept for the next one of the same size (the game's thread slots come and go,
   always 4 MB). Without costack_thread, each stack is a mapping of its own. */
#define GUARD (64u << 10)
#define SPARES 8

static uint8_t *g_pool_lo, *g_pool_hi, *g_pool_next;
static struct { uint8_t *mem; size_t size; } g_spare[SPARES];
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t *stack_alloc(size_t size)
{
    uint8_t *m = NULL;
    int i;
    pthread_mutex_lock(&g_pool_lock);
    if (g_pool_next) {
        for (i = 0; i < SPARES && !m; i++)
            if (g_spare[i].mem && g_spare[i].size == size) {
                m = g_spare[i].mem;
                g_spare[i].mem = NULL;
            }
        if (!m && (size_t)(g_pool_next - g_pool_lo) >= size + GUARD) {
            g_pool_next -= size;
            m = g_pool_next;
            g_pool_next -= GUARD;
            mprotect(g_pool_next, GUARD, PROT_NONE);
        }
    } else {
        m = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == (uint8_t *)MAP_FAILED)
            m = NULL;
    }
    pthread_mutex_unlock(&g_pool_lock);
    return m;
}

static void stack_release(uint8_t *mem, size_t size)
{
    int i;
    pthread_mutex_lock(&g_pool_lock);
    if (mem >= g_pool_lo && mem < g_pool_hi) {
        for (i = 0; i < SPARES && g_spare[i].mem; i++)
            ;
        if (i < SPARES) {
            g_spare[i].mem = mem;
            g_spare[i].size = size;
        }   /* (else it stays carved, unused) */
    } else {
        munmap(mem, size);
    }
    pthread_mutex_unlock(&g_pool_lock);
}

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} thread_start;

static void *thread_main(void *p)
{
    thread_start *t = (thread_start *)p;
    t->fn(t->arg);
    return NULL;
}

int costack_thread(size_t pool, size_t host, void (*fn)(void *arg), void *arg)
{
    /* from the bottom: a gap, the pool, a gap, fn's own frames */
    size_t total = GUARD + pool + GUARD + host;
    uint8_t *r = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                 -1, 0);
    thread_start t;
    pthread_attr_t a;
    pthread_t th;
    int ok;
    if (r == (uint8_t *)MAP_FAILED) {
        log_printf("costack: no memory for a %u MB thread stack", (unsigned)(total >> 20));
        return 0;
    }
    mprotect(r, GUARD, PROT_NONE);
    g_pool_lo = r + GUARD;
    g_pool_hi = g_pool_next = g_pool_lo + pool;
    mprotect(g_pool_hi, GUARD, PROT_NONE);
    t.fn = fn;
    t.arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstack(&a, r, total);
    ok = pthread_create(&th, &a, thread_main, &t) == 0;
    pthread_attr_destroy(&a);
    if (!ok) {
        log_printf("costack: the thread did not start");
        return 0;
    }
    pthread_join(th, NULL);
    return 1;
}
#endif

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
#elif defined(__aarch64__)
/* cs_swap(save, sp) and cs_start, arm64 (AAPCS64: X0, X1; callee-saved X19-X28, X29 frame pointer,
   X30 link register, D8-D15, and FPCR, the rounding and flush-to-zero controls). The frame a
   switch leaves, from its stack pointer up: X19-X28, X29, X30, D8-D15, FPCR, padding (176 bytes). */
void cs_swap(void **save, void *sp);
void cs_start(void);
__asm__(".text\n"
        ".globl cs_swap\n"
        ".type cs_swap, %function\n"
        ".p2align 2\n"
        "cs_swap:\n"
        "  sub sp, sp, #176\n"
        "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n"
        "  stp d14, d15, [sp, #144]\n"
        "  mrs x9, fpcr\n  str x9, [sp, #160]\n"
        "  mov x9, sp\n  str x9, [x0]\n"
        "  mov sp, x1\n"
        "  ldr x9, [sp, #160]\n  msr fpcr, x9\n"
        "  ldp d14, d15, [sp, #144]\n  ldp d12, d13, [sp, #128]\n  ldp d10, d11, [sp, #112]\n"
        "  ldp d8, d9, [sp, #96]\n"
        "  ldp x29, x30, [sp, #80]\n  ldp x27, x28, [sp, #64]\n  ldp x25, x26, [sp, #48]\n"
        "  ldp x23, x24, [sp, #32]\n  ldp x21, x22, [sp, #16]\n  ldp x19, x20, [sp, #0]\n"
        "  add sp, sp, #176\n"
        "  ret\n"
        /* a new stack's first "return": X19 = fn, X20 = arg */
        ".globl cs_start\n"
        ".type cs_start, %function\n"
        ".p2align 2\n"
        "cs_start:\n"
        "  mov x0, x20\n"
        "  blr x19\n"
        "  brk #0\n");   /* fn never returns */
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
    s->mem = stack_alloc(size);
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
#elif defined(__aarch64__)
    {   /* what cs_swap pops (see above): X19 = fn, X20 = arg, X30 = cs_start; FPCR 0 (round to nearest) */
        uint64_t *f = (uint64_t *)(((uintptr_t)(s->mem + size - 64) & ~(uintptr_t)15) - 176);
        memset(f, 0, 176);
        f[0] = (uint64_t)(uintptr_t)fn;     /* X19 */
        f[1] = (uint64_t)(uintptr_t)arg;    /* X20 */
        f[11] = (uint64_t)(uintptr_t)cs_start;   /* X30 */
        s->sp = f;
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
    stack_release(s->mem, s->size);
    free(s);
}

void costack_switch(costack *from, costack *to)
{
#ifdef _WIN32
    NT_TIB *t = (NT_TIB *)NtCurrentTeb();
    from->tib_base = t->StackBase;
    from->tib_limit = t->StackLimit;
    from->tib_except = t->ExceptionList;
    t->StackBase = to->tib_base;
    t->StackLimit = to->tib_limit;
    t->ExceptionList = (struct _EXCEPTION_REGISTRATION_RECORD *)to->tib_except;
#endif
#if defined(_M_X64) || defined(__aarch64__)
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
