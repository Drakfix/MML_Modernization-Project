/* Shot homing: the original game's own, and how much of it shots aimed with the crosshair keep
 * ([controls] shot_homing; RE-notes 7.82).
 *
 * The original (unchanged by us): each frame target_and_hits_update (0x4E0405) first runs the shot
 * spawner FUN_004E0AF0. For a buster shot (player +0x174 bit 0x20) it projects every enemy hit
 * volume through the camera and takes the one nearest the screen's vertical centre line inside a
 * window: x 130..190 of the 320 wide screen (a strip about +-5 degrees wide), ANY height on the
 * screen, from just past Mega Man to 1280 deep. Special weapons (bit 0x10) use a window per weapon.
 * A shot with a target gets a slot (FUN_004E0EC0: bit 0x80000000 >> n in 0x77E940, the shot in
 * 0x7511AC[n], the same bit in the target's record +4) and +3 = 0x80; every frame the target's hit
 * volume copies its position into the shot's +0xCC. The buster's update (0x4B0ADA) turns the
 * shot's direction (+0xA4, s16 x y z, length 4096) toward it with FUN_004DFFB5: at most 15 degrees
 * per update for updates 1-3, 2 degrees for 4-8, 1 degree on update 9, then it frees the slot. So a
 * shot bends up to 56 degrees, 45 of them within its first 0.1 s.
 *
 * The original had no vertical aim, so this is how its shots reached enemies above or below. With
 * the crosshair the shots already go where the player aims, and the window still takes an enemy
 * anywhere up or down the screen: a shot aimed level at a wall curves up onto an enemy on the
 * ceiling. So for buster shots aimed with the crosshair (camera aim or shoulder aim, nothing
 * locked) the slot the spawner just gave is kept only as [controls] shot_homing says:
 *   high  keep it (the original)
 *   low   keep it only for an enemy near the crosshair: within REDUCED_RADIUS of the line from the
 *         buster to the crosshair's point (as an angle from the buster, REDUCED_MIN_DEG ..
 *         REDUCED_MAX_DEG), so the shot follows the enemy it was aimed at and never curves away
 *   off   never
 * The combat styles set it: Classic high, Adventure low, Shooter off (menu.c); with no shot_homing
 * in mml.ini yet, the default follows the aiming settings the same way (homing_read).
 * Freed the way the game frees it (FUN_004E0F14: the bit in 0x77E940) plus +3 = 0, before the
 * shot's first update. Lock-on, shots where Mega Man faces, the special weapons and the original
 * controls are left alone.
 *
 * This runs at our hook in target_and_hits_update (lockon.c on_target_store), right after the
 * spawner. MML_HOMING_LOG=1 logs each buster shot and how far it turned. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "controls.h"
#include "game.h"
#include "guest.h"
#include "homing.h"
#include "lockon.h"
#include "log.h"

#define G_SLOT_BITS   0x77E940u   /* bit 0x80000000 >> n: slot n in use (n < 24) */
#define G_SLOT_OBJ    0x7511ACu   /* u32 shot object per slot */
#define G_VOLUMES     0x750DACu   /* this frame's hit volumes, 12 bytes each (lockon.c) */
#define G_VOLUME_COUNT 0x77E94Cu
#define G_POOL        0x6DC7FCu   /* the shot pool (FUN_004DE0BF): 16 objects of 0x11C... */
#define G_POOL_USED   0x77E780u   /* ...in use = bit 0x80000000 >> n */
#define NSLOT         24
#define NPOOL         16
#define REDUCED_RADIUS  100.0     /* about an enemy's hit volume (the crosshair's own test: 90) */
#define REDUCED_MIN_DEG 5.0
#define REDUCED_MAX_DEG 15.0      /* (the first update's turn: the shot snaps onto it there) */
#define PI 3.14159265358979323846

static int g_mode = HOMING_LOW;
static int g_log = -1;

/* the debug log's per-shot record */
typedef struct {
    uint32_t obj;
    int age, d0[3], dl[3];
    char what[160];
} track_t;
static track_t g_tr[NPOOL];

static int16_t O16(uint32_t obj, uint32_t off) { return GVAR(int16_t, obj + off); }

static double angle_between(const double *a, const double *b)
{
    double la = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]), c;
    if (la < 1e-6 || lb < 1e-6)
        return 0;
    c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    return acos(c > 1 ? 1 : c < -1 ? -1 : c) * (180.0 / PI);
}

/* the hit volume of the record that holds slot bit `bit` (where the spawner aimed the shot) */
static int target_of(uint32_t bit, double *t)
{
    uint32_t n = GVAR(uint32_t, G_VOLUME_COUNT), i;
    for (i = 0; i < n && i < 512; i++) {
        uint32_t e = G_VOLUMES + 12 * i, rec = GVAR(uint32_t, e + 8) & 0x7fffff;
        int paired = (GVAR(uint8_t, e + 7) & 6) != 0;
        if (rec >= 0x400000 && (GVAR(uint32_t, rec + 4) & bit)) {
            t[0] = GVAR(int16_t, e), t[1] = GVAR(int16_t, e + 2), t[2] = GVAR(int16_t, e + 4);
            if (paired && i + 1 < n) {   /* (as the spawner: the midpoint of the pair) */
                t[0] = (t[0] + GVAR(int16_t, e + 12)) / 2;
                t[1] = (t[1] + GVAR(int16_t, e + 14)) / 2;
                t[2] = (t[2] + GVAR(int16_t, e + 16)) / 2;
            }
            return 1;
        }
        if (paired)
            i++;
    }
    return 0;
}

static void release(int slot, uint32_t obj)
{
    uint32_t bit = 0x80000000u >> slot, n = GVAR(uint32_t, G_VOLUME_COUNT), i;
    GVAR(uint32_t, G_SLOT_BITS) &= ~bit;
    GVAR(uint8_t, obj + 3) = 0;
    for (i = 0; i < n && i < 512; i++) {   /* (the target's record: the game drops stale bits itself too) */
        uint32_t rec = GVAR(uint32_t, G_VOLUMES + 12 * i + 8) & 0x7fffff;
        if (rec >= 0x400000)
            GVAR(uint32_t, rec + 4) &= ~bit;
    }
}

static void log_pool(void);
static void log_enemy(void);

void homing_frame(void)
{
    uint32_t bits = GVAR(uint32_t, G_SLOT_BITS);
    int s, aimed = controls_shots_aimed();
    if (g_log < 0) {
        char v[8] = "";
        GetEnvironmentVariableA("MML_HOMING_LOG", v, sizeof v);
        g_log = v[0] == '1';
    }
    for (s = 0; s < NSLOT; s++) {
        uint32_t bit = 0x80000000u >> s, obj;
        double t[3], m[3], a[3], to[3], ta[3], ang, cone, dist;
        int keep, i;
        if (!(bits & bit))
            continue;
        obj = GVAR(uint32_t, G_SLOT_OBJ + 4 * s);
        /* a buster shot the spawner just made (its first update still to come) */
        if (obj < 0x400000 || GVAR(uint8_t, obj + 8) != 0 || O16(obj, 0x1A) != 0 || GVAR(uint8_t, obj + 3) < 0x80)
            continue;
        if (!target_of(bit, t))
            continue;
        m[0] = O16(obj, 0x14), m[1] = O16(obj, 0x16), m[2] = O16(obj, 0x18);
        to[0] = t[0] - m[0], to[1] = t[1] - m[1], to[2] = t[2] - m[2];
        dist = sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        ang = -1;
        if (aimed && controls_aim_point(&a[0], &a[1], &a[2])) {
            ta[0] = a[0] - m[0], ta[1] = a[1] - m[1], ta[2] = a[2] - m[2];
            ang = angle_between(to, ta);
        }
        cone = atan(REDUCED_RADIUS / (dist > 1 ? dist : 1)) * (180.0 / PI);
        cone = cone < REDUCED_MIN_DEG ? REDUCED_MIN_DEG : cone > REDUCED_MAX_DEG ? REDUCED_MAX_DEG : cone;
        keep = ang < 0 || g_mode == HOMING_HIGH || (g_mode == HOMING_LOW && ang <= cone);
        if (!keep)
            release(s, obj);
        if (g_log) {
            for (i = 0; i < NPOOL && G_POOL + 0x11C * i != obj; i++)
                ;
            if (i == NPOOL)
                continue;
            if (ang < 0)
                _snprintf_s(g_tr[i].what, sizeof g_tr[i].what, _TRUNCATE, "target %.0f away, not crosshair-aimed: kept",
                            dist);
            else
                _snprintf_s(g_tr[i].what, sizeof g_tr[i].what, _TRUNCATE,
                            "target %.0f away, %.1f deg off the crosshair (allowed %.1f): %s", dist, ang,
                            g_mode == HOMING_LOW ? cone : g_mode == HOMING_OFF ? 0.0 : 180.0, keep ? "kept" : "dropped");
        }
    }
    if (g_log) {
        static int n;
        log_pool();
        if (++n % 10 == 0)
            log_enemy();
    }
}

/* MML_HOMING_LOG: each buster shot from the pool, its first direction (after its set-up) and its
   last, and the homing decision */
static void dir_angles(const int *d, double *yaw, double *pitch)
{
    *yaw = atan2((double)d[0], (double)d[2]) * (180.0 / PI);
    *pitch = atan2((double)-d[1], sqrt((double)d[0] * d[0] + (double)d[2] * d[2])) * (180.0 / PI);
}

static void log_pool(void)
{
    static uint32_t prev;
    uint32_t used = GVAR(uint32_t, G_POOL_USED);
    int i;
    for (i = 0; i < NPOOL; i++) {
        uint32_t o = G_POOL + 0x11C * i, bit = 0x80000000u >> i;
        track_t *t = &g_tr[i];
        if ((used & bit) && !(prev & bit)) {   /* new this frame */
            t->obj = (GVAR(uint8_t, o + 2) & 0xF0) == 0x80 && O16(o, 0x1A) == 0 ? o : 0;   /* (the buster's) */
            t->age = 0;
            if (t->obj && GVAR(uint8_t, o + 3) < 0x80 && !t->what[0])
                strcpy_s(t->what, sizeof t->what, "no target");
            continue;
        }
        if (!t->obj)
            continue;
        if (used & bit) {
            int d[3];
            d[0] = O16(o, 0xA4), d[1] = O16(o, 0xA6), d[2] = O16(o, 0xA8);
            if (++t->age == 1)
                memcpy(t->d0, d, sizeof d);
            memcpy(t->dl, d, sizeof d);
            if (t->age < 12)
                continue;
        }
        if (t->age >= 1) {   /* gone, or 11 updates on (the homing ends at 9) */
            double y0, p0, y1, p1, a[3], b[3];
            dir_angles(t->d0, &y0, &p0);
            dir_angles(t->dl, &y1, &p1);
            a[0] = t->d0[0], a[1] = t->d0[1], a[2] = t->d0[2], b[0] = t->dl[0], b[1] = t->dl[1], b[2] = t->dl[2];
            log_printf("homing: shot (%s) left yaw %.1f pitch %.1f, after %d updates yaw %.1f pitch %.1f: turned %.1f deg",
                       t->what, y0, p0, t->age, y1, p1, angle_between(a, b));
        }
        t->obj = 0;
        t->what[0] = 0;
    }
    prev = used;
}

/* MML_HOMING_LOG: the nearest enemy as seen from the camera, degrees right of / above the view's
   centre (test rigs aim with it) */
static void log_enemy(void)
{
    int v[16][3], n = lockon_volumes(v, 16), i, best = -1;
    double bd = 1e30, cx = GVAR(int32_t, 0x77E9E8u), cy = GVAR(int32_t, 0x77E9ECu), cz = GVAR(int32_t, 0x77E9F0u);
    double yaw = (GVAR(uint16_t, 0x77EA0Au) & 0xfff) * (360.0 / 4096.0), pr = GVAR(uint16_t, 0x77EA08u) & 0xfff, ey, ep, r;
    for (i = 0; i < n; i++) {
        double dx = v[i][0] - cx, dz = v[i][2] - cz, d = dx * dx + dz * dz;
        if (d < bd)
            bd = d, best = i;
    }
    if (best < 0)
        return;
    ey = atan2(v[best][0] - cx, v[best][2] - cz) * (180.0 / PI) - yaw;
    while (ey > 180) ey -= 360;
    while (ey < -180) ey += 360;
    ep = atan2(cy - v[best][1], sqrt(bd)) * (180.0 / PI) - (pr > 2048 ? pr - 4096 : pr) * (360.0 / 4096.0);
    r = sqrt(bd);
    log_printf("homing: enemy %.0f from the camera, %.1f deg right, %.1f deg up of the view (view pitch %.1f, facing %d)", r, ey, ep,
               (pr > 2048 ? pr - 4096 : pr) * (360.0 / 4096.0), GVAR(uint16_t, G_PLAYER + 0x56) & 0xfff);
}

int homing_read(const char *ini)
{
    char v[16], aim[16], btn[16], cam[16];
    GetPrivateProfileStringA("controls", "shot_homing", "", v, sizeof v, ini);
    if (!_stricmp(v, "off"))
        return HOMING_OFF;
    if (!_stricmp(v, "low") || !_stricmp(v, "reduced"))
        return HOMING_LOW;
    if (!_stricmp(v, "high") || !_stricmp(v, "original"))
        return HOMING_HIGH;
    /* not set yet: as the combat style the aiming settings come closest to (Shooter: free aim; Adventure:
       shots to the crosshair or the lock-on camera over the shoulder; Classic: neither) */
    GetPrivateProfileStringA("controls", "aim_button", "lockon", btn, sizeof btn, ini);
    GetPrivateProfileStringA("controls", "aim", "character", aim, sizeof aim, ini);
    GetPrivateProfileStringA("controls", "lockon_camera", "classic", cam, sizeof cam, ini);
    if (!_stricmp(btn, "free"))
        return HOMING_OFF;
    if (!_stricmp(aim, "camera") || !_stricmp(cam, "shoulder"))
        return HOMING_LOW;
    return HOMING_HIGH;
}

void homing_set(int mode)
{
    g_mode = mode < HOMING_OFF ? HOMING_OFF : mode > HOMING_HIGH ? HOMING_HIGH : mode;
    log_printf("homing: crosshair-aimed buster shots home %s", g_mode == HOMING_HIGH ? "as in the original (high)"
               : g_mode == HOMING_OFF ? "never (off)" : "only onto an enemy near the crosshair (low)");
}

void homing_load(const char *ini)
{
    homing_set(homing_read(ini));
}
