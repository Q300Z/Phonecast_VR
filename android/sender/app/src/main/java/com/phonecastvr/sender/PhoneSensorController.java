package com.phonecastvr.sender;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

final class PhoneSensorController implements SensorEventListener {
    private static final String TAG = "PhoneSensorController";

    interface Listener {
        void onImuSample(long timestampMicros, int sensorType, int accuracy, int flags,
                         float qx, float qy, float qz, float qw,
                         float gyroX, float gyroY, float gyroZ,
                         float accelX, float accelY, float accelZ);
    }

    private final SensorManager sensorManager;
    private final Listener listener;
    private HandlerThread handlerThread;
    private Handler handler;
    private Sensor sensor;
    private int sensorTypeEnum = 1;
    private boolean running;

    PhoneSensorController(Context context, Listener listener) {
        this.sensorManager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        this.listener = listener;
    }

    synchronized boolean start() {
        if (running) return true;
        if (sensorManager == null) {
            Log.w(TAG, "SensorManager unavailable");
            return false;
        }

        sensor = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
        if (sensor != null) {
            sensorTypeEnum = 1;
        } else {
            sensor = sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR);
            if (sensor != null) {
                sensorTypeEnum = 2;
            } else {
                Log.w(TAG, "Neither game rotation vector nor rotation vector sensor available");
                return false;
            }
        }

        handlerThread = new HandlerThread("phonecast-imu-sensor");
        handlerThread.start();
        handler = new Handler(handlerThread.getLooper());

        // Request 16666 microseconds (~60 Hz) sampling rate
        boolean registered = sensorManager.registerListener(this, sensor, 16666, handler);
        if (!registered) {
            Log.w(TAG, "Failed to register sensor listener");
            handlerThread.quitSafely();
            handlerThread = null;
            handler = null;
            return false;
        }

        running = true;
        Log.i(TAG, "PhoneSensorController started with sensor: " + sensor.getName());
        return true;
    }

    synchronized void stop() {
        if (!running) return;
        running = false;
        if (sensorManager != null) {
            sensorManager.unregisterListener(this);
        }
        if (handlerThread != null) {
            handlerThread.quitSafely();
            handlerThread = null;
            handler = null;
        }
        sensor = null;
        Log.i(TAG, "PhoneSensorController stopped");
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (!running || listener == null) return;

        // Zero-allocation extraction of primitives
        float qx = event.values[0];
        float qy = event.values[1];
        float qz = event.values[2];
        float qw;
        if (event.values.length >= 4) {
            qw = event.values[3];
        } else {
            float sumSq = qx * qx + qy * qy + qz * qz;
            qw = sumSq < 1.0f ? (float) Math.sqrt(1.0f - sumSq) : 0.0f;
        }

        long timestampMicros = event.timestamp / 1000L;
        int accuracy = event.accuracy;
        if (accuracy < 0 || accuracy > 3) accuracy = 0;

        listener.onImuSample(timestampMicros, sensorTypeEnum, accuracy, 0,
                qx, qy, qz, qw, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
        // No-op
    }
}
