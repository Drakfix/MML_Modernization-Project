package org.mml.legends;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;

import java.nio.ByteBuffer;

import org.libsdl.app.SDLActivity;

/* SDL's activity, running libmain.so (our native code) after libSDL3.so */
public class MmlActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    static final int FONTS = 2, FIRST = 32, LAST = 126;

    /* The UI's text for native code (ui_font_android.c), laid out as ui_font_gdi.c does on
       Windows: both fonts' ASCII drawn white into an aw x ah coverage atlas (cov, aw bytes per
       row, native memory), each font from a new row. Returns per font: the line height, then
       for each character x, y, w, h, advance, lead (pixels); null if it failed. Font 0 is the
       body text (medium weight), font 1 the display font (condensed bold italic, 1.5x). */
    public static float[] rasterizeFont(int px, int aw, int ah, ByteBuffer cov) {
        final Typeface[] faces = {
            Typeface.create("sans-serif-medium", Typeface.NORMAL),
            Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC),
        };
        final float[] scale = { 1.0f, 1.5f };
        final int per = 1 + (LAST - FIRST + 1) * 6;
        float[] out = new float[FONTS * per];
        Bitmap bmp = Bitmap.createBitmap(aw, ah, Bitmap.Config.ALPHA_8);
        Canvas canvas = new Canvas(bmp);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xFFFFFFFF);
        int x = 8, y = 8, rowH = 0;
        for (int f = 0; f < FONTS; f++) {
            paint.setTypeface(faces[f]);
            paint.setTextSize(Math.round(px * scale[f]));
            Paint.FontMetrics fm = paint.getFontMetrics();
            int lineH = (int) Math.ceil(fm.descent - fm.ascent);
            out[f * per] = lineH;
            if (x > 8) {
                x = 8;
                y += rowH + 4;
            }
            rowH = lineH;
            for (int c = FIRST; c <= LAST; c++) {
                String ch = String.valueOf((char) c);
                float adv = paint.measureText(ch);
                int lead = lineH / 8;
                int w = (int) Math.ceil(adv) + 2 * lead + 4;   /* room for italic overhang and filtering */
                if (x + w > aw) {
                    x = 8;
                    y += rowH + 4;
                }
                if (y + lineH > ah)
                    break;
                canvas.drawText(ch, x + lead, y - fm.ascent, paint);
                int o = f * per + 1 + (c - FIRST) * 6;
                out[o] = x;
                out[o + 1] = y;
                out[o + 2] = w;
                out[o + 3] = lineH;
                out[o + 4] = adv;
                out[o + 5] = lead;
                x += w + 2;
            }
        }
        if (bmp.getRowBytes() != aw)
            return null;   /* (rows padded: an atlas width that is a multiple of 4 has none) */
        cov.rewind();
        bmp.copyPixelsToBuffer(cov);
        bmp.recycle();
        return out;
    }
}
