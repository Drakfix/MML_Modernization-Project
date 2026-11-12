/* The English subtitles for the cutscenes, downloaded and converted where the start menu does it
 * (Linux, the Windows start menu): Subtitles.java (Android) and tools/voice/build_subtitles.py
 * --download (the Windows installer) in C, with the same output byte for byte. The Rockman DASH
 * PSP English project's script is downloaded at a fixed commit (subtitles_fetch: the platform's),
 * checked against its SHA-256, and converted into Modernization/subtitles_en.txt. */
#include <windows.h>   /* (native/compat elsewhere) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamefiles.h"
#include "log.h"
#include "subtitles_get.h"

#if (defined(__linux__) && !defined(__ANDROID__)) || (defined(_WIN32) && defined(MML_RUNNER))
#define SUB_REPO "https://github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher"
#define SUB_COMMIT "7d49ceb334aae2b2381bdde3d03448cbd90320da"
#define SUB_PATH "script/subtitles_en.csv"
#define SUB_URL "https://raw.githubusercontent.com/JamesAZ0/RockmanDASH-PSP-English-Patcher/" SUB_COMMIT "/" SUB_PATH
#define SUB_SHA256 "0822f5228b3248dd62a358611abe1a7a6182c6a15de9e210896f6b753579697c"
static const char *const SUB_CREDIT[] = {
    "English subtitles for the cutscenes: the work of the Rockman DASH PSP English project",
    "(JamesAZ0 and its contributors), " SUB_REPO ",",
    "from its " SUB_PATH " at commit 7d49ceb, downloaded from there during setup.",
    "That project transcribed the US voice acting and timed the lines to it, under the MIT licence:",
    "Copyright (c) 2026 The Rockman DASH PSP English project contributors.",
    "The lines belong to Capcom.",
};
static const int SUB_NEWS_CLIP[] = { 10, 4, 5, 7, 6 };   /* C scenes 0-4: the news reports (bank 1821E) */

typedef struct {
    char *p;
    size_t len, cap;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_str(sbuf *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

/* one CSV record (the script has no line breaks inside fields): up to max fields into f, each a
   copy (free them); the count */
static int csv_fields(const char *line, char **f, int max)
{
    int n = 0, quoted = 0;
    sbuf cur = { 0 };
    const char *s;
    sb_put(&cur, "", 0);
    for (s = line;; s++) {
        if (!*s || (!quoted && *s == ',')) {
            if (n < max)
                f[n++] = _strdup(cur.p);
            cur.len = 0;
            cur.p[0] = 0;
            if (!*s)
                break;
        } else if (quoted) {
            if (*s == '"' && s[1] == '"') {
                sb_put(&cur, "\"", 1);
                s++;
            } else if (*s == '"')
                quoted = 0;
            else
                sb_put(&cur, s, 1);
        } else if (*s == '"')
            quoted = 1;
        else
            sb_put(&cur, s, 1);
    }
    free(cur.p);
    return n;
}

/* the next UTF-8 character of s (advanced past it); invalid bytes count as one character each */
static unsigned utf8_next(const unsigned char **s)
{
    const unsigned char *p = *s;
    unsigned c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, i;
    if (c >= 0x80) {
        unsigned v = c & (0x3F >> more);
        for (i = 0; i < more && (*p & 0xC0) == 0x80; i++)
            v = v << 6 | (*p++ & 0x3F);
        c = i == more && more ? v : 0xFFFD;
    }
    *s = p;
    return c;
}

/* the text as the game's font can show it: typographic marks as ASCII, the rest '?', words one
   space apart (Subtitles.asciiText) */
static void ascii_text(const char *in, sbuf *out)
{
    sbuf a = { 0 };
    const unsigned char *s = (const unsigned char *)in;
    const char *p;
    int word = 0;
    sb_put(&a, "", 0);
    while (*s) {
        unsigned c = utf8_next(&s);
        char ch[2] = { 0, 0 };
        switch (c) {
        case 0x2018: case 0x2019: sb_str(&a, "'"); break;
        case 0x201C: case 0x201D: sb_str(&a, "\""); break;
        case 0x2026: sb_str(&a, "..."); break;
        case 0x2013: sb_str(&a, "-"); break;
        case 0x2014: sb_str(&a, "--"); break;
        case 0x00A0: sb_str(&a, " "); break;
        case 0x00E9: case 0x00E8: sb_str(&a, "e"); break;
        case 0x00E0: sb_str(&a, "a"); break;
        case 0x2122: break;
        default:
            ch[0] = c < 127 ? (char)c : '?';
            if (c >= 0x10000)
                sb_str(&a, "?");   /* (Java counts it as two characters) */
            sb_str(&a, ch);
        }
    }
    for (p = a.p; *p; p++) {   /* split on whitespace, joined by single spaces */
        int space = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f';
        if (space) {
            word = 0;
            continue;
        }
        if (!word && out->len)
            sb_str(out, " ");
        word = 1;
        sb_put(out, (*p >= 32 && *p < 127) ? p : "?", 1);
    }
    free(a.p);
}

static int column(char **head, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(head[i], name) == 0)
            return i;
    return -1;
}

/* the subtitle file from the script (Subtitles.convert); 0 if the script isn't the expected kind */
static int convert(char *csv, sbuf *out)
{
    enum { MAXF = 64 };
    char *head[MAXF], *r[MAXF], *line, *next;
    int nh, sc, st, en, tx, fi, stt, ov, i, ok = 1;
    size_t k;
    for (k = 0; k < sizeof SUB_CREDIT / sizeof SUB_CREDIT[0]; k++) {
        sb_str(out, "# ");
        sb_str(out, SUB_CREDIT[k]);
        sb_str(out, "\n");
    }
    sb_str(out, "# bank clip start end text: built by tools/voice/build_subtitles.py\n");
    next = strchr(csv, '\n');
    if (next)
        *next++ = 0;
    if (strlen(csv) && csv[strlen(csv) - 1] == '\r')
        csv[strlen(csv) - 1] = 0;
    nh = csv_fields(csv, head, MAXF);
    sc = column(head, nh, "scene");
    st = column(head, nh, "start_s");
    en = column(head, nh, "end_s");
    tx = column(head, nh, "text");
    fi = column(head, nh, "en_final");
    stt = column(head, nh, "status");
    ov = column(head, nh, "time_override");
    if (sc < 0 || st < 0 || en < 0 || tx < 0 || fi < 0 || stt < 0 || ov < 0)
        ok = 0;
    for (line = next; ok && line && *line; line = next) {
        char bank[8], text_ok;
        int nr, num, clip;
        double s, e;
        sbuf t = { 0 };
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (*line && line[strlen(line) - 1] == '\r')
            line[strlen(line) - 1] = 0;
        if (!*line)
            continue;
        nr = csv_fields(line, r, MAXF);
        for (i = nr; i < nh; i++)
            r[i] = _strdup("");
        if (nr < nh)
            nr = nh;
        {
            char *a = r[stt], *b;
            while (*a == ' ' || *a == '\t')
                a++;
            b = a + strlen(a);
            while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r'))
                b--;
            if ((size_t)(b - a) == 4 && strncmp(a, "drop", 4) == 0)
                goto next_line;
        }
        {
            char *scene = r[sc], *endp;
            while (*scene == ' ')
                scene++;
            num = (int)strtol(scene + 1, &endp, 10);
            if (!scene[0] || endp == scene + 1) {
                ok = 0;   /* (Java: a NumberFormatException, the download fails) */
                goto next_line;
            }
            if (scene[0] == 'A') {
                strcpy_s(bank, sizeof bank, "3700E");
                clip = num;
            } else if (scene[0] == 'B') {
                strcpy_s(bank, sizeof bank, "3701E");
                clip = num + 2;
            } else if (scene[0] == 'C' && num >= 0 && num < 5) {
                strcpy_s(bank, sizeof bank, "1821E");
                clip = SUB_NEWS_CLIP[num];
            } else
                goto next_line;
        }
        s = strtod(r[st], NULL);
        e = strtod(r[en], NULL);
        {
            char *o = r[ov];
            while (*o == ' ')
                o++;
            if (*o) {
                char *dash = strchr(o, '-');
                s = strtod(o, NULL);
                e = dash ? strtod(dash + 1, NULL) : e;
            }
        }
        ascii_text(r[fi][0] ? r[fi] : r[tx], &t);
        text_ok = t.p && t.p[0];
        if (text_ok) {
            char num_s[96];
            snprintf(num_s, sizeof num_s, "%s %d %.2f %.2f ", bank, clip, s, e);   /* (as Python's %.2f) */
            sb_str(out, num_s);
            sb_str(out, t.p);
            sb_str(out, "\n");
        }
        free(t.p);
    next_line:
        for (i = 0; i < nr; i++)
            free(r[i]);
    }
    for (i = 0; i < nh; i++)
        free(head[i]);
    return ok;
}

int subtitles_download(const char *dir, char *msg, size_t n)
{
    char tmp[MAX_PATH + 32], dst[MAX_PATH + 32], hex[65];
    FILE *f = NULL;
    char *data = NULL;
    long len = 0;
    sbuf out = { 0 };
    snprintf(tmp, sizeof tmp, "%s/subtitles_en.csv.part", dir);
    if (!subtitles_fetch(SUB_URL, tmp)) {
        remove(tmp);
        snprintf(msg, n, "%s", "The subtitles couldn't be downloaded: no connection"
#ifndef _WIN32
                 ", or no curl or wget on this system"
#endif
                 );
        return 0;
    }
    if (fopen_s(&f, tmp, "rb") != 0)
        f = NULL;
    if (f && fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = (char *)malloc((size_t)len + 1);
        if (data && fread(data, 1, (size_t)len, f) == (size_t)len)
            data[len] = 0;
        else {
            free(data);
            data = NULL;
        }
    }
    if (f)
        fclose(f);
    remove(tmp);
    if (data)
        gamefiles_sha256(data, (size_t)len, hex);
    if (!data || strcmp(hex, SUB_SHA256) != 0) {
        snprintf(msg, n, "The subtitles couldn't be downloaded: the file from %s is not the expected one, so "
                 "nothing was written (the repository may have changed)", SUB_REPO);
        free(data);
        return 0;
    }
    if (!convert(data, &out)) {
        snprintf(msg, n, "%s", "The subtitles couldn't be downloaded: the script couldn't be read");
        free(data);
        free(out.p);
        return 0;
    }
    free(data);
    snprintf(tmp, sizeof tmp, "%s/subtitles_en.txt.part", dir);
    snprintf(dst, sizeof dst, "%s/subtitles_en.txt", dir);
    if (fopen_s(&f, tmp, "wb") != 0)
        f = NULL;
    if (!f || fwrite(out.p, 1, out.len, f) != out.len || fclose(f) != 0 || (remove(dst), rename(tmp, dst)) != 0) {
        if (f)
            remove(tmp);
        snprintf(msg, n, "The subtitles couldn't be downloaded: couldn't write %s", dst);
        free(out.p);
        return 0;
    }
    free(out.p);
    log_printf("subtitles: written to %s", dst);
    snprintf(msg, n, "%s", "Subtitles downloaded");
    return 1;
}
#else
typedef int subtitles_get_unused;   /* (the Windows installer and Android download them themselves) */
#endif
