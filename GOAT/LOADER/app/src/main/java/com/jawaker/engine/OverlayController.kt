package com.jawaker.engine

import android.app.Activity
import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import top.niunaijun.blackbox.BlackBoxCore
import java.util.WeakHashMap

/** Attaches the native GOAT panel to the unmodified Unity activity. */
object OverlayController {

    private const val TAG = "GOAT_OVERLAY"
    private val attachedActivities = WeakHashMap<Activity, View>()

    @Synchronized
    fun attachIfNeeded(activity: Activity): Boolean {
        if (activity.packageName != EngineLibManager.TARGET_PACKAGE) return false
        if (activity.javaClass.name != EngineLibManager.UNITY_ACTIVITY) return false
        if (attachedActivities.containsKey(activity)) return true

        return try {
            addLoaderAssets(activity)

            val guestClassLoader = activity.javaClass.classLoader
                ?: throw IllegalStateException("Activity class loader is null")
            val overlayClass = EngineLibManager.getOverlayClass(guestClassLoader)
            val constructor = overlayClass.getConstructor(Context::class.java)
            val overlay = constructor.newInstance(activity) as View

            val layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            activity.addContentView(overlay, layoutParams)

            val createPanel = overlayClass.getDeclaredMethod(
                "nativeCreatePanel",
                Activity::class.java,
                overlayClass
            )
            createPanel.invoke(null, activity, overlay)

            attachedActivities[activity] = overlay
            Log.i(TAG, "Overlay attached to ${activity.javaClass.name}")
            true
        } catch (error: Throwable) {
            Log.e(TAG, "Overlay attach failed", error)
            false
        }
    }

    @Synchronized
    fun onActivityDestroyed(activity: Activity) {
        attachedActivities.remove(activity)
    }

    /** Makes goat_plate.png from the loader APK visible to Activity.getAssets(). */
    private fun addLoaderAssets(activity: Activity) {
        try {
            val hostApk = BlackBoxCore.getContext().applicationInfo.sourceDir
            val method = AssetManager::class.java.getDeclaredMethod(
                "addAssetPath",
                String::class.java
            )
            method.isAccessible = true
            val cookie = method.invoke(activity.assets, hostApk) as? Int ?: 0
            if (cookie == 0) {
                Log.w(TAG, "AssetManager rejected loader APK path")
            } else {
                Log.i(TAG, "Loader assets mounted with cookie=$cookie")
            }
        } catch (error: Throwable) {
            // The panel remains functional if only the decorative plate fails.
            Log.w(TAG, "Could not add loader asset path", error)
        }
    }
}
