package org.mml.legends;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Locale;

import org.json.JSONArray;
import org.json.JSONObject;

/* The app's updater: looks up the newest release on GitHub, downloads its APK into the app's
 * cache, checks it (SHA-256 from GitHub, same package, same signing key, newer) and hands it to
 * the system's PackageInstaller. Android always asks the player to confirm an install, and to
 * allow "install unknown apps" for this app once; nothing here can skip that.
 *
 * Runs on threads of its own; native code (updater.c) polls status(), message() and version()
 * through MmlActivity, as it does for the game-files import. */
final class Updater {
    static final int IDLE = 0, CHECKING = 1, UPTODATE = 2, AVAILABLE = 3, DOWNLOADING = 4, READY = 5,
                     INSTALLING = 6, FAILED = 7;
    static final String API = "https://api.github.com/repos/Drakfix/MML_Modernization-Project/releases/latest";
    static final String ACTION = "org.mml.legends.INSTALL_RESULT";

    static volatile int sState = IDLE;
    static volatile long sDone, sTotal;
    static volatile String sMessage = "", sVersion = "";
    static String sUrl = "", sDigest = "";
    static long sSize;
    static File sApk;
    static boolean sReceiver;

    /* this app's version name, e.g. "0.6.4-beta" */
    static String current(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    /* the numbers of a version ("v0.6.5-beta" -> 0, 6, 5) */
    static int[] numbers(String v) {
        int[] n = new int[3];
        int k = 0;
        for (String p : v.replaceFirst("^[vV]", "").split("[^0-9]+")) {
            if (p.isEmpty())
                continue;
            if (k < 3)
                n[k++] = Integer.parseInt(p);
        }
        return n;
    }

    static boolean newer(String latest, String mine) {
        int[] a = numbers(latest), b = numbers(mine);
        for (int i = 0; i < 3; i++)
            if (a[i] != b[i])
                return a[i] > b[i];
        return false;
    }

    static HttpURLConnection open(String url) throws Exception {
        HttpURLConnection h = (HttpURLConnection) new URL(url).openConnection();
        h.setConnectTimeout(10000);
        h.setReadTimeout(20000);
        h.setRequestProperty("User-Agent", "MML-Modernization-Updater");
        h.setRequestProperty("Accept", "application/vnd.github+json, application/octet-stream");
        return h;
    }

    static boolean busy() {
        return sState == CHECKING || sState == DOWNLOADING || sState == INSTALLING;
    }

    /* look up the newest release (a thread of its own) */
    static void check(final Activity a) {
        if (busy())
            return;
        sState = CHECKING;
        sMessage = "Looking for a new version...";
        new Thread(new Runnable() {
            public void run() {
                try {
                    String mine = current(a);
                    HttpURLConnection h = open(API);
                    if (h.getResponseCode() != 200)
                        throw new java.io.IOException("GitHub answered " + h.getResponseCode());
                    StringBuilder sb = new StringBuilder();
                    byte[] buf = new byte[8192];
                    try (InputStream in = h.getInputStream()) {
                        int n;
                        while ((n = in.read(buf)) > 0)
                            sb.append(new String(buf, 0, n, "UTF-8"));
                    }
                    JSONObject rel = new JSONObject(sb.toString());
                    String tag = rel.getString("tag_name");
                    String url = "", digest = "";
                    long size = 0;
                    JSONArray assets = rel.getJSONArray("assets");
                    for (int i = 0; i < assets.length(); i++) {
                        JSONObject as = assets.getJSONObject(i);
                        if (as.getString("name").toLowerCase(Locale.ROOT).endsWith(".apk")) {
                            url = as.getString("browser_download_url");
                            digest = as.optString("digest", "");
                            size = as.optLong("size", 0);
                        }
                    }
                    sVersion = tag.replaceFirst("^[vV]", "");
                    if (!newer(tag, mine)) {
                        sMessage = "You have the newest version (" + mine + ")";
                        sState = UPTODATE;
                    } else if (url.isEmpty()) {
                        sMessage = sVersion + " has no Android package yet";
                        sState = FAILED;
                    } else {
                        sUrl = url;
                        sDigest = digest;
                        sSize = size;
                        sMessage = sVersion + " is available";
                        sState = AVAILABLE;
                    }
                } catch (Exception e) {
                    sMessage = "Couldn't look for updates: " + e.getMessage();
                    sState = FAILED;
                }
            }
        }).start();
    }

    /* the player chose Update: install the downloaded package, or download it first */
    static void install(final Activity a) {
        if (busy())
            return;
        if (sState == READY && sApk != null && sApk.isFile()) {   /* downloaded before: just install */
            new Thread(new Runnable() {
                public void run() {
                    handOver(a);
                }
            }).start();
            return;
        }
        if (sState == READY)   /* (the file is gone: fetch it again) */
            sState = AVAILABLE;
        if (sState != AVAILABLE)
            return;
        sState = DOWNLOADING;
        sDone = 0;
        sTotal = sSize;
        sMessage = "Downloading " + sVersion;
        new Thread(new Runnable() {
            public void run() {
                File dir = new File(a.getCacheDir(), "update");
                File part = new File(dir, "MML.apk.part"), apk = new File(dir, "MML.apk");
                try {
                    dir.mkdirs();
                    apk.delete();
                    MessageDigest md = MessageDigest.getInstance("SHA-256");
                    HttpURLConnection h = open(sUrl);
                    if (h.getResponseCode() != 200)
                        throw new java.io.IOException("the server answered " + h.getResponseCode());
                    if (sTotal <= 0)
                        sTotal = h.getContentLengthLong();
                    byte[] buf = new byte[1 << 16];
                    try (InputStream in = h.getInputStream(); OutputStream os = new FileOutputStream(part)) {
                        int n;
                        while ((n = in.read(buf)) > 0) {
                            os.write(buf, 0, n);
                            md.update(buf, 0, n);
                            sDone += n;
                        }
                    }
                    if (sSize > 0 && part.length() != sSize)
                        throw new java.io.IOException("the download is incomplete");
                    if (sDigest.startsWith("sha256:")) {
                        StringBuilder hex = new StringBuilder();
                        for (byte b : md.digest())
                            hex.append(String.format(Locale.ROOT, "%02x", b));
                        if (!sDigest.substring(7).equalsIgnoreCase(hex.toString()))
                            throw new java.io.IOException("the download doesn't match its checksum");
                    }
                    if (!part.renameTo(apk))
                        throw new java.io.IOException("couldn't save the download");
                    String why = problem(a, apk);
                    if (why != null) {
                        apk.delete();
                        throw new java.io.IOException(why);
                    }
                    sApk = apk;
                    sState = READY;
                    handOver(a);
                } catch (Exception e) {
                    part.delete();
                    sMessage = "The update failed: " + e.getMessage();
                    sState = FAILED;
                }
            }
        }).start();
    }

    /* null if the package can replace this app, else why not */
    static String problem(Context c, File apk) {
        PackageManager pm = c.getPackageManager();
        PackageInfo mine, got;
        try {
            mine = pm.getPackageInfo(c.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
        } catch (Exception e) {
            return null;   /* (can't tell: let the system decide) */
        }
        got = pm.getPackageArchiveInfo(apk.getPath(), PackageManager.GET_SIGNING_CERTIFICATES);
        if (got == null)
            return "the download isn't a valid package";
        if (!c.getPackageName().equals(got.packageName))
            return "the download is a different app";
        if (got.getLongVersionCode() <= mine.getLongVersionCode())
            return "the download isn't newer than this version";
        if (got.signingInfo != null && mine.signingInfo != null) {
            Signature[] a = got.signingInfo.getApkContentsSigners(), b = mine.signingInfo.getApkContentsSigners();
            boolean same = a.length > 0 && a.length == b.length;
            for (int i = 0; same && i < a.length; i++)
                same = a[i].equals(b[i]);
            if (!same)
                return "the download is signed with a different key, so Android would refuse it";
        }
        return null;
    }

    /* the package to the system's installer (which asks the player to confirm) */
    static void handOver(final Activity a) {
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) {
            sMessage = "Allow installs from this app in the settings that open, come back and choose Update again";
            sState = READY;
            a.runOnUiThread(new Runnable() {
                public void run() {
                    try {
                        a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                                   Uri.parse("package:" + a.getPackageName())));
                    } catch (Exception e) {
                        sMessage = "Allow \"install unknown apps\" for this app in Android's settings, then choose Update again";
                    }
                }
            });
            return;
        }
        sState = INSTALLING;
        sMessage = "Waiting for Android to install it";
        try {
            if (!sReceiver) {
                IntentFilter f = new IntentFilter(ACTION);
                BroadcastReceiver r = new BroadcastReceiver() {
                    public void onReceive(Context c, Intent i) {
                        result(a, i);
                    }
                };
                if (Build.VERSION.SDK_INT >= 33)
                    a.registerReceiver(r, f, Context.RECEIVER_NOT_EXPORTED);
                else
                    a.registerReceiver(r, f);
                sReceiver = true;
            }
            PackageInstaller pi = a.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams p = new PackageInstaller.SessionParams(
                    PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            p.setAppPackageName(a.getPackageName());
            if (Build.VERSION.SDK_INT >= 31)
                p.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
            int id = pi.createSession(p);
            try (PackageInstaller.Session s = pi.openSession(id)) {
                byte[] buf = new byte[1 << 16];
                try (InputStream in = new FileInputStream(sApk);
                     OutputStream os = s.openWrite("mml.apk", 0, sApk.length())) {
                    int n;
                    while ((n = in.read(buf)) > 0)
                        os.write(buf, 0, n);
                    s.fsync(os);
                }
                Intent back = new Intent(ACTION).setPackage(a.getPackageName());
                PendingIntent pend = PendingIntent.getBroadcast(a, id, back,
                        PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0));
                s.commit(pend.getIntentSender());
            }
        } catch (Exception e) {
            sMessage = "Couldn't start the install: " + e.getMessage();
            sState = FAILED;
        }
    }

    /* the installer's answer */
    static void result(Activity a, Intent i) {
        int status = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        String msg = i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {   /* the confirmation screen */
            Intent confirm = i.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                a.startActivity(confirm);
            }
        } else if (status == PackageInstaller.STATUS_SUCCESS) {
            sMessage = "Updated";
            sState = IDLE;
        } else if (status == PackageInstaller.STATUS_FAILURE_ABORTED) {
            sMessage = "The install was cancelled. Choose Update to try again";
            sState = READY;
        } else {
            sMessage = "Android couldn't install it" + (msg != null ? ": " + msg : "");
            sState = FAILED;
        }
    }
}
