/* Subtitles for the cutscenes ([general] subtitles=1), with the English voices or the Japanese ones
 * (voices=japanese): English lines from Modernization\subtitles_en.txt, which Setup makes from the
 * Rockman DASH PSP English project's script (tools/voice/build_subtitles.py), one per line:
 *     <US voice bank> <clip> <start seconds> <end seconds> <text>
 * When the game plays a clip that has lines (voice.c tells us), they show at their times from the
 * clip's start. They stop when the lines run out, when another voice starts, or when the voice is
 * released (a skipped cutscene): the voice object at 0x55D5C0 holds its sound buffer at +8 while
 * it exists.
 *
 * The lines are transcribed from the English voice acting and timed to it. The cutscenes run on
 * the animation, so the Japanese voices speak at about the same moments, but often say in a moment
 * what takes longer to read in English. At load (tidy): a sentence split over two lines that
 * follow each other becomes one line where it fits; a line stays up until it has had its reading
 * time (READ_CPS characters a second, at least READ_MIN), as far as the next line allows; and past
 * that, it stays above the next line (dimmed) until it has had it, so the next one still starts
 * with its voice. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "gfx_ui.h"
#include "guest.h"
#include "log.h"
#include "menu.h"
#include "plat.h"
#include "subtitles.h"
#include "ui_style.h"

#define G_VOICE_BUFFER 0x55D5C8u   /* the voice object's sound buffer (0 when released) */
#define FADE_MS 150
#define READ_CPS 15.0f    /* reading speed, characters a second */
#define READ_MIN 1.2f     /* the shortest a line is up, seconds */
#define READ_LEAD 0.3f    /* time to notice a line has come */
#define JOIN_GAP 0.4f     /* a sentence's two halves this close together become one line... */
#define JOIN_MAX 110      /* ...if the whole fits in about two rows */

typedef struct {
    char bank[8];
    unsigned clip;
    float start, end;
    float hold;   /* until when it stays (above the next line, past end) to be read */
    char *text;
} sub_line;

static sub_line *g_lines;
static int g_nlines;
static int g_first = -1, g_count;   /* the playing clip's lines (g_lines is grouped by clip) */
static uint32_t g_start_ms;
static int g_buffer_seen;

static int load(const char *path)
{
    FILE *f;
    char buf[1024];
    int cap = 0;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return 0;
    while (fgets(buf, sizeof buf, f)) {
        sub_line l;
        int used = 0;
        char *e = buf + strlen(buf);
        while (e > buf && (e[-1] == '\n' || e[-1] == '\r'))
            *--e = 0;
        if (buf[0] == '#' || !buf[0])
            continue;
        memset(&l, 0, sizeof l);
        {   /* the bank name, then numbers only (sscanf_s is plain sscanf on other systems) */
            size_t k = strcspn(buf, " ");
            if (k == 0 || k >= sizeof l.bank || !buf[k])
                continue;
            memcpy(l.bank, buf, k);
            if (sscanf_s(buf + k, "%u %f %f %n", &l.clip, &l.start, &l.end, &used) < 3 || !used)
                continue;
            used += (int)k;
        }
        if (g_nlines == cap) {
            sub_line *n = (sub_line *)realloc(g_lines, sizeof *g_lines * (size_t)(cap = cap ? cap * 2 : 256));
            if (!n)
                break;
            g_lines = n;
        }
        l.text = _strdup(buf + used);
        if (!l.text)
            break;
        g_lines[g_nlines++] = l;
    }
    fclose(f);
    return g_nlines;
}

static int same_clip(const sub_line *a, const sub_line *b)
{
    return a->clip == b->clip && _stricmp(a->bank, b->bank) == 0;
}

static int sentence_ends(const char *t)
{
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '"' || t[n - 1] == '\'' || t[n - 1] == ')'))
        n--;
    return n && strchr(".!?~-", t[n - 1]) != NULL;   /* (and "--", "...") */
}

static float read_time(const char *t)
{
    float r = (float)strlen(t) / READ_CPS + READ_LEAD;
    return r > READ_MIN ? r : READ_MIN;
}

/* see the top: joined halves, reading time (g_lines is grouped by clip, in time order) */
static void tidy(void)
{
    int i, joined = 0, held = 0, longer = 0;
    for (i = 0; i + 1 < g_nlines; i++) {
        sub_line *a = &g_lines[i], *b = &g_lines[i + 1];
        size_t na = strlen(a->text), nb = strlen(b->text);
        char *t;
        if (!same_clip(a, b) || sentence_ends(a->text) || b->start - a->end > JOIN_GAP ||
            na + 1 + nb > JOIN_MAX || !(t = (char *)malloc(na + nb + 2)))
            continue;
        memcpy(t, a->text, na);
        t[na] = ' ';
        memcpy(t + na + 1, b->text, nb + 1);
        free(a->text);
        free(b->text);
        a->text = t;
        a->end = b->end;
        memmove(b, b + 1, sizeof *b * (size_t)(g_nlines - i - 2));
        g_nlines--;
        joined++;
        i--;   /* (it may join the next one too) */
    }
    for (i = 0; i < g_nlines; i++) {
        sub_line *l = &g_lines[i];
        float want = l->start + read_time(l->text), next = 1e9f;
        if (i + 1 < g_nlines && same_clip(l, &g_lines[i + 1]))
            next = g_lines[i + 1].start - 0.05f;
        if (l->end < want && l->end < next) {
            l->end = want < next ? want : next;
            longer++;
        }
        l->hold = want > l->end ? want : l->end;
        held += l->hold > l->end;
    }
    log_printf("subtitles: %d split sentences joined, %d lines up longer, %d kept above the next to be read",
               joined, longer, held);
}

void subtitles_voice(const char *bank, unsigned clip)
{
    int i;
    size_t n = strcspn(bank, ".");
    g_first = -1;
    g_count = 0;
    for (i = 0; i < g_nlines; i++)
        if (g_lines[i].clip == clip && strlen(g_lines[i].bank) == n && _strnicmp(g_lines[i].bank, bank, n) == 0) {
            if (g_first < 0)
                g_first = i;
            g_count = i - g_first + 1;
        } else if (g_first >= 0) {
            break;
        }
    g_start_ms = plat_ticks_ms();
    g_buffer_seen = 0;
}

/* words of s into lines of at most max_w pixels; returns how many (each a start and a length) */
static int wrap(const char *s, float max_w, float scale, int starts[], int lens[], int max_lines)
{
    char tmp[512];
    int n = 0, pos = 0, len = (int)strlen(s);
    while (pos < len && n < max_lines) {
        int end = pos, best = -1;
        while (s[pos] == ' ')
            pos++;
        for (end = pos; end <= len; end++) {
            if (end == len || s[end] == ' ') {
                int k = end - pos < (int)sizeof tmp - 1 ? end - pos : (int)sizeof tmp - 1;
                memcpy(tmp, s + pos, (size_t)k);
                tmp[k] = 0;
                if (gfx_ui_text_width(tmp, scale) > max_w && best >= 0)
                    break;
                best = end;
            }
        }
        if (best < 0)
            best = len;
        starts[n] = pos;
        lens[n] = best - pos;
        n++;
        pos = best;
    }
    return n;
}

static void draw(void *user, int w, int h)
{
    float t, aw = (float)display_native_width(), ah = 480.0f, s, ox, pw, ph, oy, scale, lh, y;
    int i, k, starts[4], lens[4], nl;
    char line[512];
    (void)user;
    if (g_first < 0)
        return;
    if (GVAR(uint32_t, G_VOICE_BUFFER))
        g_buffer_seen = 1;
    else if (g_buffer_seen) {   /* released: the cutscene was skipped or the voice stopped */
        g_first = -1;
        return;
    }
    t = (float)(plat_ticks_ms() - g_start_ms) / 1000.0f;
    if (t > g_lines[g_first + g_count - 1].hold + 1.0f) {
        g_first = -1;
        return;
    }
    if (menu_blocks_input())
        return;
    s = (float)w / aw < (float)h / ah ? (float)w / aw : (float)h / ah;   /* the picture in the window */
    pw = aw * s;
    ph = ah * s;
    ox = ((float)w - pw) / 2;
    oy = ((float)h - ph) / 2;
    scale = 1.05f;
    lh = gfx_ui_line(scale) * 1.15f;   /* a little air between wrapped lines */
    {   /* the line of the moment, and above it the one before while it still is to be read */
        int cur = -1, prev = -1, n = 0, which[2];
        float bottom = oy + ph * 0.93f;
        for (i = g_first; i < g_first + g_count; i++)
            if (t >= g_lines[i].start && t < g_lines[i].end)
                cur = i;
        for (i = g_first; i < g_first + g_count; i++)
            if (i != cur && t >= g_lines[i].start && t < g_lines[i].hold && (cur < 0 || i < cur))
                prev = i;
        if (prev >= 0)
            which[n++] = prev;
        if (cur >= 0)
            which[n++] = cur;
        for (k = n - 1; k >= 0; k--) {   /* from the bottom up */
            const sub_line *l = &g_lines[which[k]];
            float in = (t - l->start) * 1000.0f / FADE_MS, out = ((which[k] == cur ? l->end : l->hold) - t) * 1000.0f / FADE_MS;
            int r;
            ui_alpha = in < out ? in : out;
            if (ui_alpha > 1.0f)
                ui_alpha = 1.0f;
            if (which[k] != cur && cur >= 0)
                ui_alpha *= 0.7f;   /* (the earlier line, dimmed) */
            nl = wrap(l->text, pw * 0.8f, scale, starts, lens, 4);
            y = bottom - nl * lh;
            for (r = 0; r < nl; r++) {
                float lw;
                memcpy(line, l->text + starts[r], (size_t)lens[r]);
                line[lens[r]] = 0;
                lw = gfx_ui_text_width(line, scale);
                ui_text_outlined(UI_FONT_BODY, ox + (pw - lw) / 2, y + r * lh, line, 0xFFFFFFFF, 0xFFE6F0FA, scale,
                                 lh * 0.07f, 0xFF000000);
            }
            bottom = y - lh * 0.25f;
        }
    }
    ui_alpha = 1;
}

int subtitles_install(const char *game_dir, int on)
{
    char path[MAX_PATH];
    if (!on)
        return 0;
    snprintf(path, sizeof path, "%s\\Modernization\\subtitles_en.txt", game_dir);
    if (!load(path)) {
        log_printf("subtitles: no %s (Setup downloads it; or tools/voice/build_subtitles.py)", path);
        return 0;
    }
    log_printf("subtitles: %d lines from %s", g_nlines, path);
    tidy();
    gfx_ui_add_draw(draw, NULL);
    return 1;
}
