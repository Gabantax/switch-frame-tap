/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.graphics.Bitmap;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** v0.7.16: where screenshots and recordings go - Pictures/Switch Frame Tap and
 *  Movies/Switch Frame Tap, where the gallery finds them. Android 10 and newer
 *  through MediaStore (no permission); older ones write the folder directly
 *  (the storage permission, asked once). */
final class Capture {
    static final String FOLDER = "Switch Frame Tap";

    /** Android 9 and older need the storage permission for the shared folders. */
    static boolean needsPermission() { return Build.VERSION.SDK_INT < 29; }

    static final class Target {
        final Context context;
        final Uri uri;                   // Android 10+: the MediaStore entry
        final ParcelFileDescriptor fd;   // ... opened for the muxer
        final String path;               // older: the file
        final String shown;              // what the toast says
        Target(Context context, Uri uri, ParcelFileDescriptor fd, String path, String shown) {
            this.context = context; this.uri = uri; this.fd = fd; this.path = path; this.shown = shown;
        }
    }

    private static String stamp() { return new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()); }

    /** A new MP4 in Movies/Switch Frame Tap. */
    static Target newVideo(Context context) throws IOException {
        String name = "switch-frame-tap-" + stamp() + ".mp4";
        String shown = Environment.DIRECTORY_MOVIES + "/" + FOLDER + "/" + name;
        if (Build.VERSION.SDK_INT >= 29) {
            ContentResolver r = context.getContentResolver();
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
            v.put(MediaStore.MediaColumns.MIME_TYPE, "video/mp4");
            v.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_MOVIES + "/" + FOLDER);
            v.put(MediaStore.MediaColumns.IS_PENDING, 1);
            Uri uri = r.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, v);
            if (uri == null) throw new IOException("the Movies folder is not available");
            ParcelFileDescriptor fd = r.openFileDescriptor(uri, "rw");
            if (fd == null) { r.delete(uri, null, null); throw new IOException("could not open the new video"); }
            return new Target(context, uri, fd, null, shown);
        }
        File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES), FOLDER);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("could not create " + dir);
        return new Target(context, null, null, new File(dir, name).getAbsolutePath(), shown);
    }

    /** Publish the file to the gallery, or remove it when nothing was saved. */
    static void finish(Target t, boolean saved) {
        if (t.fd != null) try { t.fd.close(); } catch (IOException ignored) { }
        if (Build.VERSION.SDK_INT >= 29 && t.uri != null) {
            ContentResolver r = t.context.getContentResolver();
            if (saved) {
                ContentValues v = new ContentValues();
                v.put(MediaStore.MediaColumns.IS_PENDING, 0);
                r.update(t.uri, v, null, null);
            } else {
                r.delete(t.uri, null, null);
            }
        } else if (t.path != null) {
            if (saved) MediaScannerConnection.scanFile(t.context, new String[]{t.path}, new String[]{"video/mp4"}, null);
            else //noinspection ResultOfMethodCallIgnored
                new File(t.path).delete();
        }
    }

    /** The picture as a PNG in Pictures/Switch Frame Tap; returns where. */
    static String savePng(Context context, Bitmap picture) throws IOException {
        String name = "switch-frame-tap-" + stamp() + ".png";
        String shown = Environment.DIRECTORY_PICTURES + "/" + FOLDER + "/" + name;
        if (Build.VERSION.SDK_INT >= 29) {
            ContentResolver r = context.getContentResolver();
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
            v.put(MediaStore.MediaColumns.MIME_TYPE, "image/png");
            v.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/" + FOLDER);
            v.put(MediaStore.MediaColumns.IS_PENDING, 1);
            Uri uri = r.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, v);
            if (uri == null) throw new IOException("the Pictures folder is not available");
            boolean ok = false;
            try (OutputStream out = r.openOutputStream(uri)) {
                if (out == null) throw new IOException("could not open the new picture");
                ok = picture.compress(Bitmap.CompressFormat.PNG, 100, out);
                if (!ok) throw new IOException("the PNG could not be written");
            } finally {
                if (ok) { v.clear(); v.put(MediaStore.MediaColumns.IS_PENDING, 0); r.update(uri, v, null, null); }
                else r.delete(uri, null, null);
            }
            return shown;
        }
        File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES), FOLDER);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("could not create " + dir);
        File file = new File(dir, name);
        try (FileOutputStream out = new FileOutputStream(file)) {
            if (!picture.compress(Bitmap.CompressFormat.PNG, 100, out)) throw new IOException("the PNG could not be written");
        }
        MediaScannerConnection.scanFile(context, new String[]{file.getAbsolutePath()}, new String[]{"image/png"}, null);
        return shown;
    }
}
