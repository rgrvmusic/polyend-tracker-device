// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
package nl.rgrv.ptpocket;

import android.content.Intent;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

/**
 * One activity, one SDL surface, one native main (libmain.so). Java stages the
 * runtime, holds a foreground service for audio, bridges MidiManager and opens
 * {@link UserFsActivity}.
 */
public class MainActivity extends SDLActivity {
    private MidiHandler midi;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Stage before super.onCreate(): SDL starts the native thread there and
        // it execs the firmware immediately.
        RuntimeStager.stage(this);
        super.onCreate(savedInstanceState);
        // SDL reports a content scale of 1.0 here, so the density comes from
        // Android.
        nativeUiScale(getResources().getDisplayMetrics().density);
        startForegroundService(new Intent(this, AudioService.class));
        midi = new MidiHandler(this);
        goFullscreen();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    /** Show the UserFS screen; called from the panel's top bar. */
    public static void openUserFsScreen() {
        final MainActivity a = (MainActivity) mSingleton;
        if (a == null) return;
        // startActivity is not specified for the SDL frame loop's thread;
        // post it to the UI thread.
        a.runOnUiThread(() -> a.startActivity(new Intent(a, UserFsActivity.class)));
    }

    @Override
    protected void onResume() {
        super.onResume();
        goFullscreen();   // system bars return after the UserFS screen
    }

    @Override
    protected void onDestroy() {
        if (midi != null) midi.close();
        nativeStop();
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // Gaining focus restores the system bars; hide them again here.
        if (hasFocus) goFullscreen();
    }

    /**
     * Hides the system bars through SDL, off the UI thread: the call waits for
     * the surface's new size.
     */
    private void goFullscreen() {
        Thread t = new Thread(() -> setWindowStyle(true), "tracker-fullscreen");
        t.setDaemon(true);
        t.start();
    }

    /** Stops the firmware child (the native side owns its pid). */
    private static native void nativeStop();

    /** The display density, in pixels per dp; the panel's top bar is 48dp. */
    private static native void nativeUiScale(float density);
}
