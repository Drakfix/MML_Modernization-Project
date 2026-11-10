package org.mml.legends;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigDecimal;
import java.math.RoundingMode;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/* English subtitles for the cutscenes, from the Rockman DASH PSP English project's script: the
 * same as tools/voice/build_subtitles.py --download writes, for the app (no Python there). The
 * file is downloaded from that project's GitHub repository at a fixed commit and checked; the
 * lines are that project's transcription and timing of the US voice acting (see CREDIT). */
final class Subtitles {
    static final String REPO = "https://github.com/JamesAZ0/RockmanDASH-PSP-English-Patcher";
    static final String COMMIT = "7d49ceb334aae2b2381bdde3d03448cbd90320da";
    static final String PATH = "script/subtitles_en.csv";
    static final String URL_ = "https://raw.githubusercontent.com/JamesAZ0/RockmanDASH-PSP-English-Patcher/"
            + COMMIT + "/" + PATH;
    static final String SHA256 = "0822f5228b3248dd62a358611abe1a7a6182c6a15de9e210896f6b753579697c";
    static final String[] CREDIT = {
        "English subtitles for the cutscenes: the work of the Rockman DASH PSP English project",
        "(JamesAZ0 and its contributors), " + REPO + ",",
        "from its " + PATH + " at commit " + COMMIT.substring(0, 7) + ", downloaded from there during setup.",
        "That project transcribed the US voice acting and timed the lines to it, under the MIT licence:",
        "Copyright (c) 2026 The Rockman DASH PSP English project contributors.",
        "The lines belong to Capcom.",
    };
    static final String[] TRACKS = { "3700E", "3701E" };   /* the US cutscene tracks (A, B scenes) */
    static final String NEWS_BANK = "1821E";               /* C scenes 0-4: the news reports */
    static final int[] NEWS_CLIP = { 10, 4, 5, 7, 6 };

    static String hex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b)
            s.append(String.format("%02x", x & 0xFF));
        return s.toString();
    }

    /* the project's script file, checked against SHA256 */
    static byte[] download() throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(URL_).openConnection();
        c.setConnectTimeout(30000);
        c.setReadTimeout(30000);
        c.setRequestProperty("User-Agent", "mml-modernization-setup");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (InputStream in = c.getInputStream()) {
            byte[] buf = new byte[1 << 14];
            int k;
            while ((k = in.read(buf)) > 0)
                out.write(buf, 0, k);
        } finally {
            c.disconnect();
        }
        byte[] data = out.toByteArray();
        if (!hex(MessageDigest.getInstance("SHA-256").digest(data)).equals(SHA256))
            throw new IOException("the file from " + REPO + " is not the expected one, so nothing was written "
                    + "(the repository may have changed)");
        return data;
    }

    /* one CSV record per line (the script has no line breaks inside fields) */
    static List<String> fields(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder f = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < line.length(); i++) {
            char ch = line.charAt(i);
            if (quoted) {
                if (ch == '"' && i + 1 < line.length() && line.charAt(i + 1) == '"') {
                    f.append('"');
                    i++;
                } else if (ch == '"')
                    quoted = false;
                else
                    f.append(ch);
            } else if (ch == '"')
                quoted = true;
            else if (ch == ',') {
                out.add(f.toString());
                f.setLength(0);
            } else
                f.append(ch);
        }
        out.add(f.toString());
        return out;
    }

    static String asciiText(String s) {
        StringBuilder a = new StringBuilder();
        for (char ch : s.trim().toCharArray()) {
            switch (ch) {
            case '‘': case '’': a.append('\''); break;
            case '“': case '”': a.append('"'); break;
            case '…': a.append("..."); break;
            case '–': a.append('-'); break;
            case '—': a.append("--"); break;
            case ' ': a.append(' '); break;
            case 'é': case 'è': a.append('e'); break;
            case 'à': a.append('a'); break;
            case '™': break;
            default: a.append(ch);
            }
        }
        StringBuilder out = new StringBuilder();
        for (String w : a.toString().trim().split("\\s+")) {
            if (w.isEmpty())
                continue;
            if (out.length() > 0)
                out.append(' ');
            for (char ch : w.toCharArray())
                out.append(ch >= 32 && ch < 127 ? ch : '?');
        }
        return out.toString();
    }

    static String seconds(double v) {   /* (as Python's %.2f: the double's exact value, half to even) */
        return new BigDecimal(v).setScale(2, RoundingMode.HALF_EVEN).toPlainString();
    }

    /* the subtitle file (Modernization/subtitles_en.txt) from the script */
    static String convert(String csv) {
        String[] lines = csv.replace("\r\n", "\n").split("\n");
        List<String> head = fields(lines[0]);
        int scene = head.indexOf("scene"), start = head.indexOf("start_s"), end = head.indexOf("end_s"),
            text = head.indexOf("text"), fin = head.indexOf("en_final"), status = head.indexOf("status"),
            over = head.indexOf("time_override");
        StringBuilder out = new StringBuilder();
        for (String c : CREDIT)
            out.append("# ").append(c).append('\n');
        out.append("# bank clip start end text: built by tools/voice/build_subtitles.py\n");
        for (int i = 1; i < lines.length; i++) {
            if (lines[i].isEmpty())
                continue;
            List<String> r = fields(lines[i]);
            while (r.size() < head.size())
                r.add("");
            if (r.get(status).trim().equals("drop"))
                continue;
            String sc = r.get(scene).trim();
            int n = Integer.parseInt(sc.substring(1));
            String bank;
            int clip;
            if (sc.charAt(0) == 'A') {
                bank = TRACKS[0];
                clip = n;
            } else if (sc.charAt(0) == 'B') {
                bank = TRACKS[1];
                clip = n + 2;
            } else if (sc.charAt(0) == 'C' && n >= 0 && n < NEWS_CLIP.length) {
                bank = NEWS_BANK;
                clip = NEWS_CLIP[n];
            } else
                continue;
            double s = Double.parseDouble(r.get(start)), e = Double.parseDouble(r.get(end));
            if (!r.get(over).trim().isEmpty()) {
                String[] t = r.get(over).split("-");
                s = Double.parseDouble(t[0].trim());
                e = Double.parseDouble(t[1].trim());
            }
            String t = asciiText(r.get(fin).isEmpty() ? r.get(text) : r.get(fin));
            if (!t.isEmpty())
                out.append(bank).append(' ').append(clip).append(' ').append(seconds(s)).append(' ')
                   .append(seconds(e)).append(' ').append(t).append('\n');
        }
        return out.toString();
    }

    /* download, convert, write dir/subtitles_en.txt (dir: the game's Modernization folder) */
    static void install(File dir) throws Exception {
        String txt = convert(new String(download(), StandardCharsets.UTF_8));
        dir.mkdirs();
        File tmp = new File(dir, "subtitles_en.txt.part"), dst = new File(dir, "subtitles_en.txt");
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write(txt.getBytes(StandardCharsets.US_ASCII));
        }
        if (!tmp.renameTo(dst)) {
            dst.delete();
            if (!tmp.renameTo(dst))
                throw new IOException("couldn't write " + dst);
        }
    }

    /* (a check on a PC: java Subtitles.java script.csv > out.txt) */
    public static void main(String[] a) throws Exception {
        String csv = new String(java.nio.file.Files.readAllBytes(new File(a[0]).toPath()), StandardCharsets.UTF_8);
        System.out.print(convert(csv));
    }
}
