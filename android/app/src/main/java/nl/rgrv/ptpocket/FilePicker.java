// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
package nl.rgrv.ptpocket;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * The UserFS screen's file picker. {@code ACTION_OPEN_DOCUMENT} grants access
 * to the chosen files only, so no runtime permission is needed; the result is
 * a {@code content://} URI, handled by {@link UserFsStore#importFrom}.
 */
public class FilePicker {
    private static final String TAG = "Tracker-Picker";

    public static final int PICK_FILES_REQUEST = 11;

    public interface FilesSelectedListener {
        void onFilesSelected(List<Uri> uris);
    }

    private final Activity activity;
    private final FilesSelectedListener listener;

    public FilePicker(Activity activity, FilesSelectedListener listener) {
        this.activity = activity;
        this.listener = listener;
    }

    /** Show the system file picker, allowing multiple files. */
    public void openFilePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        activity.startActivityForResult(intent, PICK_FILES_REQUEST);
    }

    /**
     * Handles the picker's result.
     *
     * One file arrives in the intent's data; several arrive in its
     * {@link ClipData}. Reading only {@code getData()} would import the first
     * and drop the rest.
     */
    public void handleActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_FILES_REQUEST || resultCode != Activity.RESULT_OK) {
            return;
        }

        final List<Uri> uris = new ArrayList<>();
        if (data != null) {
            final ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); ++i) {
                    final Uri uri = clip.getItemAt(i).getUri();
                    if (uri != null) uris.add(uri);
                }
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
        }

        if (uris.isEmpty()) {
            Log.w(TAG, "The picker returned no files");
            return;
        }

        Log.i(TAG, "Picked " + uris.size() + " file(s)");
        if (listener != null) listener.onFilesSelected(uris);
    }
}
