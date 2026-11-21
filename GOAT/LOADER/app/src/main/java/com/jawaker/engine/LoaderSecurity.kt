package com.jawaker.engine

import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.os.Build
import android.os.Debug
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/**
 * Small, deterministic host-integrity gate. The native license layer performs
 * the same certificate check again inside the guest, so bypassing one layer is
 * not enough to authorize a modified loader.
 */
object LoaderSecurity {

    private const val EXPECTED_PACKAGE = "com.jawaker.engine"
    private const val EXPECTED_CERT_SHA256 =
        "69f33c009629337b0ec4178016c86bfdc0084100386565efe308369e1ea1bfba"
    private const val EXPECTED_ENGINE_SHA256 =
        "732934d77189886c63fd93affd69ee1645159bb7eae481835e39cb816b0d48a7"

    data class Result(val trusted: Boolean, val code: String)

    fun verify(context: Context, verifyEngine: Boolean): Result {
        // Debug builds remain usable for local engineering; the distributed
        // artifact is always a non-debuggable release build.
        if (BuildConfig.DEBUG) return Result(true, "DEV")
        if (context.packageName != EXPECTED_PACKAGE) return Result(false, "S1")
        if ((context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            return Result(false, "S2")
        }
        if (Debug.isDebuggerConnected() || Debug.waitingForDebugger()) {
            return Result(false, "S3")
        }
        if (!hasExpectedSigner(context)) return Result(false, "S4")
        if (verifyEngine) {
            val nativeDir = context.applicationInfo.nativeLibraryDir ?: return Result(false, "S5")
            if (!verifyEngineLibrary(File(nativeDir, "libengine.so"))) {
                return Result(false, "S5")
            }
        }
        return Result(true, "OK")
    }

    fun verifyEngineLibrary(file: File): Boolean {
        if (!file.isFile || file.length() <= 0L) return false
        val expected = hexToBytes(EXPECTED_ENGINE_SHA256) ?: return false
        return MessageDigest.isEqual(sha256(file), expected)
    }

    /** Exact immutable engine pin used by the signed server challenge. */
    fun expectedEngineSha256(): String = EXPECTED_ENGINE_SHA256

    @Suppress("DEPRECATION")
    private fun hasExpectedSigner(context: Context): Boolean {
        return try {
            val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                PackageManager.GET_SIGNING_CERTIFICATES
            } else {
                PackageManager.GET_SIGNATURES
            }
            val info = context.packageManager.getPackageInfo(EXPECTED_PACKAGE, flags)
            val signers = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                info.signingInfo?.apkContentsSigners
            } else {
                info.signatures
            } ?: return false
            if (signers.size != 1) return false

            val expected = hexToBytes(EXPECTED_CERT_SHA256) ?: return false
            MessageDigest.isEqual(
                MessageDigest.getInstance("SHA-256").digest(signers[0].toByteArray()),
                expected
            )
        } catch (_: Throwable) {
            false
        }
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

    private fun hexToBytes(value: String): ByteArray? {
        if (value.length != 64) return null
        val result = ByteArray(32)
        for (index in result.indices) {
            val high = Character.digit(value[index * 2], 16)
            val low = Character.digit(value[index * 2 + 1], 16)
            if (high < 0 || low < 0) return null
            result[index] = ((high shl 4) or low).toByte()
        }
        return result
    }
}
