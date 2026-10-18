package com.phonecastvr.sender;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

/**
 * Manages discrete high-contrast micro-markers at the 4 extreme corners of the
 * display to ensure optical edge detection even in Dark Mode or under uneven lighting.
 */
final class CornerFiducialsController {
    private static final String TAG = "PhoneCastFiducials";
    private final Context context;
    private final WindowManager windowManager;
    private View overlayView;
    private boolean active;

    CornerFiducialsController(Context context) {
        this.context = context.getApplicationContext();
        this.windowManager = (WindowManager) this.context.getSystemService(Context.WINDOW_SERVICE);
    }

    boolean isSupported() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(context);
    }

    void setActive(boolean enable) {
        if (active == enable) return;
        active = enable;
        if (enable) {
            show();
        } else {
            hide();
        }
    }

    private void show() {
        if (overlayView != null) return;
        if (!isSupported()) {
            Log.d(TAG, "SYSTEM_ALERT_WINDOW not granted, skipping discrete fiducials view");
            return;
        }

        try {
            overlayView = new View(context) {
                private final Paint whitePaint = new Paint();
                private final Paint blackPaint = new Paint();
                {
                    whitePaint.setColor(Color.WHITE);
                    whitePaint.setStyle(Paint.Style.FILL);
                    whitePaint.setAntiAlias(false);

                    blackPaint.setColor(Color.BLACK);
                    blackPaint.setStyle(Paint.Style.FILL);
                    blackPaint.setAntiAlias(false);
                }

                @Override
                protected void onDraw(Canvas canvas) {
                    super.onDraw(canvas);
                    final int w = getWidth();
                    final int h = getHeight();
                    if (w <= 0 || h <= 0) return;

                    // 16x16 px high-contrast corner markers: outer black 16x16, inner white 12x12
                    final int size = 16;
                    final int inset = 2;

                    // Top-Left
                    canvas.drawRect(0, 0, size, size, blackPaint);
                    canvas.drawRect(inset, inset, size - inset, size - inset, whitePaint);

                    // Top-Right
                    canvas.drawRect(w - size, 0, w, size, blackPaint);
                    canvas.drawRect(w - size + inset, inset, w - inset, size - inset, whitePaint);

                    // Bottom-Right
                    canvas.drawRect(w - size, h - size, w, h, blackPaint);
                    canvas.drawRect(w - size + inset, h - size + inset, w - inset, h - inset, whitePaint);

                    // Bottom-Left
                    canvas.drawRect(0, h - size, size, h, blackPaint);
                    canvas.drawRect(inset, h - size + inset, size - inset, h - inset, whitePaint);
                }
            };

            int windowType = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_SYSTEM_OVERLAY;

            int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                    | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;

            WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.MATCH_PARENT,
                    WindowManager.LayoutParams.MATCH_PARENT,
                    windowType,
                    flags,
                    PixelFormat.TRANSLUCENT);

            windowManager.addView(overlayView, params);
            Log.i(TAG, "Discrete corner fiducials overlay activated");
        } catch (Exception e) {
            Log.w(TAG, "Failed to attach corner fiducials overlay", e);
            overlayView = null;
        }
    }

    private void hide() {
        if (overlayView != null) {
            try {
                windowManager.removeView(overlayView);
            } catch (Exception e) {
                Log.w(TAG, "Failed to remove corner fiducials overlay", e);
            }
            overlayView = null;
            Log.i(TAG, "Discrete corner fiducials overlay removed");
        }
    }
}
