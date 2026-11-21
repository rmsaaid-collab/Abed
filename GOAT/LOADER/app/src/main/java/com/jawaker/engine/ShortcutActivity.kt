package com.jawaker.engine

import android.app.Activity
import android.content.Intent
import android.os.Bundle

/**
 * ShortcutActivity
 *
 * Handles dynamic shortcut intents. When a shortcut is created for the
 * app, this activity processes the intent and forwards to MainActivity.
 */
class ShortcutActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val intent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        startActivity(intent)
        finish()
    }
}
