/* ui_font.h with GDI: each font's ASCII drawn into a DIB, its coverage kept. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ui_font.h"

int ui_font_rasterize(int px, uint8_t *cov, int aw, int ah, ui_glyph glyph[UI_FONTS][UI_LAST_CHAR + 1],
                      float line[UI_FONTS])
{
    static const struct { const char *face, *fallback; int weight, italic; float size; } F[UI_FONTS] = {
        { "Segoe UI", "Arial", FW_SEMIBOLD, 0, 1.0f },
        { "Bahnschrift", "Arial Black", FW_BOLD, 1, 1.5f },
    };
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    HGDIOBJ old_bmp;
    int x = 8, y = 8, row_h = 0, c, f;
    if (!dc)
        return 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = aw;
    bi.bmiHeader.biHeight = -ah;   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) {
        DeleteDC(dc);
        return 0;
    }
    old_bmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    memset(bits, 0, (size_t)aw * ah * 4);
    for (f = 0; f < UI_FONTS; f++) {
        int size = (int)(px * F[f].size + 0.5f);
        HFONT font = CreateFontA(-size, 0, 0, 0, F[f].weight, (DWORD)F[f].italic, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, F[f].face);
        HFONT old_font;
        TEXTMETRICA tm;
        char face[64];
        if (font) {   /* GDI quietly substitutes a missing face: check, then use the fallback */
            old_font = (HFONT)SelectObject(dc, font);
            GetTextFaceA(dc, sizeof face, face);
            SelectObject(dc, old_font);
            if (_stricmp(face, F[f].face) != 0) {
                DeleteObject(font);
                font = CreateFontA(-size, 0, 0, 0, F[f].weight, (DWORD)F[f].italic, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                                   F[f].fallback);
            }
        }
        if (!font)
            continue;
        old_font = (HFONT)SelectObject(dc, font);
        GetTextMetricsA(dc, &tm);
        line[f] = (float)tm.tmHeight;
        if (x > 8) {   /* each font starts a new row */
            x = 8;
            y += row_h + 4;
        }
        row_h = tm.tmHeight;
        for (c = UI_FIRST_CHAR; c <= UI_LAST_CHAR; c++) {
            char ch = (char)c;
            SIZE sz;
            int w, lead = tm.tmHeight / 8;
            GetTextExtentPoint32A(dc, &ch, 1, &sz);
            w = sz.cx + 2 * lead + 4;   /* room for italic/overhang and filtering */
            if (x + w > aw) {
                x = 8;
                y += row_h + 4;
            }
            if (y + tm.tmHeight > ah)
                break;
            TextOutA(dc, x + lead, y, &ch, 1);
            glyph[f][c].u0 = (float)x / aw;
            glyph[f][c].v0 = (float)y / ah;
            glyph[f][c].u1 = (float)(x + w) / aw;
            glyph[f][c].v1 = (float)(y + tm.tmHeight) / ah;
            glyph[f][c].w = (float)w;
            glyph[f][c].h = (float)tm.tmHeight;
            glyph[f][c].adv = (float)sz.cx;
            glyph[f][c].lead = (float)lead;
            x += w + 2;
        }
        SelectObject(dc, old_font);
        DeleteObject(font);
    }
    GdiFlush();
    for (c = 0; c < aw * ah; c++)   /* coverage (any channel) */
        cov[c] = (uint8_t)(((uint32_t *)bits)[c] & 0xFF);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return 1;
}
