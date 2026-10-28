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

/* guest memory */
void rt_store_log(RCtx *c, uint32_t a, unsigned n);   /* recomp.c: called before a logged write */
#define LDDEF(bits)                                                                        \
    static __inline uint##bits##_t LD##bits(uint32_t a)                                    \
    {                                                                                      \
        uint##bits##_t v;                                                                  \
        memcpy(&v, HPTR(a), sizeof v);                                                     \
        return v;                                                                          \
    }                                                                                      \
    static __inline void ST##bits(RCtx *c, uint32_t a, uint##bits##_t v)                   \
    {                                                                                      \
        if (c->log)                                                                        \
            rt_store_log(c, a, sizeof v);                                                  \
        memcpy(HPTR(a), &v, sizeof v);                                                     \
    }
LDDEF(8)
LDDEF(16)
LDDEF(32)
LDDEF(64)

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

static __inline uint8_t POPCNT(uint64_t v)
{
    uint8_t n = 0;
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
   (tracked through fdelta) and the sticky exception flags the FPU had at entry */
static __inline uint16_t FSW(const RCtx *c)
{
    unsigned top = ((unsigned)(c->fsw0 >> 11) - (unsigned)c->fdelta) & 7;
    return (uint16_t)((R16(c, 0x10A2) & 0x4700) | (top << 11) | (c->fsw0 & 0xFF));
}

/* x87 precision control (CW bits 8-9): 0 = single, 2 = double, 3 = extended (as double) */
static __inline double FPC(const RCtx *c, double v)
{
    return ((R16(c, REG_FPUCW) >> 8) & 3) == 0 ? (double)(float)v : v;
}

/* x87 round to integer by the rounding control (CW bits 10-11) */
static __inline double FROUND(const RCtx *c, double v)
{
    switch ((R16(c, REG_FPUCW) >> 10) & 3) {
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

/* 80-bit memory operands (FLD/FSTP tbyte) */
double rt_ld80(uint32_t a);
void rt_st80(RCtx *c, uint32_t a, double v);

/* calls and jumps the recompiler couldn't resolve statically */
void rt_call(RCtx *c, uint32_t target);
void rt_jump(RCtx *c, uint32_t target);
void rt_unsupported(RCtx *c, uint32_t at, const char *what);

typedef void (*rfunc)(RCtx *c);
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
