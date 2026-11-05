// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
package nl.rgrv.ptpocket;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The card's files and how one gets there.
 *
 * The card is {@code filesDir/sdcard}, mounted by the firmware as
 * {@code TRACKER_SD}. Files arrive from the app's external {@code inbox}
 * directory ({@link #sweep}) or from the system picker ({@link #importFrom}).
 * Modules are written to {@code Mod/}, where the firmware's importer looks.
 */
final class UserFsStore {

    /** The inbox, under the app's external files directory. */
    private static final String INBOX = "inbox";

    /** Where the firmware's Import Mod screen browses. */
    private static final String MOD_DIR = "Mod";

    /** Extensions the Import Mod screen offers. */
    private static final String[] MODULE_SUFFIXES = {".mod", ".it"};

    private UserFsStore() {}

    /** The card directory the firmware mounts. */
    static File home(Context ctx) {
        File f = new File(ctx.getFilesDir(), RuntimeStager.SD);
        //noinspection ResultOfMethodCallIgnored
        f.mkdirs();
        return f;
    }

    /**
     * The inbox for {@link #sweep}, or null with no shared storage. It is
     * app-specific external storage: no permission, reachable by adb push.
     */
    static File inbox(Context ctx) {
        File base = ctx.getExternalFilesDir(null);
        if (base == null) return null;
        File f = new File(base, INBOX);
        //noinspection ResultOfMethodCallIgnored
        f.mkdirs();
        return f;
    }

    /** One row of the UserFS screen: a file on the card, and how big it is. */
    static final class Entry {
        final String name;   // relative to home, with '/' separators
        final long size;

        Entry(String name, long size) {
            this.name = name;
            this.size = size;
        }
    }

    /** Every file on the card, by relative path, case-insensitively sorted. */
    static List<Entry> list(Context ctx) {
        final List<Entry> out = new ArrayList<>();
        final File home = home(ctx);
        walk(home, home, out);
        out.sort((a, b) -> a.name.compareToIgnoreCase(b.name));
        return out;
    }

    private static void walk(File home, File dir, List<Entry> out) {
        final File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            final String name = f.getName();
            // Skip dot entries and ".part" files left by interrupted copies.
            if (name.startsWith(".") || name.endsWith(".part")) continue;
            if (f.isDirectory()) walk(home, f, out);
            else if (f.isFile()) out.add(new Entry(relative(home, f), f.length()));
        }
    }

    /** Removes one file; refuses paths outside {@link #home}. */
    static boolean delete(Context ctx, File file) {
        if (file == null) return false;
        try {
            final String home = home(ctx).getCanonicalPath();
            final String path = file.getCanonicalPath();
            if (!path.startsWith(home + File.separator)) return false;
        } catch (IOException e) {
            return false;   // a path that cannot be resolved is not one to unlink
        }
        // The remains of an import killed mid-copy. Nothing lists it.
        //noinspection ResultOfMethodCallIgnored
        new File(file.getParentFile(), file.getName() + ".part").delete();
        return file.delete();
    }

    /**
     * Copies the inbox tree onto the card, keeping its layout. Files that are
     * missing, smaller or older on the card are copied; nothing is removed.
     */
    static List<String> sweep(Context ctx) {
        final List<String> lines = new ArrayList<>();
        final File home = home(ctx);
        final File inbox = inbox(ctx);

        if (inbox == null) {
            lines.add("no shared storage on this device; use Import");
            return lines;
        }

        final int[] count = {0, 0}; // copied, seen
        sweep(inbox, inbox, home, lines, count);
        lines.add("inbox      : " + inbox);
        lines.add("copied     : " + count[0] + " of " + count[1] + " file(s) from the inbox");
        return lines;
    }

    private static void sweep(File inbox, File dir, File home, List<String> lines, int[] count) {
        final File[] files = dir.listFiles();
        if (files == null) return;
        for (File src : files) {
            final String name = src.getName();
            if (name.startsWith(".") || name.endsWith(".part")) continue;
            final File dst = new File(home, relative(inbox, src));
            if (src.isDirectory()) {
                //noinspection ResultOfMethodCallIgnored
                dst.mkdirs();
                sweep(inbox, src, home, lines, count);
                continue;
            }
            if (!src.isFile()) continue;
            count[1]++;
            if (dst.isFile() && dst.length() == src.length()
                    && dst.lastModified() >= src.lastModified()) {
                continue;
            }
            try {
                copy(new FileInputStream(src), dst);
                //noinspection ResultOfMethodCallIgnored
                dst.setLastModified(src.lastModified());
                count[0]++;
            } catch (IOException e) {
                lines.add("could not copy " + name + ": " + e.getMessage());
            }
        }
    }

    /** Copies a picked file onto the card. Returns a line for the screen. */
    static String importFrom(Context ctx, Uri uri) {
        final String name = displayName(ctx, uri);
        if (name == null) return "that file has no name this app can read";

        final File dst = new File(destination(ctx, name), name);
        try (InputStream in = ctx.getContentResolver().openInputStream(uri)) {
            if (in == null) return "cannot read " + name;
            // Through copy(), not straight into dst: ".part" stops a copy
            // killed mid-write from leaving a truncated file the firmware may
            // read.
            copy(in, dst);
        } catch (Exception e) {
            return "could not copy " + name + ": " + e;
        }
        return "imported   : " + relative(home(ctx), dst);
    }

    /** The folder a file of this name belongs in; modules go to Mod. */
    private static File destination(Context ctx, String name) {
        final String lower = name.toLowerCase(Locale.US);
        for (String suffix : MODULE_SUFFIXES) {
            if (lower.endsWith(suffix)) {
                final File mod = new File(home(ctx), MOD_DIR);
                //noinspection ResultOfMethodCallIgnored
                mod.mkdirs();
                return mod;
            }
        }
        return home(ctx);
    }

    /** A picked URI's file name, as the picker's own provider spells it. */
    private static String displayName(Context ctx, Uri uri) {
        String name = uri.getLastPathSegment();
        try (Cursor c = ctx.getContentResolver().query(uri,
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                final int col = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (col >= 0) name = c.getString(col);
            }
        } catch (Exception e) {
            // Fall back to the last path segment if the provider will not
            // answer.
        }
        return safeName(name);
    }

    /** Reduces a provider name to a basename; null when unusable. */
    private static String safeName(String name) {
        if (name == null) return null;
        final int slash = Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
        name = slash >= 0 ? name.substring(slash + 1) : name;
        if (name.isEmpty() || name.contains("..")) return null;
        return name;
    }

    /** A file's path relative to an ancestor directory, with '/' separators. */
    private static String relative(File base, File f) {
        final String root = base.getAbsolutePath();
        final String path = f.getAbsolutePath();
        if (!path.startsWith(root + File.separator)) return f.getName();
        return path.substring(root.length() + 1).replace(File.separatorChar, '/');
    }

    /** Copies through a ".part" name so an interrupted copy leaves no file. */
    private static void copy(InputStream in, File dst) throws IOException {
        final File parent = dst.getParentFile();
        if (parent != null) {
            //noinspection ResultOfMethodCallIgnored
            parent.mkdirs();
        }
        final File tmp = new File(parent, dst.getName() + ".part");
        try (OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        //noinspection ResultOfMethodCallIgnored
        tmp.renameTo(dst);
    }
}
