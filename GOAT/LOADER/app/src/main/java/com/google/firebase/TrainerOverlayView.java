package com.google.firebase;

import android.content.Context;
import android.graphics.Canvas;
import android.view.MotionEvent;
import android.view.View;
import android.widget.SeekBar;

/**
 * Java bridge expected by libengine.so.
 *
 * The class is shipped by the loader and exposed to Jawaker's guest class
 * loader at runtime, so the official game APK and its signature stay intact.
 */
public final class TrainerOverlayView extends View implements
        View.OnClickListener,
        View.OnTouchListener,
        SeekBar.OnSeekBarChangeListener {

    public static volatile float[] cachedCoords;

    private static volatile Context hostContext;

    public TrainerOverlayView(Context context) {
        super(context);
        setWillNotDraw(false);
    }

    /** Called reflectively on the guest-loaded copy of this class. */
    public static void setHostContext(Context context) {
        hostContext = context == null ? null : context.getApplicationContext();
    }

    /** Used by the native license layer to verify the loader certificate. */
    public static Context getHostContext() {
        return hostContext;
    }

    public static native void nativeOnDraw(View view, Canvas canvas, float[] coordinates);

    public static native void nativeOnClick(int viewId);

    public static native boolean nativeOnTouch(int viewId, float rawX, float rawY, int action);

    public static native void nativeOnForceChanged(int progress);

    /** Version 1 performance channel; all values are clamped again in C++. */
    public static native void nativeSetPerformanceProfile(
            int profile,
            int sliceNodes,
            int sliceMs,
            int arenaNodes,
            int cardSafetyMs,
            int talonSafetyMs
    );

    public static native void nativeCreatePanel(
            android.app.Activity activity,
            TrainerOverlayView listenerView
    );

    @Override
    public void onClick(View view) {
        nativeOnClick(view.getId());
    }

    @Override
    public boolean onTouch(View view, MotionEvent event) {
        return nativeOnTouch(
                view.getId(),
                event.getRawX(),
                event.getRawY(),
                event.getAction()
        );
    }

    @Override
    public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
        nativeOnForceChanged(progress);
    }

    @Override
    public void onStartTrackingTouch(SeekBar seekBar) {
        // Native code only needs progress updates.
    }

    @Override
    public void onStopTrackingTouch(SeekBar seekBar) {
        // Native code only needs progress updates.
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float[] coordinates = cachedCoords;
        if (coordinates == null) coordinates = new float[0];
        nativeOnDraw(this, canvas, coordinates);
    }
}
