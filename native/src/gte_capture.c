/* Feeds interp.c from the game's software GTE (x86 hooks; the native port
 * would call interp_vertex() straight from its own GTE instead).
 *
 * Only RTPS (0x4EFDC9, one vertex) and RTPT (0x4EFE75, three) call the
 * perspective divide. Right after each divide the GTE globals hold the
 * vertex's model-space input (V0/V1/V2), its camera-space position and its
 * screen output, and the caller's return address says which code drew it. */
#include <windows.h>
#include <stdint.h>

#include "gte_capture.h"
#include "hook.h"
#include "interp.h"
#include "log.h"

#define G_GTE_V0   0x77EB80u   /* SVECTOR inputs */
#define G_GTE_V1   0x77EB78u
#define G_GTE_V2   0x77EB70u
#define G_GTE_CAM  0x77EB28u   /* float[3]: camera-space result of the last rotate+translate */
#define G_GTE_SXY0 0x77EB88u   /* float SX, SY pairs (fifo) */
#define G_GTE_SXY1 0x77EB90u
#define G_GTE_SXY2 0x77EB98u

#define G_GTE_ROT   0x77EAF8u  /* float[9] */
#define G_GTE_TRANS 0x77EB1Cu  /* s32[3] */
/* terrain emitters live here; their GTE matrix is the plain world -> camera one */
#define TERRAIN_CODE_LO 0x433000u
#define TERRAIN_CODE_HI 0x436000u

#define SITE_RTPS     0x4EFE61u   /* after RTPS's divide; 4 pushes before it */
#define SITE_RTPT_V0  0x4EFEDFu   /* after each of RTPT's divides; 2 pushes */
#define SITE_RTPT_V1  0x4EFF3Du
#define SITE_RTPT_V2  0x4EFF9Bu

static LARGE_INTEGER g_build_start, g_qpf;
static double g_build_ms;
static unsigned g_frames;
static int g_census;

#define SITES 64
static struct { uint32_t ret; unsigned n; } g_site_counts[SITES];

static void report(uint32_t ret, uintptr_t v, uintptr_t sxy)
{
    if (g_census) {
        int i;
        for (i = 0; i < SITES && g_site_counts[i].ret && g_site_counts[i].ret != ret; i++)
            ;
        if (i < SITES) {
            g_site_counts[i].ret = ret;
            g_site_counts[i].n++;
        }
    }
    if (!g_build_start.QuadPart)
        QueryPerformanceCounter(&g_build_start);
    if (ret >= TERRAIN_CODE_LO && ret < TERRAIN_CODE_HI) {
        const int32_t *t = (const int32_t *)G_GTE_TRANS;
        float tf[3];
        tf[0] = (float)t[0];
        tf[1] = (float)t[1];
        tf[2] = (float)t[2];
        interp_camera((const float *)G_GTE_ROT, tf);
    }
    interp_vertex(ret, (const int16_t *)v, (const float *)G_GTE_CAM,
                  *(const float *)sxy, *(const float *)(sxy + 4));
}

/* r->esp is the game's esp minus 4 (the stub's pushfd) */
static void on_rtps(regs_t *r)    { report(*(uint32_t *)(uintptr_t)(r->esp + 4 + 16), G_GTE_V0, G_GTE_SXY2); }
static void on_rtpt_v0(regs_t *r) { report(*(uint32_t *)(uintptr_t)(r->esp + 4 + 8), G_GTE_V0, G_GTE_SXY0); }
static void on_rtpt_v1(regs_t *r) { report(*(uint32_t *)(uintptr_t)(r->esp + 4 + 8), G_GTE_V1, G_GTE_SXY1); }
static void on_rtpt_v2(regs_t *r) { report(*(uint32_t *)(uintptr_t)(r->esp + 4 + 8), G_GTE_V2, G_GTE_SXY2); }

int gte_capture_install(int census)
{
    static const uint8_t mov_eax_sz3[] = { 0xA1, 0xA0, 0xEB, 0x77, 0x00 };    /* mov eax,[0x77EBA0] */
    static const uint8_t mov_ebx_cam[] = { 0xBB, 0x28, 0xEB, 0x77, 0x00 };    /* mov ebx,0x77EB28 */

    if (!hook_mid(SITE_RTPS, sizeof mov_eax_sz3, mov_eax_sz3, on_rtps) ||
        !hook_mid(SITE_RTPT_V0, sizeof mov_ebx_cam, mov_ebx_cam, on_rtpt_v0) ||
        !hook_mid(SITE_RTPT_V1, sizeof mov_ebx_cam, mov_ebx_cam, on_rtpt_v1) ||
        !hook_mid(SITE_RTPT_V2, sizeof mov_eax_sz3, mov_eax_sz3, on_rtpt_v2)) {
        log_printf("interp: GTE hooks failed");
        return 0;
    }
    QueryPerformanceFrequency(&g_qpf);
    g_census = census;
    log_printf("interp: GTE capture on");
    return 1;
}

void gte_capture_frame_end(void)
{
    LARGE_INTEGER now;
    interp_stats st;

    if (g_build_start.QuadPart) {
        QueryPerformanceCounter(&now);
        g_build_ms += (double)(now.QuadPart - g_build_start.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
        g_build_start.QuadPart = 0;
    }
    interp_frame_end();
    if (!g_census || ++g_frames % 60)
        return;
    interp_get_stats(&st, 1);
    log_printf("interp census: %u vertices projected (last frame), %u screen collisions, "
               "draw lookups %u: found %u (%.1f%%), matched to previous frame %u (%.1f%%), scene build %.2f ms/frame",
               st.projected, st.collisions, st.lookups, st.found,
               st.lookups ? 100.0 * st.found / st.lookups : 0.0, st.matched,
               st.lookups ? 100.0 * st.matched / st.lookups : 0.0, g_build_ms / 60.0);
    g_build_ms = 0.0;
    {
        int i;
        for (i = 0; i < SITES && g_site_counts[i].ret; i++) {
            log_printf("interp census: site %08X projected %u (60 frames)", g_site_counts[i].ret, g_site_counts[i].n);
            g_site_counts[i].n = 0;
        }
    }
}
