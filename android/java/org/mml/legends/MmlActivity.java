package org.mml.legends;

import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.StatFs;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

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

    /* ---- the game's files: a copy of the player's own game folder (android_files.c) ----
     * The player picks the folder with the system's folder picker (only that folder is shared
     * with the app, no storage permission); what the game uses is copied into the app's own
     * storage on a thread of its own while native code polls importStatus. Files already there
     * with the same size are kept, so an interrupted import carries on where it stopped. The US
     * version (MEGAMAN.EXE) goes into the game folder itself, the Japanese one (ROCKMAN.EXE) into
     * a folder of its own in it (gamefiles.h), so both can be there. */
    static final int IMPORT_IDLE = 0, IMPORT_PICKING = 1, IMPORT_COPYING = 2, IMPORT_DONE = 3,
                     IMPORT_FAILED = 4, IMPORT_CANCELLED = 5;
    static final int REQ_PICK = 0x4D4D;
    static final String[] DIRS = { "DAT", "BGM", "SPH", "MODERNIZATION" };   /* (compared in capitals) */
    static volatile int sState = IMPORT_IDLE;
    static volatile long sDone, sTotal;
    static volatile String sMessage = "";
    static String sDest;
    static boolean sJapanese;

    public static void importGame(String dest, boolean japanese) {
        sDest = dest;
        sJapanese = japanese;
        sDone = sTotal = 0;
        sMessage = japanese ? "Pick your Rockman DASH folder (with ROCKMAN.EXE)"
                            : "Pick your Mega Man Legends folder (with MEGAMAN.EXE)";
        sState = IMPORT_PICKING;
        mSingleton.runOnUiThread(new Runnable() {
            public void run() {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                /* open in Download, where a copy from a PC usually lands */
                i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, DocumentsContract.buildDocumentUri(
                        "com.android.externalstorage.documents", "primary:Download"));
                try {
                    mSingleton.startActivityForResult(i, REQ_PICK);
                } catch (Exception e) {
                    sMessage = "This device has no folder picker";
                    sState = IMPORT_FAILED;
                }
            }
        });
    }

    /* ---- the English subtitles for the cutscenes (Subtitles.java), on a thread of its own;
     * android_files.c polls subtitlesState: 1 running, 2 done, 3 failed ---- */
    static volatile int sSubState;
    static volatile String sSubMessage = "";

    public static void downloadSubtitles(final String dir) {
        sSubState = 1;
        sSubMessage = "Downloading the subtitles from the Rockman DASH PSP English project";
        new Thread(new Runnable() {
            public void run() {
                try {
                    Subtitles.install(new File(dir));
                    sSubMessage = "Subtitles downloaded";
                    sSubState = 2;
                } catch (Exception e) {
                    sSubMessage = "The subtitles couldn't be downloaded: " + e.getMessage();
                    sSubState = 3;
                }
            }
        }, "mml-subtitles").start();
    }

    public static int subtitlesState() {
        return sSubState;
    }

    public static String subtitlesMessage() {
        return sSubMessage;
    }

    public static long[] importStatus() {
        return new long[] { sState, sDone, sTotal };
    }

    public static String importMessage() {
        return sMessage;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_PICK) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            sMessage = "No folder picked";
            sState = IMPORT_CANCELLED;
            return;
        }
        final Uri tree = data.getData();
        new Thread(new Runnable() {
            public void run() {
                copyTree(tree);
            }
        }, "mml-import").start();
    }

    static final class Entry {
        String id, rel;
        long size;
        Entry(String id, String rel, long size) {
            this.id = id;
            this.rel = rel;
            this.size = size;
        }
    }

    /* the children of a folder in the picked tree: {id, name, mime, size} each */
    static List<String[]> children(ContentResolver cr, Uri tree, String parentId) {
        List<String[]> out = new ArrayList<>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                          DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE };
        try (Cursor c = cr.query(u, cols, null, null, null)) {
            while (c != null && c.moveToNext())
                out.add(new String[] { c.getString(0), c.getString(1), c.getString(2), c.isNull(3) ? "0" : c.getString(3) });
        }
        return out;
    }

    static void walk(ContentResolver cr, Uri tree, String id, String rel, List<Entry> out) {
        for (String[] ch : children(cr, tree, id)) {
            String r = rel + "/" + ch[1];
            if (DocumentsContract.Document.MIME_TYPE_DIR.equals(ch[2]))
                walk(cr, tree, ch[0], r, out);
            else
                out.add(new Entry(ch[0], r, Long.parseLong(ch[3])));
        }
    }

    static void copyTree(Uri tree) {
        try {
            ContentResolver cr = mSingleton.getContentResolver();
            List<Entry> files = new ArrayList<>();
            boolean dat = false, us = false, jp = false;
            String exeName = sJapanese ? "ROCKMAN.EXE" : "MEGAMAN.EXE", cfgName = sJapanese ? "ROCKMAN.CFG" : "MEGAMAN.CFG";
            sMessage = "Looking through the folder";
            for (String[] ch : children(cr, tree, DocumentsContract.getTreeDocumentId(tree))) {
                String up = ch[1].toUpperCase(Locale.ROOT);
                boolean dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(ch[2]);
                if (dir) {
                    for (String d : DIRS)
                        if (d.equals(up) && !(sJapanese && d.equals("MODERNIZATION"))) {   /* (mods: shared) */
                            walk(cr, tree, ch[0], ch[1], files);
                            dat |= up.equals("DAT");
                        }
                } else {
                    us |= up.equals("MEGAMAN.EXE");
                    jp |= up.equals("ROCKMAN.EXE");
                    if (up.equals(exeName) || up.equals(cfgName) || up.equals("CAPCOM.AVI") ||
                        up.endsWith(".MCD") || up.endsWith(".MCD.QUICKSAVE"))   /* (CAPCOM.AVI: the game checks it's there) */
                        files.add(new Entry(ch[0], ch[1], Long.parseLong(ch[3])));
                }
            }
            boolean exe = sJapanese ? jp : us;
            if (!exe || !dat) {
                if (!exe && (sJapanese ? us : jp))
                    sMessage = sJapanese ? "That's the US version (MEGAMAN.EXE): import it as the US version"
                                         : "That's the Japanese version (ROCKMAN.EXE): import it as the Japanese version";
                else
                    sMessage = "That folder has no " + (!exe ? exeName : "DAT folder") + ": pick the game's own folder";
                sState = IMPORT_FAILED;
                return;
            }
            new File(sDest).mkdirs();
            long total = 0;
            for (Entry e : files)
                total += e.size;
            long free = new StatFs(sDest).getAvailableBytes();
            if (free < total + (64L << 20)) {
                sMessage = String.format(Locale.ROOT, "Not enough free space: %.1f GB needed, %.1f GB free",
                                         total / 1e9, free / 1e9);
                sState = IMPORT_FAILED;
                return;
            }
            sTotal = total;
            sState = IMPORT_COPYING;
            byte[] buf = new byte[1 << 20];
            for (Entry e : files) {
                File out = new File(sDest, e.rel);
                if (out.isFile() && out.length() == e.size) {   /* (there from before) */
                    sDone += e.size;
                    continue;
                }
                File parent = out.getParentFile();
                if (parent != null)
                    parent.mkdirs();
                sMessage = "Copying " + e.rel;
                File part = new File(out.getPath() + ".part");
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, e.id));
                     OutputStream os = new FileOutputStream(part)) {
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        sDone += n;
                    }
                }
                if (!part.renameTo(out)) {
                    out.delete();
                    if (!part.renameTo(out))
                        throw new java.io.IOException("couldn't write " + e.rel);
                }
            }
            sMessage = "Copied";
            sState = IMPORT_DONE;
        } catch (Exception e) {
            sMessage = "The copy stopped: " + e.getMessage();
            sState = IMPORT_FAILED;
        }
    }

    /* ---- updates (Updater.java), polled by native code (updater.c) ---- */
    public static String updateCurrent() {
        return Updater.current(mSingleton);
    }

    public static void updateCheck() {
        Updater.check(mSingleton);
    }

    public static void updateInstall() {
        Updater.install(mSingleton);
    }

    public static long[] updateStatus() {
        return new long[] { Updater.sState, Updater.sDone, Updater.sTotal };
    }

    public static String updateMessage() {
        return Updater.sMessage;
    }

    public static String updateVersion() {
        return Updater.sVersion;
    }

    /* a file bundled in the app (assets: the intro) to dest; false if it isn't there */
    public static boolean extractAsset(String name, String dest) {
        File out = new File(dest), part = new File(dest + ".part");
        File parent = out.getParentFile();
        if (parent != null)
            parent.mkdirs();
        byte[] buf = new byte[1 << 16];
        try (InputStream in = mSingleton.getAssets().open(name); OutputStream os = new FileOutputStream(part)) {
            int n;
            while ((n = in.read(buf)) > 0)
                os.write(buf, 0, n);
        } catch (Exception e) {
            part.delete();
            return false;
        }
        return part.renameTo(out);
    }
}
