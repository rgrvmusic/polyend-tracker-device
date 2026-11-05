// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
package nl.rgrv.ptpocket;

import android.content.Context;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.IOException;

/**
 * MIDI through Android's MidiManager, bridged to the child's descriptor. A
 * device's output port is listened to for input; its input port is written to
 * for output.
 */
public class MidiHandler {
    private static final String TAG = "Tracker-MIDI";
    static {
        System.loadLibrary("main");
    }

    public static native void nativeMidiIn(byte[] data);
    public static native byte[] nativeMidiOut();

    private final MidiManager manager;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final MidiManager.DeviceCallback deviceCallback = new MidiManager.DeviceCallback() {
        @Override
        public void onDeviceAdded(MidiDeviceInfo info) {
            openFirst();
        }

        @Override
        public void onDeviceRemoved(MidiDeviceInfo info) {
            closeDevice();
            openFirst();
        }
    };
    private MidiDevice device;
    private MidiOutputPort output;
    private MidiInputPort input;
    private Thread pump;
    private volatile boolean running = true;

    public MidiHandler(Context context) {
        manager = (MidiManager) context.getSystemService(Context.MIDI_SERVICE);
        if (manager == null) {
            Log.w(TAG, "no MIDI service");
            return;
        }
        manager.registerDeviceCallback(deviceCallback, handler);
        openFirst();
        pump = new Thread(this::pumpOut, "tracker-midi-out");
        pump.start();
    }

    private void openFirst() {
        handler.post(() -> {
            if (device != null) return;
            MidiDeviceInfo[] infos = manager.getDevices();
            for (MidiDeviceInfo info : infos) {
                if (info.getInputPortCount() == 0 && info.getOutputPortCount() == 0) continue;
                manager.openDevice(info, opened -> {
                    if (opened == null) return;
                    device = opened;
                    try {
                        if (info.getOutputPortCount() > 0) {
                            output = opened.openOutputPort(0);
                            if (output != null) {
                                output.connect(new MidiReceiver() {
                                    @Override
                                    public void onSend(byte[] msg, int offset, int count, long ts) {
                                        byte[] copy = new byte[count];
                                        System.arraycopy(msg, offset, copy, 0, count);
                                        nativeMidiIn(copy);
                                    }
                                });
                            }
                        }
                        if (info.getInputPortCount() > 0) {
                            input = opened.openInputPort(0);
                        }
                    } catch (Exception e) {
                        // The platform implementation throws IOException.
                        Log.w(TAG, "cannot open ports: " + e);
                    }
                }, handler);
                break;
            }
        });
    }

    private void pumpOut() {
        while (running) {
            try {
                byte[] data = nativeMidiOut();
                MidiInputPort port = input;
                if (data != null && data.length > 0 && port != null) {
                    port.send(data, 0, data.length);
                }
                Thread.sleep(5);
            } catch (InterruptedException e) {
                return;
            } catch (Exception e) {
                Log.w(TAG, "midi out: " + e);
            }
        }
    }

    private void closeDevice() {
        try {
            if (output != null) output.close();
        } catch (IOException ignored) {
        }
        try {
            if (input != null) input.close();
        } catch (IOException ignored) {
        }
        try {
            if (device != null) device.close();
        } catch (IOException ignored) {
        }
        output = null;
        input = null;
        device = null;
    }

    public void close() {
        running = false;
        closeDevice();
        if (manager != null) manager.unregisterDeviceCallback(deviceCallback);
    }
}
