package com.jawaker.engine

import android.app.Application
import android.content.Context
import android.content.pm.PackageInfo
import android.os.Build
import android.util.Log
import dalvik.system.DexClassLoader
import top.niunaijun.blackbox.BlackBoxCore
import top.niunaijun.blackbox.core.env.BEnvironment
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.lang.reflect.InvocationTargetException
import java.security.MessageDigest
import java.util.zip.ZipFile

/**
 * Prepares the unmodified Jawaker runtime and loads the GOAT engine beside it.
 *
 * The original implementation replaced libil2cpp.so and ignored split APK
 * native libraries. This manager instead keeps every game library original,
 * accepts only the verified 28.2.90 build, then adds the isolated GOAT native
 * runtime. libc++_shared is staged explicitly because the engine is loaded in
 * the guest linker namespace, which cannot see the loader's dependencies.
 */
object EngineLibManager {

    private const val TAG = "GOAT_LOADER"

    const val TARGET_PACKAGE = "com.boundless.jawaker"
    const val VIRTUAL_USER_ID = 0
    const val REQUIRED_VERSION_NAME = "28.2.90"
    const val REQUIRED_VERSION_CODE = 28290L

    const val OVERLAY_CLASS = "com.google.firebase.TrainerOverlayView"
    const val UNITY_ACTIVITY = "com.google.firebase.MessagingUnityPlayerActivity"

    private const val ABI_PREFIX = "lib/arm64-v8a/"
    private const val RUNTIME_MARKER = ".goat-original-runtime-v2"
    private const val BRIDGE_ASSET_NAME = "goat_bridge.jar"
    private const val BRIDGE_FILE_NAME = "goat_bridge.jar"
    private const val PERFORMANCE_PREFS = "goat_performance_v1"
    private const val PERFORMANCE_PROFILE = "profile"

    private val originalRuntimeRequired = arrayOf(
        "libmain.so",
        "libunity.so",
        "libil2cpp.so"
    )

    private val engineLibraries = arrayOf(
        "libc++_shared.so",
        "libdobby.so",
        "libloading_compat.so",
        "libengine.so"
    )

    @Volatile
    private var guestLibrariesLoaded = false

    @Volatile
    private var isolatedOverlayClass: Class<*>? = null

    data class PerformanceConfig(
        val id: Int,
        val name: String,
        val sliceNodes: Int,
        val sliceMs: Int,
        val arenaNodes: Int,
        val cardSafetyMs: Int,
        val talonSafetyMs: Int
    )

    private val performanceProfiles = arrayOf(
        PerformanceConfig(0, "SAFE", 900, 4, 16_000, 45, 25),
        PerformanceConfig(1, "BALANCED", 1_500, 5, 22_000, 30, 16),
        PerformanceConfig(2, "ULTRA", 2_200, 7, 28_000, 24, 12),
        // PC emulators running the official ARM64 game through translation have
        // more sustained CPU/memory headroom, so they receive wider slices but
        // retain the exact same ACK and legal-move validation.
        PerformanceConfig(3, "PC TURBO", 4_800, 11, 60_000, 10, 6)
    )

    fun performanceConfig(context: Context): PerformanceConfig {
        val defaultId = if (BuildConfig.EMULATOR_PROFILE) 3 else 2
        val requested = context.getSharedPreferences(PERFORMANCE_PREFS, Context.MODE_PRIVATE)
            .getInt(PERFORMANCE_PROFILE, defaultId)
        return performanceProfiles.firstOrNull { it.id == requested }
            ?: performanceProfiles[defaultId]
    }

    fun setPerformanceProfile(context: Context, profileId: Int): PerformanceConfig {
        val selected = performanceProfiles.firstOrNull { it.id == profileId }
            ?: performanceProfiles[if (BuildConfig.EMULATOR_PROFILE) 3 else 2]
        context.getSharedPreferences(PERFORMANCE_PREFS, Context.MODE_PRIVATE)
            .edit()
            .putInt(PERFORMANCE_PROFILE, selected.id)
            .apply()
        return selected
    }

    data class SystemPackage(
        val versionName: String,
        val versionCode: Long,
        val baseApk: File,
        val splitApks: List<File>,
        val systemNativeLibraryDir: File?
    ) {
        val allApks: List<File>
            get() = listOf(baseApk) + splitApks
    }

    data class PreparationResult(
        val success: Boolean,
        val message: String,
        val originalLibraryCount: Int = 0,
        val originalRuntimeReady: Boolean = false,
        val engineReady: Boolean = false
    )

    data class GuestLoadResult(
        val success: Boolean,
        val dobbyLoaded: Boolean,
        val engineLoaded: Boolean,
        val message: String
    )

    /** Reads the real, system-installed package. */
    fun getSystemPackage(context: Context): SystemPackage? {
        return try {
            val info = context.packageManager.getPackageInfo(TARGET_PACKAGE, 0)
            val appInfo = info.applicationInfo ?: return null
            val base = File(appInfo.sourceDir ?: return null)
            val splits = appInfo.splitSourceDirs
                ?.map { File(it) }
                ?.filter { it.isFile }
                ?: emptyList()

            SystemPackage(
                info.versionName ?: "unknown",
                packageVersionCode(info),
                base,
                splits,
                appInfo.nativeLibraryDir?.let { File(it) }
            )
        } catch (error: Throwable) {
            Log.w(TAG, "System package is not available", error)
            null
        }
    }

    fun isSupported(systemPackage: SystemPackage?): Boolean {
        return systemPackage != null &&
            systemPackage.versionName == REQUIRED_VERSION_NAME &&
            systemPackage.versionCode == REQUIRED_VERSION_CODE
    }

    fun unsupportedVersionMessage(systemPackage: SystemPackage?): String {
        if (systemPackage == null) {
            return "Jawaker is not installed on the device"
        }
        return "Unsupported Jawaker ${systemPackage.versionName} (${systemPackage.versionCode}); " +
            "required $REQUIRED_VERSION_NAME ($REQUIRED_VERSION_CODE)"
    }

    /**
     * Copies the exact original native runtime plus the two GOAT libraries to
     * BlackBox's app lib directory. Original files are never patched.
     */
    @Synchronized
    fun prepareVirtualRuntime(context: Context): PreparationResult {
        val integrity = LoaderSecurity.verify(context, verifyEngine = true)
        if (!integrity.trusted) {
            return PreparationResult(false, "Loader integrity rejected (${integrity.code})")
        }
        val systemPackage = getSystemPackage(context)
        if (!isSupported(systemPackage)) {
            return PreparationResult(false, unsupportedVersionMessage(systemPackage))
        }
        systemPackage ?: return PreparationResult(false, "Jawaker package information unavailable")

        val virtualLibDir = BEnvironment.getAppLibDir(TARGET_PACKAGE)
        if (!virtualLibDir.exists() && !virtualLibDir.mkdirs()) {
            return PreparationResult(false, "Cannot create virtual native library directory")
        }

        return try {
            val entries = collectArm64Libraries(systemPackage.allApks)
            val missingOriginal = originalRuntimeRequired.filter { !entries.containsKey(it) }
            if (missingOriginal.isNotEmpty()) {
                return PreparationResult(
                    false,
                    "Original split is incomplete: ${missingOriginal.joinToString()}"
                )
            }

            val fingerprint = runtimeFingerprint(systemPackage, entries)
            val marker = File(virtualLibDir, RUNTIME_MARKER)
            val markerMatches = marker.isFile && marker.readText() == fingerprint
            var originalReady = markerMatches && verifyEntries(virtualLibDir, entries)

            if (!originalReady) {
                Log.i(TAG, "Extracting ${entries.size} original arm64 libraries")
                for ((name, source) in entries) {
                    extractEntryAtomically(source.apk, source.entryName, File(virtualLibDir, name))
                }
                originalReady = verifyEntries(virtualLibDir, entries)
                if (originalReady) {
                    writeTextAtomically(marker, fingerprint)
                }
            }

            val engineReady = copyEngineLibraries(context, virtualLibDir)
            val bridgeReady = copyGuestBridge(context)
            val requiredReady = originalRuntimeRequired.all {
                val file = File(virtualLibDir, it)
                file.isFile && file.length() > 0L
            }

            if (!originalReady || !requiredReady || !engineReady || !bridgeReady) {
                PreparationResult(
                    false,
                    "Runtime verification failed",
                    entries.size,
                    originalReady && requiredReady,
                    engineReady && bridgeReady
                )
            } else {
                Log.i(
                    TAG,
                    "Runtime ready: original=${entries.size}, engine=${engineLibraries.joinToString()}"
                )
                PreparationResult(
                    true,
                    "Original Jawaker runtime and GOAT engine are ready",
                    entries.size,
                    true,
                    true
                )
            }
        } catch (error: Throwable) {
            Log.e(TAG, "Runtime preparation failed", error)
            PreparationResult(false, error.message ?: error.javaClass.simpleName)
        }
    }

    /**
     * Runs before the guest Application class loader is materialized.
     * Prepares native paths before LoadedApk materializes the guest. The Java
     * bridge itself is loaded later by a dedicated child-first DexClassLoader;
     * it must not be added as a shared library because Android may cache shared
     * library metadata before this callback runs.
     */
    fun configureGuestApplicationInfo(context: Context): Boolean {
        if (context.packageName != TARGET_PACKAGE) return false
        if (!LoaderSecurity.verify(BlackBoxCore.getContext(), verifyEngine = true).trusted) {
            Log.e(TAG, "Guest configuration refused: loader integrity failed")
            return false
        }

        return try {
            val virtualLibDir = BEnvironment.getAppLibDir(TARGET_PACKAGE)
            if (!verifyRuntimeFiles(virtualLibDir)) {
                Log.e(TAG, "Guest configuration refused: prepared runtime is incomplete")
                return false
            }

            val applicationInfo = context.applicationInfo
            applicationInfo.nativeLibraryDir = virtualLibDir.absolutePath

            val bridgeJar = guestBridgeFile()
            if (!bridgeJar.isFile || bridgeJar.length() <= 0L) {
                Log.e(TAG, "Guest configuration refused: bridge jar is missing")
                return false
            }
            Log.i(TAG, "Guest nativeLibraryDir=${applicationInfo.nativeLibraryDir}")
            Log.i(TAG, "Guest isolated bridge ready at ${bridgeJar.absolutePath}")
            true
        } catch (error: Throwable) {
            Log.e(TAG, "Could not configure guest ApplicationInfo", error)
            false
        }
    }

    /** Loads Dobby and the engine through the guest class loader namespace. */
    @Synchronized
    fun loadEngineInGuest(application: Application): GuestLoadResult {
        if (!LoaderSecurity.verify(BlackBoxCore.getContext(), verifyEngine = true).trusted) {
            return GuestLoadResult(false, false, false, "Loader integrity rejected")
        }
        if (guestLibrariesLoaded) {
            return GuestLoadResult(true, true, true, "GOAT engine already loaded")
        }

        val virtualLibDir = BEnvironment.getAppLibDir(TARGET_PACKAGE)
        if (!verifyRuntimeFiles(virtualLibDir)) {
            return GuestLoadResult(false, false, false, "Prepared runtime is incomplete")
        }

        return try {
            val guestClassLoader = application.javaClass.classLoader
                ?: throw IllegalStateException("Guest class loader is null")
            val overlayClass = getOverlayClass(guestClassLoader)

            // Set this on the guest copy of TrainerOverlayView. The native
            // license layer verifies the signed loader, not the official game.
            val setHostContext = overlayClass.getDeclaredMethod(
                "setHostContext",
                Context::class.java
            )
            setHostContext.invoke(null, BlackBoxCore.getContext().applicationContext)

            // Load the C++ runtime through the exact same class-loader owner as
            // libengine. Merely placing it in the host APK is insufficient on
            // Android's isolated linker namespaces.
            runtimeLoadFromClass(overlayClass, File(virtualLibDir, "libc++_shared.so"))
            runtimeLoadFromClass(overlayClass, File(virtualLibDir, "libdobby.so"))
            runtimeLoadFromClass(overlayClass, File(virtualLibDir, "libloading_compat.so"))
            runtimeLoadFromClass(overlayClass, File(virtualLibDir, "libengine.so"))

            val performance = performanceConfig(BlackBoxCore.getContext())
            try {
                val applyPerformance = overlayClass.getDeclaredMethod(
                    "nativeSetPerformanceProfile",
                    Integer.TYPE, Integer.TYPE, Integer.TYPE,
                    Integer.TYPE, Integer.TYPE, Integer.TYPE
                )
                applyPerformance.invoke(
                    null,
                    performance.id,
                    performance.sliceNodes,
                    performance.sliceMs,
                    performance.arenaNodes,
                    performance.cardSafetyMs,
                    performance.talonSafetyMs
                )
                Log.i(TAG, "Performance v1 applied: $performance")
            } catch (error: Throwable) {
                // The native engine has safe ULTRA defaults; a profile-channel
                // failure must not turn a valid licensed launch into a crash.
                Log.w(TAG, "Performance profile channel unavailable; native defaults kept", unwrap(error))
            }

            guestLibrariesLoaded = true
            Log.i(TAG, "GOAT engine loaded in guest namespace")
            GuestLoadResult(true, true, true, "GOAT engine loaded in guest namespace")
        } catch (error: Throwable) {
            val cause = unwrap(error)
            Log.e(TAG, "Guest native load failed", cause)
            GuestLoadResult(
                false,
                false,
                false,
                cause.message ?: cause.javaClass.simpleName
            )
        }
    }

    fun verifyLibsInVirtualEnv(): Boolean {
        return verifyRuntimeFiles(BEnvironment.getAppLibDir(TARGET_PACKAGE))
    }

    /** Returns only the bridge class, never the loader APK's dependency graph. */
    @Synchronized
    fun getOverlayClass(guestClassLoader: ClassLoader): Class<*> {
        isolatedOverlayClass?.let { return it }

        val bridgeJar = guestBridgeFile()
        if (!bridgeJar.isFile || bridgeJar.length() <= 0L) {
            throw IllegalStateException("Isolated bridge jar is missing")
        }

        val optimizedDir = File(bridgeJar.parentFile, "goat_bridge_opt")
        if (!optimizedDir.exists() && !optimizedDir.mkdirs()) {
            throw IllegalStateException("Cannot create bridge optimization directory")
        }

        val loader = BridgeDexClassLoader(
            bridgeJar.absolutePath,
            optimizedDir.absolutePath,
            guestClassLoader
        )
        val loaded = loader.loadClass(OVERLAY_CLASS)
        isolatedOverlayClass = loaded
        Log.i(TAG, "Isolated bridge class loaded by ${loaded.classLoader}")
        return loaded
    }

    /** Ensures the bridge class itself wins over any same-named parent class. */
    private class BridgeDexClassLoader(
        dexPath: String,
        optimizedDirectory: String,
        parent: ClassLoader
    ) : DexClassLoader(dexPath, optimizedDirectory, null, parent) {

        override fun loadClass(name: String, resolve: Boolean): Class<*> {
            if (name != OVERLAY_CLASS) return super.loadClass(name, resolve)

            // Older Android/Java stubs do not expose ClassLoader#getClassLoadingLock
            // to Kotlin. This loader owns only one child-first bridge class, so a
            // loader-wide monitor provides the same required serialization here.
            synchronized(this) {
                var loaded = findLoadedClass(name)
                if (loaded == null) loaded = findClass(name)
                if (resolve) resolveClass(loaded)
                return loaded
            }
        }
    }

    private data class NativeEntry(
        val apk: File,
        val entryName: String,
        val size: Long,
        val crc: Long
    )

    private fun collectArm64Libraries(apks: List<File>): Map<String, NativeEntry> {
        val result = linkedMapOf<String, NativeEntry>()
        for (apk in apks) {
            if (!apk.isFile) continue
            ZipFile(apk).use { zip ->
                val entries = zip.entries()
                while (entries.hasMoreElements()) {
                    val entry = entries.nextElement()
                    if (entry.isDirectory || !entry.name.startsWith(ABI_PREFIX)) continue
                    val name = entry.name.substring(ABI_PREFIX.length)
                    if (name.isEmpty() || name.contains('/') || !name.endsWith(".so")) continue
                    result[name] = NativeEntry(apk, entry.name, entry.size, entry.crc)
                }
            }
        }
        return result
    }

    private fun runtimeFingerprint(
        systemPackage: SystemPackage,
        entries: Map<String, NativeEntry>
    ): String {
        val builder = StringBuilder()
        builder.append(systemPackage.versionName)
            .append('|')
            .append(systemPackage.versionCode)
            .append('\n')
        for (apk in systemPackage.allApks) {
            builder.append(apk.absolutePath)
                .append('|')
                .append(apk.length())
                .append('|')
                .append(apk.lastModified())
                .append('\n')
        }
        for ((name, source) in entries.toSortedMap()) {
            builder.append(name)
                .append('|')
                .append(source.size)
                .append('|')
                .append(source.crc)
                .append('\n')
        }
        return builder.toString()
    }

    private fun verifyEntries(directory: File, entries: Map<String, NativeEntry>): Boolean {
        for ((name, source) in entries) {
            val file = File(directory, name)
            if (!file.isFile || file.length() != source.size || source.size <= 0L) return false
        }
        return true
    }

    private fun verifyRuntimeFiles(directory: File): Boolean {
        return originalRuntimeRequired.all {
            val file = File(directory, it)
            file.isFile && file.length() > 0L
        } && engineLibraries.all {
            val file = File(directory, it)
            file.isFile && file.length() > 0L
        } && LoaderSecurity.verifyEngineLibrary(File(directory, "libengine.so")) &&
            guestBridgeFile().let { it.isFile && it.length() > 0L }
    }

    private fun guestBridgeFile(): File {
        val guestCodeCache = File(
            BEnvironment.getDataDir(TARGET_PACKAGE, VIRTUAL_USER_ID),
            "code_cache"
        )
        return File(guestCodeCache, BRIDGE_FILE_NAME)
    }

    private fun copyGuestBridge(context: Context): Boolean {
        val destination = guestBridgeFile()
        val destinationDir = destination.parentFile ?: return false
        if (!destinationDir.exists() && !destinationDir.mkdirs()) return false
        val temporary = File(destinationDir, "$BRIDGE_FILE_NAME.tmp")
        return try {
            context.assets.open(BRIDGE_ASSET_NAME).use { input ->
                FileOutputStream(temporary).use { output ->
                    input.copyTo(output, 32 * 1024)
                    output.fd.sync()
                }
            }
            if (temporary.length() <= 0L) {
                temporary.delete()
                false
            } else {
                replaceAtomically(temporary, destination)
                destination.setWritable(false, false)
                true
            }
        } catch (error: Throwable) {
            temporary.delete()
            Log.e(TAG, "Could not stage isolated guest bridge", error)
            false
        }
    }

    private fun extractEntryAtomically(apk: File, entryName: String, destination: File) {
        ZipFile(apk).use { zip ->
            val entry = zip.getEntry(entryName)
                ?: throw IllegalStateException("Missing $entryName in ${apk.name}")
            val temporary = File(destination.parentFile, destination.name + ".tmp")
            zip.getInputStream(entry).use { input ->
                FileOutputStream(temporary).use { output ->
                    input.copyTo(output, 64 * 1024)
                    output.fd.sync()
                }
            }
            if (entry.size > 0L && temporary.length() != entry.size) {
                temporary.delete()
                throw IllegalStateException("Short extraction for ${destination.name}")
            }
            replaceAtomically(temporary, destination)
        }
    }

    private fun copyEngineLibraries(context: Context, destinationDir: File): Boolean {
        val hostNativeDir = File(context.applicationInfo.nativeLibraryDir)
        for (name in engineLibraries) {
            val source = File(hostNativeDir, name)
            if (!source.isFile || source.length() <= 0L) {
                Log.e(TAG, "Loader APK is missing $name")
                return false
            }
            if (name == "libengine.so" && !LoaderSecurity.verifyEngineLibrary(source)) {
                Log.e(TAG, "Loader engine fingerprint rejected")
                return false
            }
            val destination = File(destinationDir, name)
            if (!sameFileContent(source, destination)) {
                copyFileAtomically(source, destination)
            }
            destination.setWritable(false, false)
        }
        return LoaderSecurity.verifyEngineLibrary(File(destinationDir, "libengine.so"))
    }

    private fun copyFileAtomically(source: File, destination: File) {
        val temporary = File(destination.parentFile, destination.name + ".tmp")
        FileInputStream(source).use { input ->
            FileOutputStream(temporary).use { output ->
                input.copyTo(output, 64 * 1024)
                output.fd.sync()
            }
        }
        if (temporary.length() != source.length()) {
            temporary.delete()
            throw IllegalStateException("Short copy for ${source.name}")
        }
        replaceAtomically(temporary, destination)
    }

    private fun replaceAtomically(temporary: File, destination: File) {
        destination.parentFile?.mkdirs()
        if (destination.exists() && !destination.delete()) {
            temporary.delete()
            throw IllegalStateException("Cannot replace ${destination.absolutePath}")
        }
        if (!temporary.renameTo(destination)) {
            temporary.delete()
            throw IllegalStateException("Cannot commit ${destination.absolutePath}")
        }
        destination.setReadable(true, false)
        destination.setExecutable(true, false)
    }

    private fun writeTextAtomically(destination: File, value: String) {
        val temporary = File(destination.parentFile, destination.name + ".tmp")
        FileOutputStream(temporary).use { output ->
            output.write(value.toByteArray(Charsets.UTF_8))
            output.fd.sync()
        }
        replaceAtomically(temporary, destination)
    }

    private fun sameFileContent(first: File, second: File): Boolean {
        if (!first.isFile || !second.isFile || first.length() != second.length()) return false
        return sha256(first).contentEquals(sha256(second))
    }

    private fun sha256(file: File): ByteArray {
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                if (count > 0) digest.update(buffer, 0, count)
            }
        }
        return digest.digest()
    }

    /** Namespace-safe equivalent of System.load(path). */
    private fun runtimeLoadFromClass(ownerClass: Class<*>, library: File) {
        if (!library.isFile) throw IllegalStateException("Missing ${library.name}")
        if (library.name == "libengine.so" && !LoaderSecurity.verifyEngineLibrary(library)) {
            throw SecurityException("Engine integrity rejected")
        }

        val runtime = Runtime.getRuntime()
        val methods = Runtime::class.java.declaredMethods
            .filter { it.name == "load0" && it.parameterTypes.size == 2 }

        var lastError: Throwable? = null
        for (method in methods) {
            val parameters = method.parameterTypes
            if (parameters[1] != String::class.java) continue
            val owner: Any = when (parameters[0]) {
                Class::class.java -> ownerClass
                ClassLoader::class.java -> ownerClass.classLoader
                    ?: throw IllegalStateException("Overlay class loader is null")
                else -> continue
            }

            try {
                method.isAccessible = true
                method.invoke(runtime, owner, library.absolutePath)
                return
            } catch (error: Throwable) {
                lastError = unwrap(error)
            }
        }
        throw IllegalStateException(
            "Android Runtime.load0 failed for ${library.name}",
            lastError
        )
    }

    private fun unwrap(error: Throwable): Throwable {
        var current = error
        while (current is InvocationTargetException && current.targetException != null) {
            current = current.targetException
        }
        return current
    }

    private fun packageVersionCode(info: PackageInfo): Long {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            info.longVersionCode
        } else {
            @Suppress("DEPRECATION")
            info.versionCode.toLong()
        }
    }
}
