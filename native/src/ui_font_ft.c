/* ui_font.h on Linux: the system's fonts, found by fontconfig and drawn by FreeType (both part of
 * every desktop Linux; linked from the system, not shipped). Laid out as ui_font_gdi.c and Android's
 * MmlActivity.rasterizeFont do: each font starts a new row, a glyph's box has room for an italic
 * overhang and for filtering. A font without the asked-for weight or slant is emboldened or slanted
 * here, as GDI and Android do themselves. */
#if defined(__linux__) && !defined(__ANDROID__)
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include "log.h"
#include "ui_font.h"

/* the face fontconfig picks for pattern: its file and index into path/index; 0 if none */
static int find_font(const char *pattern, char *path, size_t n, int *index, int *weight, int *slant)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern), *match;
    FcResult res;
    FcChar8 *file = NULL;
    int ok = 0;
    if (!pat)
        return 0;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    match = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    if (!match)
        return 0;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        snprintf(path, n, "%s", (const char *)file);
        if (FcPatternGetInteger(match, FC_INDEX, 0, index) != FcResultMatch)
            *index = 0;
        if (FcPatternGetInteger(match, FC_WEIGHT, 0, weight) != FcResultMatch)
            *weight = FC_WEIGHT_REGULAR;
        if (FcPatternGetInteger(match, FC_SLANT, 0, slant) != FcResultMatch)
            *slant = FC_SLANT_ROMAN;
        ok = 1;
    }
    FcPatternDestroy(match);
    return ok;
}

/* how wide the face sets a sample line at its current size, in pixels (as drawn: emboldened or not) */
#define WIDTH_SAMPLE "The quick brown fox jumps over the lazy dog. 0123456789 MEGA MAN LEGENDS"
static float sample_width(FT_Face face, int embolden)
{
    const char *s;
    float w = 0;
    for (s = WIDTH_SAMPLE; *s; s++)
        if (!FT_Load_Char(face, (FT_ULong)(unsigned char)*s, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT)) {
            if (embolden)
                FT_GlyphSlot_Embolden(face->glyph);
            w += face->glyph->advance.x / 64.0f;
        }
    return w;
}

int ui_font_rasterize(int px, uint8_t *cov, int aw, int ah, ui_glyph glyph[UI_FONTS][UI_LAST_CHAR + 1],
                      float line[UI_FONTS])
{
    /* as on Windows (Segoe UI semibold; Bahnschrift bold italic) and Android (sans-serif medium;
       sans-serif condensed bold italic) */
    /* em: how wide Windows' face sets WIDTH_SAMPLE, in ems (Segoe UI Semibold 36.58, Bahnschrift Bold
       35.62): the menus are laid out for those, so a wider system font is set smaller to take the
       same room (DejaVu Sans Bold, common on Linux, is about a fifth wider) */
    static const struct { const char *pattern; int weight, italic; float size, em; } F[UI_FONTS] = {
        { "sans-serif:semibold", FC_WEIGHT_SEMIBOLD, 0, 1.0f, 36.58f },
        { "sans-serif:condensed:bold:italic", FC_WEIGHT_BOLD, 1, 1.5f, 35.62f },
    };
    FT_Library ft;
    int x = 8, y = 8, row_h = 0, f, c, drawn = 0;
    memset(cov, 0, (size_t)aw * ah);
    if (!FcInit()) {
        log_printf("ui: fontconfig failed, menu text off");
        return 0;
    }
    if (FT_Init_FreeType(&ft)) {
        log_printf("ui: FreeType failed, menu text off");
        return 0;
    }
    for (f = 0; f < UI_FONTS; f++) {
        char path[1024];
        int index = 0, weight = 0, slant = 0, size = (int)(px * F[f].size + 0.5f), asc, lh;
        int embolden, oblique;
        FT_Face face;
        if (!find_font(F[f].pattern, path, sizeof path, &index, &weight, &slant) ||
            FT_New_Face(ft, path, index, &face)) {
            log_printf("ui: no font for %s", F[f].pattern);
            continue;
        }
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
        embolden = weight < F[f].weight - 20;   /* (well below what was asked: a regular face) */
        oblique = F[f].italic && slant == FC_SLANT_ROMAN;
        {   /* as wide as Windows' face (see F) */
            float want = F[f].em * size, have = sample_width(face, embolden);
            if (have > want * 1.02f) {
                size = (int)(size * want / have + 0.5f);
                FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
            }
        }
        log_printf("ui: font %d: %s (%dpx%s%s)", f, path, size, embolden ? ", emboldened" : "",
                   oblique ? ", slanted" : "");
        asc = (int)((face->size->metrics.ascender + 63) >> 6);
        lh = asc + (int)((-face->size->metrics.descender + 63) >> 6);
        line[f] = (float)lh;
        if (x > 8) {   /* each font starts a new row */
            x = 8;
            y += row_h + 4;
        }
        row_h = lh;
        for (c = UI_FIRST_CHAR; c <= UI_LAST_CHAR; c++) {
            FT_GlyphSlot g;
            FT_Bitmap *b;
            float adv;
            int w, lead = lh / 8, gx, gy, r, k;
            if (FT_Load_Char(face, (FT_ULong)c, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT))
                continue;
            g = face->glyph;
            if (embolden)
                FT_GlyphSlot_Embolden(g);
            if (oblique)
                FT_GlyphSlot_Oblique(g);
            if (FT_Render_Glyph(g, FT_RENDER_MODE_LIGHT))
                continue;
            adv = g->advance.x / 64.0f;
            w = (int)ceilf(adv) + 2 * lead + 4;   /* room for italic/overhang and filtering */
            if (x + w > aw) {
                x = 8;
                y += row_h + 4;
            }
            if (y + lh > ah)
                break;
            b = &g->bitmap;
            gx = x + lead + g->bitmap_left;   /* the pen at (x + lead, y + ascent), as TextOut puts it */
            gy = y + asc - g->bitmap_top;
            for (r = 0; r < (int)b->rows; r++) {
                int ty = gy + r;
                if (ty < y || ty >= y + lh || ty >= ah)
                    continue;
                for (k = 0; k < (int)b->width; k++) {
                    int tx = gx + k;
                    uint8_t v = b->buffer[r * b->pitch + k];
                    if (tx < x || tx >= x + w || tx >= aw)
                        continue;
                    if (v > cov[ty * aw + tx])
                        cov[ty * aw + tx] = v;
                }
            }
            glyph[f][c].u0 = (float)x / aw;
            glyph[f][c].v0 = (float)y / ah;
            glyph[f][c].u1 = (float)(x + w) / aw;
            glyph[f][c].v1 = (float)(y + lh) / ah;
            glyph[f][c].w = (float)w;
            glyph[f][c].h = (float)lh;
            glyph[f][c].adv = adv;
            glyph[f][c].lead = (float)lead;
            x += w + 2;
            drawn++;
        }
        FT_Done_Face(face);
    }
    FT_Done_FreeType(ft);
    return drawn > 0;
}
#else
typedef int ui_font_ft_unused;   /* Windows: ui_font_gdi.c; Android: ui_font_android.c */
#endif
