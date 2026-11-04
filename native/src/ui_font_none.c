/* ui_font.h where there is no font engine of ours yet (Linux and Apple come later). Without
 * glyphs the UI can't draw (gfx_ui.c gives up quietly). */
#if !defined(_WIN32) && !defined(__ANDROID__)
#include "log.h"
#include "ui_font.h"

int ui_font_rasterize(int px, uint8_t *cov, int aw, int ah, ui_glyph glyph[UI_FONTS][UI_LAST_CHAR + 1],
                      float line[UI_FONTS])
{
    (void)px; (void)cov; (void)aw; (void)ah; (void)glyph; (void)line;
    log_printf("ui: no font engine on this platform yet, menu text off");
    return 0;
}
#else
typedef int ui_font_none_unused;   /* Windows: ui_font_gdi.c; Android: ui_font_android.c */
#endif
