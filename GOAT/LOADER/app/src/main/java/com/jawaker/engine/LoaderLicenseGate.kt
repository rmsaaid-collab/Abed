package com.jawaker.engine

import android.content.Context
import android.os.Build
import android.provider.Settings
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONObject
import java.io.BufferedInputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.SecureRandom
import java.security.Signature
import java.security.spec.ECGenParameterSpec
import java.security.spec.X509EncodedKeySpec
import java.security.KeyFactory
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.Mac
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Online, device-bound license gate for the loader itself.
 *
 * The private installation key never leaves Android Keystore. The Worker signs
 * every response and the loader verifies that signature locally, so a forged
 * HTTP response cannot unlock the launch path. The native engine performs the
 * same protocol again inside the guest as a second independent authorization.
 */
object LoaderLicenseGate {

    private const val ENDPOINT =
        "https://golden-validate-worker.goat7.workers.dev/api/v2/7f4a9c/session"
    private const val BUILD_ID = "GOAT-P3-20260818-FINAL"
    private const val INSTALL_ALIAS = "goat.p2.install.identity"
    private const val STORAGE_ALIAS = "goat.loader.storage.v2"
    private const val PREFS = "goat.loader.secure.v2"
    private const val PREF_BLOB = "vault"
    private const val PROTOCOL = 2
    private const val MAX_RESPONSE_BYTES = 64 * 1024
    private const val MAX_CLOCK_SKEW_MS = 5 * 60 * 1000L
    private const val MAX_SESSION_MS = 60 * 60 * 1000L

    // P-256 SubjectPublicKeyInfo for the Worker signing key. Responses signed
    // by any other key are rejected even if TLS traffic is intercepted.
    private const val SERVER_PUBLIC_SPKI =
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEQXP9FPKAoLuwBJQK3SKBvZrD8CH3Hd" +
            "HuMW5tXlYNBRmeS-xpLIlUjfACerN1i6W4V6Fk9ICEivuKRWLz9aCIPA"

    data class SavedLicense(val key: String, val expiry: String)

    data class GateResult(
        val ok: Boolean,
        val code: String,
        val message: String,
        val normalizedKey: String = "",
        val expiry: String = "",
        val expiryMillis: Long = 0L,
    )

    private data class InstallIdentity(
        val privateKey: PrivateKey,
        val publicKeyBytes: ByteArray,
    )

    fun deviceId(context: Context): String {
        val androidId = Settings.Secure.getString(
            context.contentResolver,
            Settings.Secure.ANDROID_ID,
        ).orEmpty()
        val raw = "$androidId|${Build.FINGERPRINT}|${Build.BOARD}"
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec("hwid".toByteArray(StandardCharsets.UTF_8), "HmacSHA256"))
        return mac.doFinal(raw.toByteArray(StandardCharsets.UTF_8))
            .take(8)
            .joinToString("") { "%02X".format(it.toInt() and 0xff) }
    }

    fun normalizeKey(raw: String): String? {
        val compact = raw.filterNot { it == '-' || it.isWhitespace() }.toUpperCase(Locale.US)
        if (compact.length != 16 || compact.any { !it.isLetterOrDigit() }) return null
        return compact.chunked(4).joinToString("-")
    }

    fun restore(context: Context): SavedLicense? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return null
        val blob = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(PREF_BLOB, null) ?: return null
        return try {
            val pieces = blob.split('.', limit = 2)
            if (pieces.size != 2) return null
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(
                Cipher.DECRYPT_MODE,
                storageKey(),
                GCMParameterSpec(128, decodeUrl(pieces[0])),
            )
            val plain = String(cipher.doFinal(decodeUrl(pieces[1])), StandardCharsets.UTF_8)
            val separator = plain.indexOf('\n')
            if (separator <= 0) return null
            val key = normalizeKey(plain.substring(0, separator)) ?: return null
            val expiry = plain.substring(separator + 1).trim()
            if (parseExpiryMillis(expiry) <= System.currentTimeMillis()) {
                clear(context)
                return null
            }
            SavedLicense(key, expiry)
        } catch (_: Throwable) {
            clear(context)
            null
        }
    }

    fun clear(context: Context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .remove(PREF_BLOB)
            .apply()
    }

    fun validate(context: Context, rawKey: String): GateResult {
        val integrity = LoaderSecurity.verify(context, verifyEngine = true)
        if (!integrity.trusted) {
            return GateResult(false, integrity.code, "Loader integrity verification failed")
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return GateResult(false, "ANDROID_VERSION", "Android 6.0 or newer is required")
        }
        val key = normalizeKey(rawKey)
            ?: return GateResult(false, "INVALID_KEY", "Enter a valid 16-character license key")

        return try {
            val identity = installIdentity()
            val publicHash = sha256Hex(identity.publicKeyBytes)
            val nonceBytes = ByteArray(24).also(SecureRandom()::nextBytes)
            val nonce = encodeUrl(nonceBytes)
            val timestamp = System.currentTimeMillis()
            val engineHash = LoaderSecurity.expectedEngineSha256()
            val canonical = listOf(
                "GOAT-AUTH-2",
                key,
                deviceId(context),
                BUILD_ID,
                engineHash,
                nonce,
                timestamp.toString(),
                publicHash,
            ).joinToString("\n")

            val signer = Signature.getInstance("SHA256withECDSA")
            signer.initSign(identity.privateKey)
            signer.update(canonical.toByteArray(StandardCharsets.UTF_8))

            val body = JSONObject()
                .put("protocol", PROTOCOL)
                .put("key", key)
                .put("device_id", deviceId(context))
                .put("build_id", BUILD_ID)
                .put("binary_hash", engineHash)
                .put("nonce", nonce)
                .put("timestamp_ms", timestamp)
                .put("install_pub", encodeUrl(identity.publicKeyBytes))
                .put("proof", encodeUrl(signer.sign()))
                .toString()

            val response = postSignedRequest(body)
            verifyResponse(
                response.body,
                response.status,
                key,
                deviceId(context),
                engineHash,
                nonce,
            ).also { result ->
                if (result.ok) save(context, result.normalizedKey, result.expiry)
            }
        } catch (_: java.net.SocketTimeoutException) {
            GateResult(false, "TIMEOUT", "Server timeout — check your connection")
        } catch (_: java.net.UnknownHostException) {
            GateResult(false, "OFFLINE", "No internet connection")
        } catch (_: Throwable) {
            GateResult(false, "SECURITY_ERROR", "Security verification failed — contact support")
        }
    }

    fun parseExpiryMillis(value: String): Long {
        if (value.isBlank()) return 0L

        // Older dashboard builds stored UTC timestamps without a trailing Z
        // and sometimes kept six fractional digits. Canonicalize those values
        // locally instead of weakening signature/time validation.
        val isoMatch = Regex(
            """^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?([zZ]|[+-]\d{2}:?\d{2})?$""",
        ).matchEntire(value.trim())
        if (isoMatch != null) {
            val base = isoMatch.groupValues[1]
            val millis = isoMatch.groupValues[2].padEnd(3, '0').take(3)
            val rawZone = isoMatch.groupValues[3]
            val isUtc = rawZone.isBlank() || rawZone.equals("Z", ignoreCase = true)
            val zone = if (isUtc) "Z" else rawZone.replace(":", "")
            val canonical = "$base.$millis$zone"
            try {
                val pattern = if (isUtc) {
                    "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'"
                } else {
                    "yyyy-MM-dd'T'HH:mm:ss.SSSZ"
                }
                val format = SimpleDateFormat(pattern, Locale.US)
                format.timeZone = TimeZone.getTimeZone("UTC")
                format.isLenient = false
                val parsed: Date = format.parse(canonical) ?: return 0L
                return parsed.time
            } catch (_: Throwable) {
                // Fall through to the legacy accepted patterns below.
            }
        }

        val patterns = arrayOf(
            "yyyy-MM-dd'T'HH:mm:ss.SSSXXX",
            "yyyy-MM-dd'T'HH:mm:ssXXX",
            "yyyy-MM-dd'T'HH:mm:ss.SSSX",
            "yyyy-MM-dd'T'HH:mm:ssX",
        )
        for (pattern in patterns) {
            try {
                val format = SimpleDateFormat(pattern, Locale.US)
                format.timeZone = TimeZone.getTimeZone("UTC")
                format.isLenient = false
                val parsed: Date = format.parse(value) ?: continue
                return parsed.time
            } catch (_: Throwable) {
                // Try the next accepted ISO-8601 representation.
            }
        }
        return 0L
    }

    private data class HttpResult(val status: Int, val body: String)

    private fun postSignedRequest(json: String): HttpResult {
        val connection = (URL(ENDPOINT).openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            connectTimeout = 8_000
            readTimeout = 10_000
            instanceFollowRedirects = false
            doOutput = true
            useCaches = false
            setRequestProperty("Content-Type", "application/json; charset=utf-8")
            setRequestProperty("Accept", "application/json")
        }
        return try {
            connection.outputStream.use { it.write(json.toByteArray(StandardCharsets.UTF_8)) }
            val status = connection.responseCode
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            HttpResult(status, stream?.use(::readLimitedText).orEmpty())
        } finally {
            connection.disconnect()
        }
    }

    private fun readLimitedText(stream: java.io.InputStream): String {
        val input = BufferedInputStream(stream)
        val output = java.io.ByteArrayOutputStream()
        val buffer = ByteArray(4096)
        var total = 0
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            total += count
            if (total > MAX_RESPONSE_BYTES) throw SecurityException("oversized_response")
            output.write(buffer, 0, count)
        }
        return output.toString("UTF-8")
    }

    private fun verifyResponse(
        responseBody: String,
        httpStatus: Int,
        key: String,
        expectedDeviceId: String,
        expectedEngineHash: String,
        expectedNonce: String,
    ): GateResult {
        val response = JSONObject(responseBody)
        if (response.optInt("protocol", -1) != PROTOCOL) throw SecurityException("protocol")
        val payloadBytes = decodeUrl(response.getString("payload"))
        val signatureBytes = decodeUrl(response.getString("signature"))

        val publicKey = KeyFactory.getInstance("EC").generatePublic(
            X509EncodedKeySpec(decodeUrl(SERVER_PUBLIC_SPKI)),
        )
        val verifier = Signature.getInstance("SHA256withECDSA")
        verifier.initVerify(publicKey)
        verifier.update(payloadBytes)
        if (!verifier.verify(signatureBytes)) throw SecurityException("server_signature")

        val fields = splitCanonical(String(payloadBytes, StandardCharsets.UTF_8))
        if (fields.size != 13 || fields[0] != "GOAT-LICENSE-2") {
            throw SecurityException("payload_shape")
        }
        if (!constantTimeEquals(fields[3], sha256Hex(key.toByteArray(StandardCharsets.UTF_8))) ||
            !constantTimeEquals(fields[4], expectedDeviceId) ||
            !constantTimeEquals(fields[5], BUILD_ID) ||
            !constantTimeEquals(fields[6], expectedEngineHash) ||
            !constantTimeEquals(fields[7], expectedNonce)
        ) {
            throw SecurityException("payload_binding")
        }

        val issuedAt = fields[8].toLongOrNull() ?: throw SecurityException("issued_at")
        val sessionExpiry = fields[9].toLongOrNull() ?: throw SecurityException("session_expiry")
        val now = System.currentTimeMillis()
        if (kotlin.math.abs(now - issuedAt) > MAX_CLOCK_SKEW_MS ||
            sessionExpiry < issuedAt || sessionExpiry - issuedAt > MAX_SESSION_MS
        ) {
            throw SecurityException("payload_time")
        }

        if (fields[1] != "1" || fields[2] != "ok" || httpStatus != 200) {
            return deniedResult(fields[2])
        }
        if (sessionExpiry <= now || fields[11].isBlank() || fields[12].length < 22) {
            throw SecurityException("invalid_grant")
        }
        val expiryMillis = parseExpiryMillis(fields[10])
        if (expiryMillis <= now) return deniedResult("license_expired")

        return GateResult(
            ok = true,
            code = "OK",
            message = "VVIP access verified",
            normalizedKey = key,
            expiry = fields[10],
            expiryMillis = expiryMillis,
        )
    }

    private fun deniedResult(reason: String): GateResult {
        val message = when (reason) {
            "license_expired" -> "License expired"
            "license_denied" -> "License is invalid or revoked"
            "device_mismatch" -> "License is bound to another device"
            "upgrade_required" -> "This loader version is not authorized"
            "rate_limited" -> "Too many attempts — wait a moment"
            "clock_rejected" -> "Correct the device date and time"
            "device_proof_rejected", "replay_rejected" -> "Device security proof rejected"
            else -> "Security session rejected — contact support"
        }
        return GateResult(false, reason.toUpperCase(Locale.US), message)
    }

    private fun installIdentity(): InstallIdentity {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (!store.containsAlias(INSTALL_ALIAS)) {
            val generator = KeyPairGenerator.getInstance(
                KeyProperties.KEY_ALGORITHM_EC,
                "AndroidKeyStore",
            )
            val specification = KeyGenParameterSpec.Builder(
                INSTALL_ALIAS,
                KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY,
            )
                .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
                .setDigests(KeyProperties.DIGEST_SHA256)
                .setUserAuthenticationRequired(false)
                .build()
            generator.initialize(specification)
            generator.generateKeyPair()
        }
        val privateKey = store.getKey(INSTALL_ALIAS, null) as? PrivateKey
            ?: throw SecurityException("missing_install_key")
        val publicBytes = store.getCertificate(INSTALL_ALIAS)?.publicKey?.encoded
            ?: throw SecurityException("missing_install_certificate")
        return InstallIdentity(privateKey, publicBytes)
    }

    private fun storageKey(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        val existing = store.getKey(STORAGE_ALIAS, null) as? SecretKey
        if (existing != null) return existing
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(
                STORAGE_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build(),
        )
        return generator.generateKey()
    }

    private fun save(context: Context, key: String, expiry: String) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, storageKey())
        val encrypted = cipher.doFinal("$key\n$expiry".toByteArray(StandardCharsets.UTF_8))
        val blob = "${encodeUrl(cipher.iv)}.${encodeUrl(encrypted)}"
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_BLOB, blob)
            .apply()
    }

    private fun splitCanonical(value: String): List<String> {
        val fields = ArrayList<String>(13)
        var start = 0
        for (index in value.indices) {
            if (value[index] == '\n') {
                fields.add(value.substring(start, index))
                start = index + 1
            }
        }
        fields.add(value.substring(start))
        return fields
    }

    private fun sha256Hex(value: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(value)
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }

    private fun constantTimeEquals(left: String, right: String): Boolean =
        MessageDigest.isEqual(
            left.toByteArray(StandardCharsets.UTF_8),
            right.toByteArray(StandardCharsets.UTF_8),
        )

    private fun encodeUrl(value: ByteArray): String =
        Base64.encodeToString(value, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)

    private fun decodeUrl(value: String): ByteArray =
        Base64.decode(value, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
}
