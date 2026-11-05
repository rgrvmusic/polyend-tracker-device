// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
package nl.rgrv.ptpocket;

import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Copies the runtime assets into {@code filesDir} and marks them executable.
 * The set is the glibc 2.24 loader and libraries, the firmware, the font and
 * the card directories.
 */
public final class RuntimeStager {
    public static final String BIN = "tracker-firmware-mini";
    public static final String LOADER = "ld-linux-armhf.so.3";
    public static final String SD = "sdcard";
    private static final String STAMP = "tracker-runtime-4";

    private RuntimeStager() {}

    public static File binary(Context c) {
        return new File(c.getFilesDir(), "runtime/" + BIN);
    }

    public static File loader(Context c) {
        return new File(c.getFilesDir(), "runtime/" + LOADER);
    }

    public static File sdDir(Context c) {
        return new File(c.getFilesDir(), SD);
    }

    public static File font(Context c) {
        return new File(c.getFilesDir(), "fonts/NotoSansMono.ttf");
    }

    public static synchronized void stage(Context c) {
        File runtime = new File(c.getFilesDir(), "runtime");
        File stamp = new File(runtime, ".stamp");
        File bin = binary(c);
        try {
            if (!bin.isFile() || bin.length() == 0 || !stamp.isFile()
                    || !STAMP.equals(readStamp(stamp))) {
                runtime.mkdirs();
                String[] names = c.getAssets().list("runtime");
                if (names == null || names.length == 0) {
                    throw new IOException("the APK carries no runtime/ assets");
                }
                for (String name : names) {
                    copyAsset(c, "runtime/" + name, new File(runtime, name));
                }
                FileOutputStream s = new FileOutputStream(stamp);
                s.write(STAMP.getBytes("UTF-8"));
                s.close();
            }
            // Every launch: a restore or upgrade can return non-executable files.
            if (!bin.setExecutable(true, false)
                    || !loader(c).setExecutable(true, false)) {
                throw new IOException("cannot mark the runtime executable");
            }
            // execve needs the execute bit on the directory too.
            runtime.setExecutable(true, false);
            c.getFilesDir().setExecutable(true, false);

            File fonts = new File(c.getFilesDir(), "fonts");
            fonts.mkdirs();
            copyAsset(c, "fonts/NotoSansMono.ttf", font(c));

            File sd = sdDir(c);
            for (String d : new String[] {"", "Workspace", "Themes", "Fonts", "Mod"}) {
                new File(sd, d).mkdirs();
            }
        } catch (IOException e) {
            throw new RuntimeException("cannot stage the firmware runtime", e);
        }
    }

    private static String readStamp(File f) {
        try {
            byte[] b = new byte[(int) f.length()];
            InputStream in = new FileInputStream(f);
            int n = in.read(b);
            in.close();
            return n > 0 ? new String(b, 0, n, "UTF-8") : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static void copyAsset(Context c, String name, File out) throws IOException {
        InputStream in = c.getAssets().open(name);
        File parent = out.getParentFile();
        if (parent != null) parent.mkdirs();
        OutputStream o = new FileOutputStream(out);
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
        o.close();
        in.close();
    }
}
