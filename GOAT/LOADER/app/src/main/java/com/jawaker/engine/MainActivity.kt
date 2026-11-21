package com.jawaker.engine

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import top.niunaijun.blackbox.BlackBoxCore

/**
 * MainActivity
 *
 * The main user interface for the Jawaker Engine. This activity:
 *
 *   1. Displays real-time injection and target status via two CardViews
 *   2. Provides a large gradient "LAUNCH ENGINE" button
 *   3. On button press:
 *      a. Verifies the exact original Jawaker 28.2.90 package
 *      b. Installs its base package into the BlackBox environment
 *      c. Stages the untouched base/split native runtime
 *      d. Adds libdobby + libengine beside the original runtime
 *      e. Launches the game via BlackBoxCore.get().launchApk()
 *   5. Shows a live log output of all engine operations
 *
 * The status cards update in real-time to reflect the current state of
 * the injection pipeline and the virtual environment.
 */
class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "MainActivity"
        private const val TARGET_PACKAGE = EngineLibManager.TARGET_PACKAGE
        private const val VIRTUAL_USER_ID = EngineLibManager.VIRTUAL_USER_ID
    }

    // ─── UI References ──────────────────────────────────────────────────
    private lateinit var btnLaunch: Button
    private lateinit var tvLog: TextView
    private lateinit var scrollLog: ScrollView

    // Signed loader license gate
    private lateinit var etLicenseKey: EditText
    private lateinit var btnActivateLicense: Button
    private lateinit var tvLicenseState: TextView
    private lateinit var tvLicenseDeviceId: TextView
    private lateinit var tvLicenseExpiry: TextView
    private lateinit var tvLicenseCountdown: TextView
    private lateinit var tvContactSupport: TextView

    // Injection Status Card
    private lateinit var dotDobby: View
    private lateinit var dotIl2cpp: View
    private lateinit var dotGgengine: View
    private lateinit var dotRedirect: View
    private lateinit var tvDobbyStatus: TextView
    private lateinit var tvIl2cppStatus: TextView
    private lateinit var tvGgengineStatus: TextView
    private lateinit var tvRedirectStatus: TextView
    private lateinit var tvInjectionOverall: TextView

    // Target Status Card
    private lateinit var dotTargetInstalled: View
    private lateinit var dotEngine: View
    private lateinit var tvTargetInstalled: TextView
    private lateinit var tvEngineStatus: TextView
    private lateinit var tvTargetOverall: TextView

    // GOAT Performance Engine (versioned native profile channel)
    private lateinit var rgPerformance: RadioGroup
    private lateinit var tvPerformanceSummary: TextView

    // ─── State ──────────────────────────────────────────────────────────
    private var isLaunching = false
    private var isActivating = false
    private var licenseUnlocked = false
    private var licenseExpiryMillis = 0L
    private val countdownHandler = Handler(Looper.getMainLooper())
    private val countdownRunnable = object : Runnable {
        override fun run() {
            updateLicenseCountdown()
            countdownHandler.postDelayed(this, 1_000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        bindViews()
        val integrity = LoaderSecurity.verify(this, verifyEngine = true)
        if (!integrity.trusted) {
            lockForSecurity(integrity.code)
            return
        }

        Log.i(TAG, "═══════════════════════════════════════════════════")
        Log.i(TAG, "  MainActivity.onCreate()")
        Log.i(TAG, "  API Level: ${Build.VERSION.SDK_INT}")
        Log.i(TAG, "  Device: ${Build.MANUFACTURER} ${Build.MODEL}")
        Log.i(TAG, "═══════════════════════════════════════════════════")

        setupListeners()
        initializePerformanceProfile()
        initializeLicenseGate()

        // Initial status refresh
        refreshStatus()

        // Log initial engine state
        val status = MainApplication.injectionStatus
        logLine("[Engine] Virtual engine: ${if (status.engineInitialized) "READY" else "INITIALIZING"}")
        logLine("[Engine] Libraries copied: ${if (status.libsCopied) "YES" else "NO"}")
        logLine("[Engine] Original runtime: ${if (status.redirectActive) "READY" else "PENDING"}")
        logLine("[Target] Package: $TARGET_PACKAGE")
        logLine("[Target] Virtual user: $VIRTUAL_USER_ID")

        // Check if the target is installed in the virtual environment
        checkTargetInstallation()
    }

    /**
     * Binds all UI views by their IDs.
     */
    private fun bindViews() {
        // Launch button
        btnLaunch = findViewById(R.id.btnLaunch)
        tvLog = findViewById(R.id.tvLog)
        scrollLog = findViewById(R.id.scrollLog)

        etLicenseKey = findViewById(R.id.etLicenseKey)
        btnActivateLicense = findViewById(R.id.btnActivateLicense)
        tvLicenseState = findViewById(R.id.tvLicenseState)
        tvLicenseDeviceId = findViewById(R.id.tvLicenseDeviceId)
        tvLicenseExpiry = findViewById(R.id.tvLicenseExpiry)
        tvLicenseCountdown = findViewById(R.id.tvLicenseCountdown)
        tvContactSupport = findViewById(R.id.tvContactSupport)

        // Injection status indicators
        dotDobby = findViewById(R.id.dotDobby)
        dotIl2cpp = findViewById(R.id.dotIl2cpp)
        dotGgengine = findViewById(R.id.dotGgengine)
        dotRedirect = findViewById(R.id.dotRedirect)
        tvDobbyStatus = findViewById(R.id.tvDobbyStatus)
        tvIl2cppStatus = findViewById(R.id.tvIl2cppStatus)
        tvGgengineStatus = findViewById(R.id.tvGgengineStatus)
        tvRedirectStatus = findViewById(R.id.tvRedirectStatus)
        tvInjectionOverall = findViewById(R.id.tvInjectionOverall)

        // Target status indicators
        dotTargetInstalled = findViewById(R.id.dotTargetInstalled)
        dotEngine = findViewById(R.id.dotEngine)
        tvTargetInstalled = findViewById(R.id.tvTargetInstalled)
        tvEngineStatus = findViewById(R.id.tvEngineStatus)
        tvTargetOverall = findViewById(R.id.tvTargetOverall)

        rgPerformance = findViewById(R.id.rgPerformance)
        tvPerformanceSummary = findViewById(R.id.tvPerformanceSummary)
    }

    /**
     * Sets up click listeners for the launch button.
     */
    private fun setupListeners() {
        btnLaunch.setOnClickListener {
            if (!isLaunching) {
                onLaunchEngineClicked()
            }
        }
        btnActivateLicense.setOnClickListener {
            activateLicense(silent = false)
        }
        tvContactSupport.setOnClickListener {
            try {
                startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://t.me/onlygoat07")))
            } catch (_: Throwable) {
                showToast("Telegram: @onlygoat07")
            }
        }
        rgPerformance.setOnCheckedChangeListener { _, checkedId ->
            val profileId = when (checkedId) {
                R.id.rbPerformanceSafe -> 0
                R.id.rbPerformanceBalanced -> 1
                R.id.rbPerformancePc -> 3
                else -> 2
            }
            val profile = EngineLibManager.setPerformanceProfile(this, profileId)
            renderPerformanceProfile(profile)
        }
    }

    private fun initializePerformanceProfile() {
        val profile = EngineLibManager.performanceConfig(this)
        val checkedId = when (profile.id) {
            0 -> R.id.rbPerformanceSafe
            1 -> R.id.rbPerformanceBalanced
            3 -> R.id.rbPerformancePc
            else -> R.id.rbPerformanceUltra
        }
        rgPerformance.check(checkedId)
        renderPerformanceProfile(profile)
    }

    private fun renderPerformanceProfile(profile: EngineLibManager.PerformanceConfig) {
        tvPerformanceSummary.text =
            "${profile.name}  •  ${profile.sliceNodes} nodes / ${profile.sliceMs}ms  •  exact ACK"
        logLine("[Performance] ${profile.name}: nodes=${profile.sliceNodes}, " +
            "slice=${profile.sliceMs}ms, arena=${profile.arenaNodes}")
    }

    /** Restores only encrypted input, then performs a fresh signed check. */
    private fun initializeLicenseGate() {
        tvLicenseDeviceId.text = LoaderLicenseGate.deviceId(this)
        licenseUnlocked = false
        btnLaunch.isEnabled = false
        countdownHandler.removeCallbacks(countdownRunnable)
        countdownHandler.post(countdownRunnable)

        val saved = LoaderLicenseGate.restore(this)
        if (saved != null) {
            etLicenseKey.setText(saved.key)
            tvLicenseExpiry.text = "Saved license • verifying online…"
            activateLicense(silent = true)
        } else {
            showLicenseLocked("Enter your VVIP license key")
        }
    }

    private fun activateLicense(silent: Boolean) {
        if (isActivating || isLaunching) return
        val normalized = LoaderLicenseGate.normalizeKey(etLicenseKey.text?.toString().orEmpty())
        if (normalized == null) {
            showLicenseLocked("Invalid key format")
            if (!silent) showToast("Enter a valid license key")
            return
        }
        etLicenseKey.setText(normalized)
        etLicenseKey.setSelection(normalized.length)
        isActivating = true
        licenseUnlocked = false
        btnLaunch.isEnabled = false
        btnActivateLicense.isEnabled = false
        btnActivateLicense.text = "VERIFYING SIGNATURE…"
        tvLicenseState.text = "VERIFYING"
        tvLicenseState.setTextColor(ContextCompat.getColor(this, R.color.status_info))
        tvLicenseExpiry.text = "Creating secure device session…"

        Thread {
            val result = LoaderLicenseGate.validate(this, normalized)
            runOnUiThread {
                isActivating = false
                btnActivateLicense.isEnabled = true
                btnActivateLicense.text = getString(R.string.license_unlock)
                applyLicenseResult(result)
                if (!silent) showToast(result.message)
            }
        }.start()
    }

    private fun applyLicenseResult(result: LoaderLicenseGate.GateResult) {
        if (result.ok) {
            licenseUnlocked = true
            licenseExpiryMillis = result.expiryMillis
            tvLicenseState.text = "VVIP ACTIVE"
            tvLicenseState.setTextColor(ContextCompat.getColor(this, R.color.status_success))
            tvLicenseExpiry.text = "Verified • expires ${result.expiry}"
            tvLicenseExpiry.setTextColor(ContextCompat.getColor(this, R.color.status_success))
            btnLaunch.isEnabled = !isLaunching
            logLine("[License] ✓ Signed loader access verified")
            logLine("[License] Device: ${LoaderLicenseGate.deviceId(this)}")
        } else {
            licenseUnlocked = false
            licenseExpiryMillis = 0L
            btnLaunch.isEnabled = false
            tvLicenseState.text = result.code.take(18)
            tvLicenseState.setTextColor(ContextCompat.getColor(this, R.color.status_error))
            tvLicenseExpiry.text = result.message
            tvLicenseExpiry.setTextColor(ContextCompat.getColor(this, R.color.status_error))
            logLine("[License] ✗ ${result.code}")
        }
        updateLicenseCountdown()
    }

    private fun showLicenseLocked(message: String) {
        licenseUnlocked = false
        licenseExpiryMillis = 0L
        btnLaunch.isEnabled = false
        tvLicenseState.text = getString(R.string.license_pending)
        tvLicenseState.setTextColor(ContextCompat.getColor(this, R.color.status_warning))
        tvLicenseExpiry.text = message
        tvLicenseExpiry.setTextColor(ContextCompat.getColor(this, R.color.text_secondary))
        tvLicenseCountdown.text = "--:--:--"
    }

    private fun updateLicenseCountdown() {
        if (!licenseUnlocked || licenseExpiryMillis <= 0L) {
            tvLicenseCountdown.text = "--:--:--"
            return
        }
        val remaining = licenseExpiryMillis - System.currentTimeMillis()
        if (remaining <= 0L) {
            LoaderLicenseGate.clear(this)
            showLicenseLocked("License expired")
            tvLicenseState.text = "EXPIRED"
            tvLicenseState.setTextColor(ContextCompat.getColor(this, R.color.status_error))
            return
        }
        val totalSeconds = remaining / 1_000L
        val days = totalSeconds / 86_400L
        val hours = (totalSeconds % 86_400L) / 3_600L
        val minutes = (totalSeconds % 3_600L) / 60L
        val seconds = totalSeconds % 60L
        tvLicenseCountdown.text = if (days > 0L) {
            String.format(java.util.Locale.US, "%dd %02d:%02d:%02d", days, hours, minutes, seconds)
        } else {
            String.format(java.util.Locale.US, "%02d:%02d:%02d", hours, minutes, seconds)
        }
    }

    /**
     * Handles the "LAUNCH ENGINE" button click.
     *
     * This is the main action flow:
     *   1. Disable the button to prevent double-clicks
     *   2. Ensure the virtual user exists
     *   3. Check if the target is installed in the virtual environment
     *   4. If not installed, install it from the system
     *   5. Stage and verify the original split runtime + GOAT engine
     *   6. Launch the game
     */
    private fun onLaunchEngineClicked() {
        val integrity = LoaderSecurity.verify(this, verifyEngine = true)
        if (!integrity.trusted) {
            lockForSecurity(integrity.code)
            return
        }
        isLaunching = true
        btnLaunch.isEnabled = false
        btnActivateLicense.isEnabled = false
        btnLaunch.text = getString(R.string.status_initializing)

        logLine("─────────────────────────────────────────")
        logLine("[Engine] LAUNCH ENGINE button pressed")
        logLine("[Engine] Starting launch sequence...")

        // Run the launch sequence on a background thread to avoid blocking UI
        Thread {
            try {
                // A successful UI activation is not blindly trusted. Obtain a
                // fresh, signed lease immediately before touching the guest.
                val license = LoaderLicenseGate.validate(
                    this,
                    etLicenseKey.text?.toString().orEmpty(),
                )
                runOnUiThread { applyLicenseResult(license) }
                if (!license.ok) {
                    runOnUiThread {
                        showToast(license.message)
                        resetLaunchButton()
                    }
                    return@Thread
                }

                // ─── Step 1: Ensure virtual user exists ────────────────
                ensureVirtualUser()

                // ─── Step 2: Check/install target package ──────────────
                val installed = ensureTargetInstalled()

                if (!installed) {
                    runOnUiThread {
                        logLine("[Error] Exact original Jawaker 28.2.90 is required")
                        showToast("Install original Jawaker 28.2.90 first")
                        resetLaunchButton()
                    }
                    return@Thread
                }

                // ─── Step 3: Prepare exact original runtime + engine ───
                logLine("[Engine] Preparing original base/split runtime...")
                val preparation = EngineLibManager.prepareVirtualRuntime(this)
                val status = MainApplication.injectionStatus
                status.libsCopied = preparation.success
                status.il2cppLoaded = preparation.originalRuntimeReady
                status.redirectActive = preparation.originalRuntimeReady
                status.dobbyLoaded = preparation.engineReady
                status.ggengineLoaded = preparation.engineReady

                if (!preparation.success) {
                    runOnUiThread {
                        logLine("[Error] ${preparation.message}")
                        showToast(preparation.message)
                        resetLaunchButton()
                        refreshStatus()
                    }
                    return@Thread
                }

                logLine(
                    "[Engine] ✓ ${preparation.originalLibraryCount} original arm64 libraries verified"
                )
                logLine("[Engine] ✓ libdobby.so + libengine.so staged")

                // ─── Step 4: Launch the game ───────────────────────────
                runOnUiThread {
                    btnLaunch.text = getString(R.string.msg_launching)
                }
                logLine("[Engine] Launching $TARGET_PACKAGE in virtual environment...")

                val launched = launchTarget()

                runOnUiThread {
                    if (launched) {
                        logLine("[Engine] ✓ Jawaker launched successfully!")
                        logLine("[Engine] Guest namespace loader is active")
                        showToast("Jawaker launched with GOAT Engine")
                    } else {
                        logLine("[Engine] ✗ Failed to launch Jawaker")
                        showToast("Failed to launch Jawaker")
                    }
                    resetLaunchButton()
                    refreshStatus()
                }

            } catch (e: Exception) {
                Log.e(TAG, "Launch sequence failed", e)
                runOnUiThread {
                    logLine("[Error] Launch sequence failed: ${e.message}")
                    showToast("Launch failed: ${e.message}")
                    resetLaunchButton()
                }
            }
        }.start()
    }

    /**
     * Ensures that the virtual user (ID 0) exists in the BlackBox environment.
     * If it doesn't exist, creates it.
     */
    private fun ensureVirtualUser() {
        try {
            val users = BlackBoxCore.get().users
            if (users.isEmpty()) {
                logLine("[Engine] No virtual users found — creating user $VIRTUAL_USER_ID")
                BlackBoxCore.get().createUser(VIRTUAL_USER_ID)
                logLine("[Engine] ✓ Virtual user $VIRTUAL_USER_ID created")
            } else {
                val userExists = users.any { it.id == VIRTUAL_USER_ID }
                if (!userExists) {
                    logLine("[Engine] User $VIRTUAL_USER_ID not found — creating...")
                    BlackBoxCore.get().createUser(VIRTUAL_USER_ID)
                    logLine("[Engine] ✓ Virtual user $VIRTUAL_USER_ID created")
                } else {
                    logLine("[Engine] ✓ Virtual user $VIRTUAL_USER_ID already exists")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "ensureVirtualUser failed (may already exist)", e)
            logLine("[Engine] User setup: ${e.message}")
        }
    }

    /**
     * Ensures the target package is installed in the virtual environment.
     *
     * If not installed, attempts to install it from the system (the real
     * Android system, not the virtual environment) using
     * BlackBoxCore.get().installPackageAsUser(packageName, userId).
     *
     * @return true if the target is installed (or was successfully installed)
     */
    private fun ensureTargetInstalled(): Boolean {
        return try {
            val systemPackage = EngineLibManager.getSystemPackage(this)
            if (!EngineLibManager.isSupported(systemPackage)) {
                logLine("[Target] ✗ ${EngineLibManager.unsupportedVersionMessage(systemPackage)}")
                return false
            }
            systemPackage ?: return false

            logLine(
                "[Target] ✓ Original Jawaker ${systemPackage.versionName} " +
                    "(${systemPackage.versionCode})"
            )
            logLine("[Target] ✓ Split APK count: ${systemPackage.splitApks.size}")

            var virtualInstalled = BlackBoxCore.get()
                .isInstalled(TARGET_PACKAGE, VIRTUAL_USER_ID)

            if (virtualInstalled) {
                val virtualInfo = BlackBoxCore.getBPackageManager()
                    .getPackageInfo(TARGET_PACKAGE, 0, VIRTUAL_USER_ID)
                val sameVersion = virtualInfo != null &&
                    virtualInfo.versionName == EngineLibManager.REQUIRED_VERSION_NAME &&
                    virtualPackageVersionCode(virtualInfo) == EngineLibManager.REQUIRED_VERSION_CODE

                if (!sameVersion) {
                    logLine("[Target] Virtual copy is stale — reinstalling exact build...")
                    BlackBoxCore.get().uninstallPackageAsUser(TARGET_PACKAGE, VIRTUAL_USER_ID)
                    virtualInstalled = false
                }
            }

            if (!virtualInstalled) {
                runOnUiThread {
                    btnLaunch.text = getString(R.string.msg_installing)
                }
                logLine("[Target] Installing the original system package into BlackBox...")
                val result = BlackBoxCore.get()
                    .installPackageAsUser(TARGET_PACKAGE, VIRTUAL_USER_ID)
                if (result == null || !result.success) {
                    logLine("[Target] ✗ Install failed: ${result?.msg ?: "unknown error"}")
                    return false
                }
            }

            val verified = BlackBoxCore.get()
                .isInstalled(TARGET_PACKAGE, VIRTUAL_USER_ID)
            if (verified) {
                logLine("[Target] ✓ Exact original build is ready in BlackBox")
            } else {
                logLine("[Target] ✗ Virtual installation verification failed")
            }
            verified
        } catch (error: Throwable) {
            Log.e(TAG, "ensureTargetInstalled failed", error)
            logLine("[Target] Installation error: ${error.message}")
            false
        }
    }

    @Suppress("DEPRECATION")
    private fun virtualPackageVersionCode(info: android.content.pm.PackageInfo): Long {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            info.longVersionCode
        } else {
            info.versionCode.toLong()
        }
    }

    /**
     * Launches the target package in the virtual environment.
     *
     * launchApk(String, int) returns a boolean indicating whether the launch
     * was initiated successfully. We use that return value directly.
     *
     * @return true if the launch was initiated successfully
     */
    private fun launchTarget(): Boolean {
        return try {
            val ok = BlackBoxCore.get().launchApk(TARGET_PACKAGE, VIRTUAL_USER_ID)
            if (!ok) {
                logLine("[Error] launchApk returned false")
            }
            ok
        } catch (e: Exception) {
            Log.e(TAG, "launchTarget failed", e)
            logLine("[Error] Launch failed: ${e.message}")
            false
        }
    }

    /**
     * Resets the launch button to its default state.
     */
    private fun resetLaunchButton() {
        isLaunching = false
        btnLaunch.isEnabled = licenseUnlocked
        btnActivateLicense.isEnabled = true
        btnLaunch.text = getString(R.string.launch_engine)
    }

    /**
     * Checks the target installation status and updates the UI.
     */
    private fun checkTargetInstallation() {
        Thread {
            try {
                val isInstalled = BlackBoxCore.get().isInstalled(TARGET_PACKAGE, VIRTUAL_USER_ID)
                runOnUiThread {
                    updateTargetInstalledStatus(isInstalled)
                }
            } catch (e: Exception) {
                Log.w(TAG, "checkTargetInstallation failed", e)
            }
        }.start()
    }

    /**
     * Refreshes all status indicators in the UI.
     */
    private fun refreshStatus() {
        val status = MainApplication.injectionStatus

        // Injection status indicators
        updateStatusIndicator(dotDobby, tvDobbyStatus, status.dobbyLoaded)
        updateStatusIndicator(dotIl2cpp, tvIl2cppStatus, status.il2cppLoaded)
        updateStatusIndicator(dotGgengine, tvGgengineStatus, status.ggengineLoaded)
        updateStatusIndicator(dotRedirect, tvRedirectStatus, status.redirectActive)

        // Injection overall
        if (status.allLoaded && status.redirectActive) {
            tvInjectionOverall.text = getString(R.string.status_ready)
            tvInjectionOverall.setTextColor(ContextCompat.getColor(this, R.color.status_success))
        } else if (status.engineInitialized) {
            tvInjectionOverall.text = getString(R.string.status_ready)
            tvInjectionOverall.setTextColor(ContextCompat.getColor(this, R.color.status_info))
        } else {
            tvInjectionOverall.text = getString(R.string.status_initializing)
            tvInjectionOverall.setTextColor(ContextCompat.getColor(this, R.color.status_warning))
        }

        // Engine status in target card
        updateStatusIndicator(dotEngine, tvEngineStatus, status.engineInitialized)

        // Target overall
        val targetReady = try {
            BlackBoxCore.get().isInstalled(TARGET_PACKAGE, VIRTUAL_USER_ID)
        } catch (e: Exception) {
            false
        }
        updateTargetInstalledStatus(targetReady)
    }

    /**
     * Updates a status indicator (dot + text) based on a boolean state.
     */
    private fun updateStatusIndicator(dot: View, text: TextView, isActive: Boolean) {
        if (isActive) {
            dot.setBackgroundResource(R.drawable.dot_success)
            text.text = getString(R.string.status_loaded)
            text.setTextColor(ContextCompat.getColor(this, R.color.status_success))
        } else {
            dot.setBackgroundResource(R.drawable.dot_warning)
            text.text = getString(R.string.status_pending)
            text.setTextColor(ContextCompat.getColor(this, R.color.status_warning))
        }
    }

    /**
     * Updates the target installation status indicator.
     */
    private fun updateTargetInstalledStatus(isInstalled: Boolean) {
        if (isInstalled) {
            dotTargetInstalled.setBackgroundResource(R.drawable.dot_success)
            tvTargetInstalled.text = getString(R.string.status_installed)
            tvTargetInstalled.setTextColor(ContextCompat.getColor(this, R.color.status_success))
            tvTargetOverall.text = getString(R.string.status_ready)
            tvTargetOverall.setTextColor(ContextCompat.getColor(this, R.color.status_success))
        } else {
            dotTargetInstalled.setBackgroundResource(R.drawable.dot_error)
            tvTargetInstalled.text = getString(R.string.status_not_installed)
            tvTargetInstalled.setTextColor(ContextCompat.getColor(this, R.color.status_error))
            tvTargetOverall.text = getString(R.string.status_inactive)
            tvTargetOverall.setTextColor(ContextCompat.getColor(this, R.color.status_error))
        }
    }

    /**
     * Appends a line to the log output and auto-scrolls to the bottom.
     */
    private fun logLine(message: String) {
        Log.i(TAG, message)
        runOnUiThread {
            val timestamp = System.currentTimeMillis() % 100000
            val line = "[$timestamp] $message\n"
            tvLog.append(line)
            scrollLog.post {
                scrollLog.fullScroll(ScrollView.FOCUS_DOWN)
            }
        }
    }

    /**
     * Shows a toast message.
     */
    private fun showToast(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show()
    }

    private fun lockForSecurity(code: String) {
        isLaunching = false
        isActivating = false
        licenseUnlocked = false
        btnLaunch.isEnabled = false
        btnActivateLicense.isEnabled = false
        btnLaunch.text = "SECURITY LOCK"
        btnActivateLicense.text = "INTEGRITY FAILED"
        tvLicenseState.text = code
        tvLicenseState.setTextColor(ContextCompat.getColor(this, R.color.status_error))
        tvLicenseExpiry.text = "Loader integrity verification failed"
        tvLicenseExpiry.setTextColor(ContextCompat.getColor(this, R.color.status_error))
        tvInjectionOverall.text = "INTEGRITY FAILED"
        tvInjectionOverall.setTextColor(ContextCompat.getColor(this, R.color.status_error))
        tvTargetOverall.text = code
        tvTargetOverall.setTextColor(ContextCompat.getColor(this, R.color.status_error))
        logLine("[Security] Loader verification failed ($code)")
    }

    override fun onResume() {
        super.onResume()
        val integrity = LoaderSecurity.verify(this, verifyEngine = true)
        if (!integrity.trusted) {
            lockForSecurity(integrity.code)
            return
        }
        // Refresh status when returning to the activity
        refreshStatus()
        checkTargetInstallation()
    }

    override fun onDestroy() {
        countdownHandler.removeCallbacks(countdownRunnable)
        super.onDestroy()
    }
}
