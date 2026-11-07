/* Crosshair for camera aim (modern controls, [controls] aim=camera + crosshair=1).
 *
 * Camera-aimed shots fly toward the point the camera looks at (controls.c, vertical aim), so the
 * marker sits on that point, projected through the game's camera like the lock-on reticle: the
 * middle of the view. (Without vertical aim it marks where level shots go instead.) Four
 * chamfered corner brackets in the reticle's colour with a dark rim, no centre point: the shots
 * are big and not that precise.
 *
 * Drawn with our UI over the final picture (gfx_ui), in window pixels: the game's own 320x240
 * HUD space snapped its corners to a 4.5-pixel grid at 1080p. The game's screen maps to the
 * window as display.c and the backends place it: device x = 2 x + the widescreen shift,
 * device y = 2 y on a W x 480 surface, fitted into the window keeping its aspect. Edges fade over
 * one pixel (antialiasing); corners are mitred. */
#include <math.h>
#include <stdint.h>

#include "controls.h"
#include "crosshair.h"
#include "display.h"
#include "game.h"
#include "gfx_ui.h"
#include "guest.h"
#include "lockon.h"
#include "menu.h"

#define G_CAM_YAW_ANGLE 0x77EA0Au
#define PI 3.14159265358979323846
#define XH_DIST   2000.0
#define XH_CHEST  100       /* chest height above the player's position (y grows downward) */
/* sizes as fractions of the picture's height */
#define XH_HALF   0.042f    /* centre to the box's edge */
#define XH_ARM    0.021f    /* a bracket arm's length from the box corner */
#define XH_CHAMF  0.008f    /* the 45-degree cut at the corner */
#define XH_THICK  0.0026f   /* half line thickness */
#define XH_RIM    0.0016f   /* dark rim around the lines */
#define XH_FEATHER 1.0f     /* window pixels the outer edge fades over */
#define XH_ALPHA  0.65f     /* at full visibility; it fades out a few seconds after the last shot */

/* the polyline P[0..3] offset sideways by d (left normal), mitred at the two inner points; the
   ends pushed out along the line by `ext` */
static void offset(const float p[4][2], float d, float ext, float o[4][2])
{
    float n[3][2], t0[2], t2[2];
    int k;
    for (k = 0; k < 3; k++) {
        float dx = p[k + 1][0] - p[k][0], dy = p[k + 1][1] - p[k][1], l = sqrtf(dx * dx + dy * dy);
        n[k][0] = -dy / l;
        n[k][1] = dx / l;
    }
    t0[0] = n[0][1], t0[1] = -n[0][0];   /* along the first segment */
    t2[0] = n[2][1], t2[1] = -n[2][0];   /* along the last */
    o[0][0] = p[0][0] + n[0][0] * d - t0[0] * ext;
    o[0][1] = p[0][1] + n[0][1] * d - t0[1] * ext;
    o[3][0] = p[3][0] + n[2][0] * d + t2[0] * ext;
    o[3][1] = p[3][1] + n[2][1] * d + t2[1] * ext;
    for (k = 1; k <= 2; k++) {
        float mx = n[k - 1][0] + n[k][0], my = n[k - 1][1] + n[k][1], ml = sqrtf(mx * mx + my * my), dot;
        mx /= ml, my /= ml;
        dot = mx * n[k][0] + my * n[k][1];
        o[k][0] = p[k][0] + mx * d / dot;
        o[k][1] = p[k][1] + my * d / dot;
    }
}

/* the strip between two offsets of the polyline, colour c1 along the first and c2 along the second */
static void band(const float p[4][2], float d1, float d2, float ext, uint32_t c1, uint32_t c2)
{
    float a[4][2], b[4][2], xy[8];
    uint32_t c[4];
    int k;
    offset(p, d1, ext, a);
    offset(p, d2, ext, b);
    c[0] = c[1] = c1;
    c[2] = c[3] = c2;
    for (k = 0; k < 3; k++) {
        xy[0] = a[k][0], xy[1] = a[k][1];
        xy[2] = a[k + 1][0], xy[3] = a[k + 1][1];
        xy[4] = b[k + 1][0], xy[5] = b[k + 1][1];
        xy[6] = b[k][0], xy[7] = b[k][1];
        gfx_ui_quad(xy, c);
    }
}

/* the square end of the polyline at point `e` (0 or 3), from the line's end out by ext, d wide each side */
static void cap(const float p[4][2], int e, float d, float ext, uint32_t col)
{
    float a[4][2], b[4][2], c[4][2], f[4][2], xy[8];
    uint32_t cc[4];
    offset(p, d, 0, a);
    offset(p, -d, 0, b);
    offset(p, -d, ext, c);
    offset(p, d, ext, f);
    xy[0] = a[e][0], xy[1] = a[e][1];
    xy[2] = b[e][0], xy[3] = b[e][1];
    xy[4] = c[e][0], xy[5] = c[e][1];
    xy[6] = f[e][0], xy[7] = f[e][1];
    cc[0] = cc[1] = cc[2] = cc[3] = col;
    gfx_ui_quad(xy, cc);
}

static uint32_t argb(float a, unsigned rgb)
{
    return (uint32_t)(a * 255.0f + 0.5f) << 24 | (rgb & 0xFFFFFF);
}

static void draw(void *user, int w, int h)
{
    double yaw, sx, sy, px, py, pz;
    float aw = (float)display_native_width(), ah = 480.0f, s, ox, oy, cx, cy, u, half, rim, fe;
    float a = XH_ALPHA * controls_crosshair();
    unsigned col = lockon_color(), dark;
    int c, k;
    (void)user;
    if (a <= 0.0f || menu_blocks_input())
        return;
    if (controls_aim_point(&px, &py, &pz)) {   /* vertical aim: where the camera looks */
        if (!lockon_project(px, py, pz, &sx, &sy))
            return;
    } else {   /* level shots along the camera's heading */
        yaw = (GVAR(uint16_t, G_CAM_YAW_ANGLE) & 0xfff) * (PI / 2048.0);
        if (!lockon_project(GVAR(int16_t, G_PLAYER + 0x14) + XH_DIST * sin(yaw), GVAR(int16_t, G_PLAYER + 0x16) - XH_CHEST,
                            GVAR(int16_t, G_PLAYER + 0x18) + XH_DIST * cos(yaw), &sx, &sy))
            return;
    }
    s = (float)w / aw < (float)h / ah ? (float)w / aw : (float)h / ah;
    ox = ((float)w - aw * s) / 2;
    oy = ((float)h - ah * s) / 2;
    cx = ox + ((float)sx * 2.0f + display_world_shift()) * s;
    cy = oy + (float)sy * 2.0f * s;
    u = ah * s;   /* the picture's height in window pixels */
    half = XH_THICK * u;
    rim = XH_RIM * u;
    fe = XH_FEATHER;
    dark = ((col >> 16 & 0xFF) / 5) << 16 | ((col >> 8 & 0xFF) / 5) << 8 | (col & 0xFF) / 5;
    for (c = 0; c < 4; c++) {
        float mx = (c & 1) ? -1.0f : 1.0f, my = (c & 2) ? -1.0f : 1.0f, p[4][2];
        /* one corner (bottom right, y down): along the bottom edge, the chamfer, up the side */
        const float base[4][2] = {
            { XH_HALF - XH_ARM, XH_HALF }, { XH_HALF - XH_CHAMF, XH_HALF },
            { XH_HALF, XH_HALF - XH_CHAMF }, { XH_HALF, XH_HALF - XH_ARM },
        };
        for (k = 0; k < 4; k++) {
            p[k][0] = cx + mx * base[k][0] * u;
            p[k][1] = cy + my * base[k][1] * u;
        }
        /* the line, the rim around it (sides and ends, not under it: both are see-through), and
           the rim's fading outer edges */
        band(p, -half, half, 0, argb(a, col), argb(a, col));
        band(p, half, half + rim, 0, argb(a, dark), argb(a, dark));
        band(p, -half, -(half + rim), 0, argb(a, dark), argb(a, dark));
        cap(p, 0, half + rim, rim, argb(a, dark));
        cap(p, 3, half + rim, rim, argb(a, dark));
        band(p, half + rim, half + rim + fe, rim, argb(a, dark), argb(0, dark));
        band(p, -(half + rim), -(half + rim + fe), rim, argb(a, dark), argb(0, dark));
    }
}

void crosshair_install(void)
{
    gfx_ui_add_draw(draw, NULL);
}
