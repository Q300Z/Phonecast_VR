package com.phonecastvr.sender;

import java.util.Arrays;

final class TrackingProtocol {
    static final int TYPE_SCREEN_METRICS = 15;
    static final int TYPE_PHONE_IMU = 16;
    private static final byte[] CAPABILITY = {'P', 'C', 'T', 'R', 1, 1, 0, 0};

    private TrackingProtocol() {}

    static byte[] capability() {
        return CAPABILITY.clone();
    }

    static boolean hasCapability(byte[] payload) {
        if (payload == null || payload.length < 8) return false;
        for (int i = 0; i + 8 <= payload.length; i += 8) {
            boolean match = true;
            for (int j = 0; j < 8; j++) {
                if (payload[i + j] != CAPABILITY[j]) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    static byte[] combineCapabilities(boolean audio, boolean tracking) {
        if (audio && tracking) {
            byte[] combined = new byte[16];
            byte[] audioCap = AudioProtocol.capability();
            byte[] trackCap = capability();
            System.arraycopy(audioCap, 0, combined, 0, 8);
            System.arraycopy(trackCap, 0, combined, 8, 8);
            return combined;
        } else if (audio) {
            return AudioProtocol.capability();
        } else if (tracking) {
            return capability();
        } else {
            return new byte[0];
        }
    }
}
