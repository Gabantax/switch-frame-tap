/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import org.json.JSONArray;
import org.json.JSONObject;

/** v0.7.14: the app updates itself from the project's GitHub releases, as
 *  the PC viewer and the Switch app do. A check asks api.github.com for the
 *  newest release (with betas, the highest version among the latest of any
 *  kind - GitHub lists them by date) and compares its tag with this app's
 *  versionName (the same rules as common/sft_version.h). An update downloads
 *  the release's -android.apk into the cache and hands it to the system's
 *  PackageInstaller: Android shows its own confirmation (and, the first time,
 *  asks to allow installs from this app); the release key signs every APK,
 *  so it installs over this one. */
final class Updater {
    static final String REPO = "TomasUribe/switch-frame-tap";
    private static final String ACTION = "io.github.switchframetap.tv.INSTALL_RESULT";

    enum State { IDLE, CHECKING, LATEST, FOUND, GETTING, INSTALLING, FAILED }

    interface Listener { void changed(); }

    private final Context context;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Listener listener;
    volatile State state = State.IDLE;
    volatile String tag = "", url = "", message = "";
    volatile int percent = -1;
    private boolean receiverOn;

    private final BroadcastReceiver result = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent intent) {
            int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
            if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirm != null) { confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); c.startActivity(confirm); }
                return;
            }
            if (status == PackageInstaller.STATUS_SUCCESS) return;   // the new app replaces this one
            String why = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            fail(status == PackageInstaller.STATUS_FAILURE_ABORTED ? "The update was cancelled"
                    : "Android did not install it" + (why != null ? " (" + why + ")" : ""));
        }
    };

    Updater(Context context, Listener listener) { this.context = context; this.listener = listener; }

    void release() {
        if (receiverOn) { try { context.unregisterReceiver(result); } catch (IllegalArgumentException ignored) { } receiverOn = false; }
    }

    private void set(State s) { state = s; main.post(listener::changed); }
    private void fail(String why) { message = why; set(State.FAILED); }

    static String version(Context c) {
        try { return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName; }
        catch (Exception e) { return "dev"; }
    }

    /* ---- versions: common/sft_version.h in Java ---- */

    private static long[] parse(String s) {
        long[] v = { 0, 0, 0, 0, 3, 0 };   // four numbers, kind (3 release, 2 rc, 1 beta, 0 other), its number
        int i = 0, n = s.length();
        if (i < n && (s.charAt(i) == 'v' || s.charAt(i) == 'V')) i++;
        for (int k = 0; k < 4 && i < n && Character.isDigit(s.charAt(i)); k++) {
            while (i < n && Character.isDigit(s.charAt(i))) v[k] = v[k] * 10 + (s.charAt(i++) - '0');
            if (i < n && s.charAt(i) == '.') i++;
        }
        if (i < n && s.charAt(i) == '-') {
            i++;
            String rest = s.substring(i);
            v[4] = rest.startsWith("rc") ? 2 : rest.startsWith("beta") ? 1 : 0;
            while (i < n && !Character.isDigit(s.charAt(i))) i++;
            while (i < n && Character.isDigit(s.charAt(i))) v[5] = v[5] * 10 + (s.charAt(i++) - '0');
        }
        return v;
    }

    static int compare(String a, String b) {
        long[] x = parse(a), y = parse(b);
        for (int i = 0; i < 6; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
        return 0;
    }

    /* ---- the check ---- */

    private static String get(String address) throws Exception {
        HttpURLConnection c = (HttpURLConnection)new URL(address).openConnection();
        c.setConnectTimeout(15000); c.setReadTimeout(30000);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        c.setRequestProperty("User-Agent", "switch-frame-tap-android");
        try (InputStream in = c.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] b = new byte[16384];
            for (int r; (r = in.read(b)) > 0; ) out.write(b, 0, r);
            return out.toString("UTF-8");
        } finally { c.disconnect(); }
    }

    void check(boolean betas) {
        if (state == State.CHECKING || state == State.GETTING || state == State.INSTALLING) return;
        set(State.CHECKING);
        final String current = version(context);
        new Thread(() -> {
            try {
                JSONObject best = null;
                if (betas) {
                    JSONArray list = new JSONArray(get("https://api.github.com/repos/" + REPO + "/releases?per_page=15"));
                    for (int i = 0; i < list.length(); i++) {
                        JSONObject r = list.getJSONObject(i);
                        if (r.optBoolean("draft")) continue;
                        if (best == null || compare(r.optString("tag_name"), best.optString("tag_name")) > 0) best = r;
                    }
                } else {
                    best = new JSONObject(get("https://api.github.com/repos/" + REPO + "/releases/latest"));
                }
                if (best == null) { fail("GitHub's answer has no release"); return; }
                String t = best.optString("tag_name");
                if (compare(t, current) <= 0) { set(State.LATEST); return; }
                JSONArray assets = best.optJSONArray("assets");
                String found = "";
                for (int i = 0; assets != null && i < assets.length(); i++) {
                    JSONObject a = assets.getJSONObject(i);
                    if (a.optString("name").endsWith("-android.apk")) { found = a.optString("browser_download_url"); break; }
                }
                if (found.isEmpty()) { fail(t + " has no Android app yet"); return; }
                tag = t; url = found;
                set(State.FOUND);
            } catch (Exception e) {
                fail("GitHub could not be reached");
            }
        }, "update-check").start();
    }

    /* ---- the update ---- */

    void install() {
        if (state != State.FOUND && state != State.FAILED) return;
        if (url.isEmpty()) return;
        percent = -1;
        listen();   // before the session is committed: its first answer asks for the user's confirmation
        set(State.GETTING);
        new Thread(() -> {
            File apk = new File(context.getCacheDir(), "update.apk");
            try {
                HttpURLConnection c = (HttpURLConnection)new URL(url).openConnection();   // follows GitHub's redirect to its file host
                c.setConnectTimeout(15000); c.setReadTimeout(60000);
                c.setRequestProperty("User-Agent", "switch-frame-tap-android");
                long total = c.getContentLengthLong(), got = 0;
                try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(apk)) {
                    byte[] b = new byte[65536];
                    for (int r; (r = in.read(b)) > 0; ) {
                        out.write(b, 0, r);
                        got += r;
                        int p = total > 0 ? (int)(got * 100 / total) : -1;
                        if (p != percent) { percent = p; main.post(listener::changed); }
                    }
                } finally { c.disconnect(); }

                PackageInstaller installer = context.getPackageManager().getPackageInstaller();
                PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                params.setAppPackageName(context.getPackageName());
                int id = installer.createSession(params);
                try (PackageInstaller.Session session = installer.openSession(id)) {
                    try (InputStream in = new FileInputStream(apk); OutputStream out = session.openWrite("switch-frame-tap", 0, apk.length())) {
                        byte[] b = new byte[65536];
                        for (int r; (r = in.read(b)) > 0; ) out.write(b, 0, r);
                        session.fsync(out);
                    }
                    Intent done = new Intent(ACTION).setPackage(context.getPackageName());
                    int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
                    PendingIntent pending = PendingIntent.getBroadcast(context, id, done, flags);
                    session.commit(pending.getIntentSender());
                }
                set(State.INSTALLING);
            } catch (Exception e) {
                apk.delete();
                fail("The download did not finish - try again");
            }
        }, "update-get").start();
    }

    private void listen() {
        if (receiverOn) return;
        IntentFilter filter = new IntentFilter(ACTION);
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(result, filter, Context.RECEIVER_NOT_EXPORTED);
        else context.registerReceiver(result, filter);
        receiverOn = true;
    }

    /** one line for the menus */
    String status(String current) {
        switch (state) {
            case CHECKING:   return "Checking GitHub...";
            case LATEST:     return "Up to date (" + current + ")";
            case FOUND:      return tag + " is available (you have " + current + ")";
            case GETTING:    return "Downloading " + tag + (percent >= 0 ? " - " + percent + " %" : "...");
            case INSTALLING: return "Installing " + tag + " - confirm on screen";
            case FAILED:     return message;
            default:         return "Version " + current;
        }
    }
}
