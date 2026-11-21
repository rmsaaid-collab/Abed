package com.jawaker.engine

import android.app.Activity
import android.app.Application
import android.content.Context
import android.os.Bundle
import android.util.Log
import top.niunaijun.blackbox.BlackBoxCore
import top.niunaijun.blackbox.app.configuration.AppLifecycleCallback
import top.niunaijun.blackbox.app.configuration.ClientConfiguration
import java.io.File

/** Host application and the lifecycle bridge into the BlackBox guest process. */
class MainApplication : Application() {

    companion object {
        private const val TAG = "GOAT_CONTAINER"

        @Volatile
        private var hostContext: Context? = null

        fun getContext(): Context? = hostContext

        object InjectionStatus {
            @Volatile var dobbyLoaded = false
            @Volatile var il2cppLoaded = false
            @Volatile var ggengineLoaded = false
            @Volatile var redirectActive = false
            @Volatile var engineInitialized = false
            @Volatile var libsCopied = false
            @Volatile var securityReady = false
            @Volatile var securityCode = "S0"

            val allLoaded: Boolean
                get() = dobbyLoaded && il2cppLoaded && ggengineLoaded

            fun resetRuntime() {
                dobbyLoaded = false
                il2cppLoaded = false
                ggengineLoaded = false
                redirectActive = false
                libsCopied = false
            }
        }

        /** Kept for the existing MainActivity UI. */
        val injectionStatus: InjectionStatus
            get() = InjectionStatus
    }

    override fun attachBaseContext(base: Context) {
        super.attachBaseContext(base)
        hostContext = base.applicationContext

        val integrity = LoaderSecurity.verify(base, verifyEngine = true)
        InjectionStatus.securityReady = integrity.trusted
        InjectionStatus.securityCode = integrity.code
        if (!integrity.trusted) {
            InjectionStatus.engineInitialized = false
            Log.e(TAG, "Loader integrity rejected: ${integrity.code}")
            return
        }

        try {
            BlackBoxCore.get().doAttachBaseContext(base, JawakerClientConfiguration())
            BlackBoxCore.get().addAppLifecycleCallback(JawakerLifecycleCallback())
            Log.i(TAG, "BlackBox attach complete; process=${currentProcessName()}")
        } catch (error: Throwable) {
            InjectionStatus.engineInitialized = false
            Log.e(TAG, "BlackBox attach failed", error)
        }
    }

    override fun onCreate() {
        super.onCreate()
        if (!InjectionStatus.securityReady) return
        try {
            BlackBoxCore.get().doCreate()
            InjectionStatus.engineInitialized = true
            Log.i(TAG, "BlackBox create complete; main=${BlackBoxCore.get().isMainProcess}")
        } catch (error: Throwable) {
            InjectionStatus.engineInitialized = false
            Log.e(TAG, "BlackBox create failed", error)
        }
    }

    private class JawakerClientConfiguration : ClientConfiguration() {
        override fun getHostPackageName(): String = "com.jawaker.engine"

        override fun isHideRoot(): Boolean = true

        override fun isHideXposed(): Boolean = true

        override fun isEnableDaemonService(): Boolean = true

        override fun isEnableLauncherActivity(): Boolean = false

        override fun requestInstallPackage(file: File, userId: Int): Boolean = false
    }

    /**
     * These callbacks execute inside the virtual Jawaker process.  No game APK
     * is modified: the original runtime is selected before class-loader
     * creation, then the engine is loaded beside it in the same namespace.
     */
    private class JawakerLifecycleCallback : AppLifecycleCallback() {

        override fun beforeCreateApplication(
            packageName: String,
            processName: String,
            context: Context,
            userId: Int
        ) {
            if (!isTargetMainProcess(packageName, processName)) return
            if (!LoaderSecurity.verify(BlackBoxCore.getContext(), verifyEngine = true).trusted) return

            val configured = EngineLibManager.configureGuestApplicationInfo(context)
            InjectionStatus.redirectActive = configured
            InjectionStatus.il2cppLoaded = configured
            Log.i(
                TAG,
                "[GUEST_CONFIG] process=$processName user=$userId configured=$configured"
            )
        }

        override fun beforeApplicationOnCreate(
            packageName: String,
            processName: String,
            application: Application,
            userId: Int
        ) {
            if (!isTargetMainProcess(packageName, processName)) return
            if (!LoaderSecurity.verify(BlackBoxCore.getContext(), verifyEngine = true).trusted) return

            val result = EngineLibManager.loadEngineInGuest(application)
            InjectionStatus.dobbyLoaded = result.dobbyLoaded
            InjectionStatus.ggengineLoaded = result.engineLoaded
            InjectionStatus.libsCopied = result.success
            Log.i(
                TAG,
                "[GUEST_LOAD] success=${result.success} message=${result.message}"
            )
        }

        override fun afterApplicationOnCreate(
            packageName: String,
            processName: String,
            application: Application,
            userId: Int
        ) {
            if (!isTargetMainProcess(packageName, processName)) return
            Log.i(TAG, "[GUEST_READY] application=${application.javaClass.name}")
        }

        override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {
            if (!InjectionStatus.securityReady) return
            OverlayController.attachIfNeeded(activity)
        }

        override fun onActivityResumed(activity: Activity) {
            if (!InjectionStatus.securityReady) return
            // Fallback for devices where Unity replaces/recreates its Activity.
            OverlayController.attachIfNeeded(activity)
        }

        override fun onActivityDestroyed(activity: Activity) {
            OverlayController.onActivityDestroyed(activity)
        }

        private fun isTargetMainProcess(packageName: String, processName: String): Boolean {
            return packageName == EngineLibManager.TARGET_PACKAGE &&
                processName == EngineLibManager.TARGET_PACKAGE
        }
    }

    private fun currentProcessName(): String {
        return try {
            val pid = android.os.Process.myPid()
            val manager = getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
            manager.runningAppProcesses
                ?.firstOrNull { it.pid == pid }
                ?.processName
                ?: "unknown"
        } catch (_: Throwable) {
            "unknown"
        }
    }
}
