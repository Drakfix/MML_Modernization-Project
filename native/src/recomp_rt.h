#pragma once
/* Runtime for statically recompiled game code (tools/recomp/recomp.py).
 *
 * The recompiler turns each x86 function of MEGAMAN.EXE into a C function
 * taking an RCtx: the guest CPU state. Registers live in a byte array laid out
 * like Ghidra's x86 register space (EAX at 0, ESP at 0x10, the x87 stack ST0..ST7
 * at 0x1100.., flags as one byte each at 0x200..), so partial registers (AL, AX)
 * overlap exactly as on the real CPU. Guest memory is addressed with 32-bit
 * guest addresses through the LD/ST helpers; on Windows the guest and host
 * address spaces are the same (G_MEMBASE 0), on other platforms the game's
 * image and heap will live in one reserved block.
 *
 * x87 values (Ghidra's 10-byte registers) are kept as doubles. The precision
 * and rounding control of the guest's FPU control word are honoured, since
 * the game relies on both (single precision set up by Direct3D, truncating
 * float-to-int conversions). */
#include <math.h>
#include <stdint.h>
#include <string.h>

#define RCTX_REGS 0x2240

typedef struct RCtx {
    uint8_t R[RCTX_REGS];
    uint32_t ret;        /* target of the last RETURN */
    int log;             /* differential mode: record memory writes (see recomp.c) */
    int fdelta;          /* net x87 pushes since entry (Ghidra's model has no TOP) */
    uint16_t fsw0;       /* x87 status word at entry: TOP and the sticky exception flags */
    int fdepth0;         /* x87 stack depth at entry (absolute depth = fdepth0 + fdelta) */
} RCtx;

#ifndef G_MEMBASE
#define G_MEMBASE ((uintptr_t)0)
#endif
#define HPTR(a) ((uint8_t *)(G_MEMBASE + (uintptr_t)(uint32_t)(a)))

/* register file */
#define RDEF(bits)                                                                         \
    static __inline uint##bits##_t R##bits(const RCtx *c, unsigned off)                    \
    {                                                                                      \
        uint##bits##_t v;                                                                  \
        memcpy(&v, c->R + off, sizeof v);                                                  \
        return v;                                                                          \
    }                                                                                      \
    static __inline void R##bits##_SET(RCtx *c, unsigned off, uint##bits##_t v)            \
    {                                                                                      \
        memcpy(c->R + off, &v, sizeof v);                                                  \
    }
RDEF(8)
RDEF(16)
RDEF(32)
RDEF(64)
static __inline double RF(const RCtx *c, unsigned off)
{
    double v;
    memcpy(&v, c->R + off, sizeof v);
    return v;
}
static __inline void RF_SET(RCtx *c, unsigned off, double v)
{
    memcpy(c->R + off, &v, sizeof v);
}

#define REG_EAX 0x0
#define REG_ECX 0x4
#define REG_EDX 0x8
#define REG_EBX 0xC
#define REG_ESP 0x10
#define REG_EBP 0x14
#define REG_ESI 0x18
#define REG_EDI 0x1C
#define REG_DF  0x20A
#define REG_FPUCW 0x10A0
#define REG_FS_BASE 0x110   /* Ghidra's FS_OFFSET: FS:[x] reads FS_BASE + x (the thread block) */

/* guest memory. Diff mode (recomp.c) logs every write so it can undo it; that check on every
   store costs the normal build a lot (the possible call blocks optimisation around each store),
   so it is only compiled in with RECOMP_DIFF (native\build.bat with RECOMP_DIFF=1 set). */
void rt_store_log(RCtx *c, uint32_t a, unsigned n);   /* recomp.c: called before a logged write */
#ifdef RECOMP_DIFF
#define RT_STORE_LOG(c, a, n)                                                              \
    do {                                                                                   \
        if ((c)->log)                                                                      \
            rt_store_log(c, a, n);                                                         \
    } while (0)
#else
#define RT_STORE_LOG(c, a, n) ((void)(c))
#endif
#define LDDEF(bits)                                                                        \
    static __inline uint##bits##_t LD##bits(uint32_t a)                                    \
    {                                                                                      \
        uint##bits##_t v;                                                                  \
        memcpy(&v, HPTR(a), sizeof v);                                                     \
        return v;                                                                          \
    }                                                                                      \
    static __inline void ST##bits(RCtx *c, uint32_t a, uint##bits##_t v)                   \
    {                                                                                      \
        RT_STORE_LOG(c, a, sizeof v);                                                      \
        memcpy(HPTR(a), &v, sizeof v);                                                     \
    }
LDDEF(8)
LDDEF(16)
LDDEF(32)
LDDEF(64)

/* odd-sized memory operands (LES/LFS far pointers are 6 bytes) */
static __inline uint64_t LDN(uint32_t a, unsigned n)
{
    uint64_t v = 0;
    memcpy(&v, HPTR(a), n);
    return v;
}
static __inline void STN(RCtx *c, uint32_t a, uint64_t v, unsigned n)
{
    RT_STORE_LOG(c, a, n);
    memcpy(HPTR(a), &v, n);
}

/* integer helpers with P-code semantics (shift counts >= width give 0 / sign) */
#define SHDEF(bits, sbits)                                                                 \
    static __inline uint##bits##_t SHL##bits(uint##bits##_t a, uint64_t n)                 \
    {                                                                                      \
        return n >= bits ? 0 : (uint##bits##_t)(a << n);                                   \
    }                                                                                      \
    static __inline uint##bits##_t SHR##bits(uint##bits##_t a, uint64_t n)                 \
    {                                                                                      \
        return n >= bits ? 0 : (uint##bits##_t)(a >> n);                                   \
    }                                                                                      \
    static __inline uint##bits##_t SAR##bits(uint##bits##_t a, uint64_t n)                 \
    {                                                                                      \
        if (n >= bits)                                                                     \
            n = bits - 1;                                                                  \
        return (uint##bits##_t)((int##sbits##_t)a >> n);                                   \
    }                                                                                      \
    static __inline uint8_t CARRY##bits(uint##bits##_t a, uint##bits##_t b)                \
    {                                                                                      \
        return (uint8_t)((uint##bits##_t)(a + b) < a);                                     \
    }                                                                                      \
    static __inline uint8_t SCARRY##bits(uint##bits##_t a, uint##bits##_t b)               \
    {                                                                                      \
        uint##bits##_t r = (uint##bits##_t)(a + b);                                        \
        return (uint8_t)((((a ^ r) & (b ^ r)) >> (bits - 1)) & 1);                         \
    }                                                                                      \
    static __inline uint8_t SBORROW##bits(uint##bits##_t a, uint##bits##_t b)              \
    {                                                                                      \
        uint##bits##_t r = (uint##bits##_t)(a - b);                                        \
        return (uint8_t)((((a ^ b) & (a ^ r)) >> (bits - 1)) & 1);                         \
    }
SHDEF(8, 8)
SHDEF(16, 16)
SHDEF(32, 32)
SHDEF(64, 64)

/* x86 P-code only counts bits for the parity flag, of the result's low byte: a table */
extern const uint8_t g_popcnt8[256];
static __inline uint8_t POPCNT(uint64_t v)
{
    uint8_t n = 0;
    if (v < 256)
        return g_popcnt8[v];
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}

/* floating point: 4/8-byte values travel as bit patterns, x87 values as doubles */
static __inline float F32(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static __inline double F64(uint64_t b)
{
    double f;
    memcpy(&f, &b, 8);
    return f;
}
static __inline uint32_t FBITS32(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
static __inline uint64_t FBITS64(double f)
{
    uint64_t b;
    memcpy(&b, &f, 8);
    return b;
}

/* FNSTSW: Ghidra keeps only the condition codes in the status word; add the stack top
   (tracked through fdelta, the net pushes since the machine code handed over) and the sticky
   exception flags the FPU had then. The generated code keeps fdelta and the status word
   (sw, with the condition codes) in locals. */
static __inline uint16_t FSWD(const RCtx *c, int fdelta, uint16_t sw)
{
    unsigned top = ((unsigned)(c->fsw0 >> 11) - (unsigned)fdelta) & 7;
    return (uint16_t)((sw & 0x4700) | (top << 11) | (c->fsw0 & 0xFF));
}

/* x87 precision control (control word bits 8-9): 0 = single, 2 = double, 3 = extended (as
   double). The generated code passes the control word, which it keeps in a local. */
static __inline double FPCW(uint16_t cw, double v)
{
    return ((cw >> 8) & 3) == 0 ? (double)(float)v : v;
}

/* The same in a function specialised on the precision at its entry (recomp.py fp_specialize):
   sp is a constant there, 1 for single precision (the game's mode once Direct3D is up). Then
   the compiler can do the arithmetic of float operands in float: a double result rounded once to
   float is exactly the float result of + - * / on floats, so it drops the double round trips. */
#define FPCS(sp, cw, v) ((sp) ? (double)(float)(v) : FPCW(cw, v))

/* x87 round to integer by the rounding control (control word bits 10-11), any value */
static __inline double FROUND_SLOW(uint16_t cw, double v)
{
    switch ((cw >> 10) & 3) {
    case 0: {   /* nearest, ties to even */
        double f = floor(v), d = v - f;
        if (d > 0.5 || (d == 0.5 && fmod(f, 2.0) != 0))
            f += 1;
        return f;
    }
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return v < 0 ? ceil(v) : floor(v);
    }
}

/* The same for FRNDINT and every FIST/FISTP before its conversion. The game converts floats to
   integers all the time, so values in int32 range (all of them in practice) take a fast exact
   path: a truncating conversion, then floor / ceil from it, and round-to-nearest-even with the
   1.5 * 2^52 trick (adding it leaves no fraction bits, so the hardware's own nearest-even
   rounding does the work; double arithmetic is SSE2 or ARM here, never x87). The result takes
   v's sign, as on the x87 (-0.4 gives -0; FROUND_SLOW's nearest case gave +0). Checked against
   FROUND_SLOW on 80 million values in all four modes: only that sign differed. */
static __inline double FROUNDW(uint16_t cw, double v)
{
    double t;
    if (!(v > -2147483648.0 && v < 2147483648.0))   /* big (integral already), or NaN */
        return FROUND_SLOW(cw, v);
    switch ((cw >> 10) & 3) {
    case 0:
        t = (v + 6755399441055744.0) - 6755399441055744.0;
        break;
    case 1:
        t = (double)(int32_t)v;
        if (t > v)
            t -= 1.0;
        break;
    case 2:
        t = (double)(int32_t)v;
        if (t < v)
            t += 1.0;
        break;
    default:
        t = (double)(int32_t)v;
        break;
    }
    return F64(FBITS64(t) | (FBITS64(v) & 0x8000000000000000ull));
}

/* float to integer as FIST does it: out-of-range values give the "integer indefinite" */
static __inline uint32_t FTOI32(double v)
{
    if (!(v >= -2147483648.0 && v < 2147483648.0))
        return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static __inline uint16_t FTOI16(double v)
{
    if (!(v >= -32768.0 && v < 32768.0))
        return 0x8000u;
    return (uint16_t)(int16_t)v;
}
static __inline uint64_t FTOI64(double v)
{
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        return 0x8000000000000000ull;
    return (uint64_t)(int64_t)v;
}

/* MMX saturating arithmetic on 8-byte registers (they alias the x87 registers' low bytes) */
#define MMX_LANES(name, bits, T, lo, hi, op)                                                           static __inline uint64_t name(uint64_t a, uint64_t b)                                              {                                                                                                      uint64_t r = 0, mask = (1ull << bits) - 1;                                                         int sh;                                                                                            for (sh = 0; sh < 64; sh += bits) {                                                                    int v = (int)(T)(a >> sh) op (int)(T)(b >> sh);                                                    if (v < lo)                                                                                            v = lo;                                                                                        if (v > hi)                                                                                            v = hi;                                                                                        r |= ((uint64_t)(uint32_t)v & mask) << sh;                                                     }                                                                                                  return r;                                                                                      }
MMX_LANES(MMX_PADDUSB, 8, uint8_t, 0, 255, +)
MMX_LANES(MMX_PSUBUSB, 8, uint8_t, 0, 255, -)
MMX_LANES(MMX_PADDSB, 8, int8_t, -128, 127, +)
MMX_LANES(MMX_PSUBSB, 8, int8_t, -128, 127, -)
MMX_LANES(MMX_PADDUSW, 16, uint16_t, 0, 65535, +)
MMX_LANES(MMX_PSUBUSW, 16, uint16_t, 0, 65535, -)
MMX_LANES(MMX_PADDSW, 16, int16_t, -32768, 32767, +)
MMX_LANES(MMX_PSUBSW, 16, int16_t, -32768, 32767, -)

/* CPUID: the guest address of four dwords EAX, EBX, EDX, ECX (Ghidra's layout) */
uint32_t rt_cpuid(uint32_t leaf);
uint32_t rt_port_in(RCtx *c, uint32_t port);
void rt_port_out(RCtx *c, uint32_t port);

/* 80-bit memory operands (FLD/FSTP tbyte) */
double rt_ld80(uint32_t a);
void rt_st80(RCtx *c, uint32_t a, double v);

/* a hook mml.dll installed at this address (tools/recomp/patch_sites.txt): run its handler */
void rt_hook(RCtx *c, uint32_t site);

/* calls and jumps the recompiler couldn't resolve statically */
void rt_call(RCtx *c, uint32_t target);
void rt_jump(RCtx *c, uint32_t target);

/* the game's cooperative threads (recomp.c): called where the game calls its own
   scheduler 0x4EED08, create 0x4EED79, yield 0x4EEDA4, exec 0x4EEDCA and exit 0x4EEDFD */
void rt_co_schedule(RCtx *c);
void rt_co_create(RCtx *c);
void rt_co_yield(RCtx *c);
void rt_co_exec(RCtx *c);
void rt_co_exit(RCtx *c);

/* calls to recompiled functions: per function, the runtime can choose to run the original
   machine code instead (code that mml.dll patches, so its changes keep working) */
extern uint8_t *g_rc_native;
void rt_call_native(RCtx *c, uint32_t target);
/* A generated function returns 0 when it returned (RET), or the address it jumps on to: a
   jump into another function. The caller's dispatcher runs that one, and whatever it jumps to,
   at a constant C stack depth (a loop whose back edge crosses functions would otherwise nest). */
uint32_t rt_dispatch(RCtx *c, uint32_t target);
#define RC_CALL(c, i, a, f)                                                                        \
    do {                                                                                           \
        if (g_rc_native[i]) {                                                                      \
            rt_call_native(c, a);                                                                  \
        } else {                                                                                   \
            uint32_t t_ = f(c);                                                                    \
            if (t_)                                                                                \
                rt_dispatch(c, t_);                                                                \
        }                                                                                          \
    } while (0)
void rt_unsupported(RCtx *c, uint32_t at, const char *what);

typedef uint32_t (*rfunc)(RCtx *c);   /* returns a tail-jump target or 0 */

/* A JMP that mml.dll put at a function's entry sends the function to our routine; when the
   runtime runs that routine's C version, which calls the original, the entry is passed once. */
extern uint32_t g_rt_bypass;
static __inline int rt_bypass_take(uint32_t a)
{
    if (g_rt_bypass != a)
        return 0;
    g_rt_bypass = 0;
    return 1;
}
typedef struct {
    uint32_t addr;
    rfunc fn;
    const char *name;
    uint8_t steal;            /* bytes of whole, position-independent instructions at the entry (0 = can't hook) */
    uint8_t pure;             /* everything it reaches is recompiled code (safe to run twice in diff mode) */
    uint8_t entry_bytes[16];
    uint32_t lo, hi;          /* the function's code range */
    const uint16_t *callees;  /* indices into the table */
    uint16_t ncallees;
} RFuncInfo;
