/* Culling for screens wider than 16:9 (Fill screen on an ultrawide monitor or a 20:9 phone).
 *
 * The game decides what to draw from a view that is narrower than ours. The exe patches widened it
 * for 16:9 (RE-notes 3.27-3.35); past 16:9 the sides still lost things, which popped in as the
 * camera turned. Three places, each widened here only when the picture is wider than 854x480, so
 * 16:9 and 4:3 keep exactly the patched game's behaviour:
 *
 * 1. The terrain chunk test (terrain_emit_chunks 0x4344F0, every map piece: ground, walls,
 *    buildings, the 2D trees): a chunk passes if |x| <= (z >> 2) * 77 >> 5 + its radius in camera
 *    space, a half-width of x/z 0.60 (the stock 106 >> 6 was 0.41, 4:3). The view needs
 *    x/z = (width / 4) / H (H the projection distance, 330 in play): 1.29 at 32:9. The multiplier
 *    and shift (0x43454E, 0x434551) follow H and the width, 10% over.
 *
 * 2. The chunk walker. FUN_00433C30 picks the cells to test: a wedge from the camera (its yaw
 *    +-22.5 degrees, the 4:3 view) widened by +-16 cells (the 16:9 patch), walked row by row along
 *    whichever axis the camera faces most (FUN_004340C4 along x, FUN_004342E7 along z), plus a
 *    window around Mega Man. Wider than 16:9 the view's edges leave that wedge (past 45 degrees
 *    from the camera's axis at 21:9 and up: no slope of the row walk can follow them), so both
 *    walkers are replaced: every non-empty cell of the same room range in front of the camera
 *    (horizontally, with 4 cells' slack behind) whose lateral distance is within the view's width
 *    plus 16 cells, in the walker's own order and skipping Mega Man's window like it does. The
 *    chunk test above still does the exact culling; this only offers it the cells.
 *
 * 3. Actors and props (FUN_004EAF76, 004EB5D4, 004EC000, 004EE1A3): drawn when the projected
 *    centre x + 352 is within 0..0x400 (a 16-bit unsigned compare), centre +-512 in the game's
 *    320-wide space, 352 past the 4:3 edge. Past 16:9 the edge itself moves out to +-427 (32:9),
 *    so wide characters near the side vanished early. The window keeps that 352 past the edge:
 *    the float constant (data) and the compare's limit (code) move together.
 *
 * Code bytes here are mem_write patches the recompiled game reads at run time
 * (tools/recomp/patch_sites.txt); the walkers' entries are jumps into our routines, which the
 * recompiled game reaches as recomp_register_detour C functions. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "addr.h"
#include "guest.h"
#include "hook.h"
#include "log.h"
#include "recomp.h"
#include "widecull.h"

#define BASE_WIDTH 854               /* 16:9: the exe's own widening covers up to this */

/* 1. terrain chunk test */
#define G_CHUNK_MUL     GA(0x43454Eu)   /* imul eax,eax,imm8 (77) */
#define G_CHUNK_SHIFT   GA(0x434551u)   /* sar eax,imm8 (5) */
static const uint8_t k_chunk_mul_shift[2] = { 0x4D, 0x05 };

/* 2. chunk walker */
#define G_FN_EMIT       GA(0x4344F0u)   /* terrain_emit_chunks: watcall EAX x, EDX z, EBX cell, ECX pos */
#define G_FN_WALK_X     GA(0x4340C4u)   /* watcall EAX x0, EDX x1, EBX z0, ECX z1 */
#define G_FN_WALK_Z     GA(0x4342E7u)   /* same arguments */
#define G_CHUNK_RADIUS  GA(0x434187u)   /* the walker's mov word [esp+6],0x4B0: the radius it hands the test */
#define G_CAM_X         0x56F184u       /* s16 camera position (the walker's [0x56F182] >> 16) */
#define G_CAM_Z         0x56F188u
#define G_CAM_YAW       0x77EA0Au       /* u16 12-bit */
#define G_GRID          0x77E700u       /* u32 guest pointer to the room's s16 cells */
#define G_WIN_X0        0x77EAA0u       /* s16 Mega Man's window: x0 x1 z0 z1 (the walkers skip it) */
#define G_WIN_X1        0x77EAA2u
#define G_WIN_Z0        0x77EAA4u
#define G_WIN_Z1        0x77EAA6u
#define G_GRID_W        0x77EAAEu       /* u16 cells per row */
#define G_GRID_Z0       0x77EAC3u       /* u8 the grid's first z / x cell */
#define G_GRID_X0       0x77EAC5u
static const uint8_t k_walk_x_prologue[6] = { 0x56, 0x57, 0x55, 0x83, 0xEC, 0x30 };   /* push esi/edi/ebp, sub esp,0x30 */
static const uint8_t k_walk_z_prologue[6] = { 0x56, 0x57, 0x55, 0x83, 0xEC, 0x24 };
#define CELL 512.0
#define SLACK_BEHIND (4.0 * CELL)
#define SLACK_SIDE (16.0 * CELL)        /* as the walker's own +-16 cells */

/* 3. actors and props: the FADD constant (x + it) and the compare's limit (imm16) */
static const struct { uintptr_t fadd; uintptr_t cmp_imm; } k_actor_sites[] = {
    { 0x50A4B0u, 0x4EB017u },   /* FUN_004EAF76 */
    { 0x50A4B8u, 0x4EB682u },   /* FUN_004EB5D4, two paths share the constant */
    { 0x50A4B8u, 0x4EB6A5u },
    { 0x50A4C8u, 0x4EC098u },   /* FUN_004EC000 */
    { 0x50A4DCu, 0x4EE265u },   /* FUN_004EE1A3 */
};
#define ACTOR_PAST_EDGE 352.0f      /* the 4:3 game's margin past the screen edge */
#define GAME_CENTRE_X 160.0f        /* the game's screen x of the centre (320-wide space) */

static int g_on;
static int g_width = 640;
static double g_view_t = 0.6;       /* tan of half the horizontal view at the current H */
static int g_walk_on;
static uint32_t g_pos;              /* the s16[4] position the chunk test reads: in guest memory */
static unsigned g_frames, g_cells, g_emits;

#ifndef MML_RUNNER
static uint8_t *g_tramp_x, *g_tramp_z;
static volatile LONG g_handled;
static uint32_t g_fn_emit;          /* G_FN_EMIT in the running program */

/* cdecl (x, z, cell, pos) -> watcall terrain_emit_chunks; it changes EBX ECX EDX without saving them */
static __declspec(naked) void emit_thunk(void)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax, [esp + 20]
        mov edx, [esp + 24]
        mov ebx, [esp + 28]
        mov ecx, [esp + 32]
        mov esi, g_fn_emit
        call esi
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}
#endif

static void emit(int x, int z, int cell)
{
    g_emits++;
    if (recomp_in_shim()) {   /* the recompiled game: its own function, no machine code */
        uint32_t regs[4];
        regs[0] = (uint32_t)x, regs[1] = (uint32_t)z, regs[2] = (uint32_t)cell, regs[3] = g_pos;
        recomp_call_watcall(G_FN_EMIT, regs, 0, NULL);
        return;
    }
#ifndef MML_RUNNER
    ((void(__cdecl *)(int, int, int, uint32_t))(void *)emit_thunk)(x, z, cell, g_pos);
#endif
}

/* One walker's work, all of the room range in view. along_x: rows along x (FUN_004340C4's order),
 * else along z. Always 1 (handled): the game's walker never runs once ours is in (in the recompiled
 * game a declined detour runs the original past the entry jump, whose other bytes it would check
 * for jumps of their own). */
static int walk(int along_x, int x0, int x1, int z0, int z1)
{
    double yaw, fx, fz, cx, cz, tw;
    int wx0, wx1, wz0, wz1, gw, gx0, gz0, a, b;
    uint32_t grid;
    volatile int16_t *pos;

    grid = GVAR(uint32_t, G_GRID);
    gw = GVAR(uint16_t, G_GRID_W);
    if (!grid || gw <= 0)
        return 1;
    gx0 = GVAR(uint8_t, G_GRID_X0);
    gz0 = GVAR(uint8_t, G_GRID_Z0);
    wx0 = GVAR(int16_t, G_WIN_X0), wx1 = GVAR(int16_t, G_WIN_X1);
    wz0 = GVAR(int16_t, G_WIN_Z0), wz1 = GVAR(int16_t, G_WIN_Z1);
    yaw = (GVAR(uint16_t, G_CAM_YAW) & 0xFFF) * (6.283185307179586 / 4096.0);
    fx = sin(yaw), fz = cos(yaw);   /* the camera's forward on the ground (x, z) */
    cx = GVAR(int16_t, G_CAM_X), cz = GVAR(int16_t, G_CAM_Z);
    tw = g_view_t * 1.15;
    pos = (volatile int16_t *)GUEST(g_pos);
    pos[3] = (int16_t)GVAR(uint16_t, G_CHUNK_RADIUS);
    g_frames++;
    for (a = along_x ? x0 : z0; a < (along_x ? x1 : z1); a++)
        for (b = along_x ? z0 : x0; b < (along_x ? z1 : x1); b++) {
            int x = along_x ? a : b, z = along_x ? b : a, cell;
            double dx, dz, depth, side;
            if (x >= wx0 && x < wx1 && z >= wz0 && z < wz1)
                continue;   /* Mega Man's window: FUN_00433C30 walks it after this */
            cell = GVAR(int16_t, grid + 2u * (uint32_t)((x - gx0) + (z - gz0) * gw));
            if (!cell)
                continue;
            g_cells++;
            dx = (double)((x << 9) - 0x7F00) - cx;
            dz = (double)((z << 9) - 0x7F00) - cz;
            depth = dx * fx + dz * fz;
            side = fabs(dx * fz - dz * fx);
            if (depth < -SLACK_BEHIND || side > (depth > 0.0 ? depth : 0.0) * tw + SLACK_SIDE)
                continue;
            pos[0] = (int16_t)((x << 9) - 0x7F00);
            pos[2] = (int16_t)((z << 9) - 0x7F00);
            emit(x, z, cell);
        }
    return 1;
}

#ifndef MML_RUNNER
static int __cdecl walk_x_native(int x0, int x1, int z0, int z1) { return walk(1, x0, x1, z0, z1); }
static int __cdecl walk_z_native(int x0, int x1, int z0, int z1) { return walk(0, x0, x1, z0, z1); }

static __declspec(naked) void walk_x_detour(void)
{
    __asm {
        pushad
        push ecx
        push ebx
        push edx
        push eax
        call walk_x_native
        add esp, 16
        mov g_handled, eax
        popad
        cmp g_handled, 0
        jne handled
        jmp dword ptr [g_tramp_x]
    handled:
        ret
    }
}

static __declspec(naked) void walk_z_detour(void)
{
    __asm {
        pushad
        push ecx
        push ebx
        push edx
        push eax
        call walk_z_native
        add esp, 16
        mov g_handled, eax
        popad
        cmp g_handled, 0
        jne handled
        jmp dword ptr [g_tramp_z]
    handled:
        ret
    }
}
#endif

/* the same for the recompiled game (recomp.c): regs EAX ECX EDX EBX ESP EBP ESI EDI */
static int walk_x_c(const uint32_t regs[8])
{
    return walk(1, (int)regs[0], (int)regs[2], (int)regs[3], (int)regs[1]);
}

static int walk_z_c(const uint32_t regs[8])
{
    return walk(0, (int)regs[0], (int)regs[2], (int)regs[3], (int)regs[1]);
}

/* the jump at a walker's entry into our routine (and, for the machine code, a trampoline back) */
static int detour_walker(uintptr_t fn, const uint8_t *prologue, void *native_routine, int (*c_fn)(const uint32_t[8]),
                         uint8_t **tramp)
{
    uint8_t jmp[5] = { 0xE9 };
    void *routine;
    int32_t rel;
    if (!mem_expect(fn, prologue, 6))
        return 0;
#ifndef MML_RUNNER
    *tramp = (uint8_t *)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!*tramp)
        return 0;
    memcpy(*tramp, prologue, 6);
    (*tramp)[6] = 0xE9;
    rel = (int32_t)((fn + 6) - ((uintptr_t)*tramp + 11));
    memcpy(*tramp + 7, &rel, 4);
    routine = native_routine;
#else
    (void)native_routine;
    (void)tramp;
    routine = (void *)c_fn;   /* no machine code: the C version is all, known by this one */
#endif
    rel = (int32_t)(recomp_routine_address(routine) - (fn + 5));
    memcpy(jmp + 1, &rel, 4);
    if (!mem_write(fn, jmp, sizeof jmp))
        return 0;
    recomp_register_detour(routine, fn, c_fn, 0);
    return 1;
}

/* the chunk test's half-width for the view at projection distance h (game thread: H setters) */
void widecull_set_h(float h)
{
    static uint8_t cur[2] = { 0x4D, 0x05 };
    double need;
    uint8_t want[2];
    int shift;
    if (!g_on || h <= 1.0f)
        return;
    g_view_t = (g_width / 4.0) / h;
    need = 4.0 * g_view_t * 1.10;          /* x <= (z >> 2) * mul >> shift */
    want[0] = k_chunk_mul_shift[0], want[1] = k_chunk_mul_shift[1];
    if (need > 77.0 / 32.0) {               /* never narrower than the exe's own */
        for (shift = 5; shift > 1 && ceil(need * (1 << shift)) > 127.0; shift--)
            ;
        want[0] = (uint8_t)(int)fmin(127.0, ceil(need * (1 << shift)));
        want[1] = (uint8_t)shift;
    }
    if (want[0] == cur[0] && want[1] == cur[1])
        return;
    host_write(GUEST(G_CHUNK_MUL), &want[0], 1);
    host_write(GUEST(G_CHUNK_SHIFT), &want[1], 1);
    cur[0] = want[0], cur[1] = want[1];
    log_printf("widecull: H %.1f, view x/z %.3f: chunk test %d >> %d (x/z %.3f)", (double)h, g_view_t,
               want[0], want[1], want[0] / (4.0 * (1 << want[1])));
}

int widecull_install(int width)
{
    size_t i;
    float edge, c;
    uint16_t limit;
    g_width = width;
    if (width <= BASE_WIDTH) {
        log_printf("widecull: %dx480 is no wider than 16:9, the game's culling as patched", width);
        return 0;
    }
    if (!mem_expect(G_CHUNK_MUL - 2, "\x6B\xC0", 2) || !mem_expect(G_CHUNK_MUL, k_chunk_mul_shift, 1) ||
        !mem_expect(G_CHUNK_SHIFT - 2, "\xC1\xF8", 2) || !mem_expect(G_CHUNK_SHIFT, k_chunk_mul_shift + 1, 1)) {
        log_printf("widecull: chunk test code unexpected, skipped");
        return 0;
    }
    /* the first writes go through mem_write (the recompiled game's list); widecull_set_h rewrites */
    mem_write(G_CHUNK_MUL, k_chunk_mul_shift, 1);
    mem_write(G_CHUNK_SHIFT, k_chunk_mul_shift + 1, 1);
    g_on = 1;

    /* 3. actors: the centre may be ACTOR_PAST_EDGE beyond the wide edge, as it could beyond 4:3's */
    edge = (float)width / 4.0f;
    c = edge + ACTOR_PAST_EDGE - GAME_CENTRE_X;   /* x + c in 0..limit */
    limit = (uint16_t)(2.0f * (edge + ACTOR_PAST_EDGE));
    for (i = 0; i < sizeof k_actor_sites / sizeof k_actor_sites[0]; i++) {
        static const uint8_t cmp_400[4] = { 0x66, 0x3D, 0x00, 0x04 };
        float was;
        if (!mem_expect(GA(k_actor_sites[i].cmp_imm) - 2, cmp_400, sizeof cmp_400)) {
            log_printf("widecull: actor test %08X unexpected, skipped", (unsigned)GA(k_actor_sites[i].cmp_imm));
            continue;
        }
        was = GVAR(float, GA(k_actor_sites[i].fadd));
        if (was != 352.0f && was != c) {
            log_printf("widecull: actor test constant %08X is %.1f, skipped", (unsigned)GA(k_actor_sites[i].fadd),
                       (double)was);
            continue;
        }
        mem_write(GA(k_actor_sites[i].fadd), &c, 4);
        mem_write(GA(k_actor_sites[i].cmp_imm), &limit, 2);
    }

    /* 2. the walkers */
    g_pos = guest_alloc(8);
#ifndef MML_RUNNER
    g_fn_emit = (uint32_t)G_FN_EMIT;
    g_walk_on = detour_walker(G_FN_WALK_X, k_walk_x_prologue, (void *)walk_x_detour, walk_x_c, &g_tramp_x) &&
                detour_walker(G_FN_WALK_Z, k_walk_z_prologue, (void *)walk_z_detour, walk_z_c, &g_tramp_z);
#else
    g_walk_on = detour_walker(G_FN_WALK_X, k_walk_x_prologue, NULL, walk_x_c, NULL) &&
                detour_walker(G_FN_WALK_Z, k_walk_z_prologue, NULL, walk_z_c, NULL);
#endif
    log_printf("widecull: %dx480 (wider than 16:9): actors drawn to %.0f px past the centre, chunk walk %s",
               width, (double)(edge + ACTOR_PAST_EDGE), g_walk_on ? "over the room in view" : "FAILED (the game's own)");
    return 1;
}

void widecull_frame(void)
{
    if (g_on && g_frames >= 1800) {
        log_printf("widecull: %u walks, %u cells, %u tested (per walk %u / %u)", g_frames, g_cells, g_emits,
                   g_cells / g_frames, g_emits / g_frames);
        g_frames = g_cells = g_emits = 0;
    }
}
