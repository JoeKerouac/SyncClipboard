package com.syncclipboard.android.data

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

/**
 * Encrypted-at-rest configuration store. Replaces the plain SharedPreferences
 * used in v1. On first launch this class migrates any existing plain prefs
 * (sync_clipboard_config) into the encrypted file and then deletes the
 * originals. Callers should never read/write the plain file directly.
 */
class SecureConfigStore(context: Context) {

    private val ctx = context.applicationContext

    private val prefs: SharedPreferences = createEncryptedPrefs()

    init {
        migrateLegacyPrefsIfNeeded()
    }

    private fun createEncryptedPrefs(): SharedPreferences {
        val masterKey = MasterKey.Builder(ctx)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        return EncryptedSharedPreferences.create(
            ctx,
            ENCRYPTED_FILE,
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

    private fun migrateLegacyPrefsIfNeeded() {
        val flag = prefs.getBoolean(KEY_MIGRATED, false)
        if (flag) return
        val legacy = ctx.getSharedPreferences(LEGACY_FILE, Context.MODE_PRIVATE)
        val all = legacy.all
        if (all.isNotEmpty()) {
            val editor = prefs.edit()
            for ((k, v) in all) {
                when (v) {
                    is String -> editor.putString(k, v)
                    is Int -> editor.putInt(k, v)
                    is Long -> editor.putLong(k, v)
                    is Boolean -> editor.putBoolean(k, v)
                    is Float -> editor.putFloat(k, v)
                }
            }
            editor.putBoolean(KEY_MIGRATED, true).apply()
            legacy.edit().clear().apply()
            ctx.deleteSharedPreferences(LEGACY_FILE)
            Log.i(TAG, "migrated ${all.size} legacy preference entries")
        } else {
            prefs.edit().putBoolean(KEY_MIGRATED, true).apply()
        }
    }

    // ---- typed accessors ----

    var serverHost: String
        get() = prefs.getString("server_host", "") ?: ""
        set(v) = prefs.edit().putString("server_host", v).apply()

    var serverPort: Int
        get() = prefs.getInt("server_port", 0)
        set(v) = prefs.edit().putInt("server_port", v).apply()

    var serverPath: String
        get() = prefs.getString("server_path", "/ws/v2/clipboard") ?: "/ws/v2/clipboard"
        set(v) = prefs.edit().putString("server_path", v).apply()

    /** When true the client uses wss:// + https://. Recommended for production. */
    var useTls: Boolean
        get() = prefs.getBoolean("use_tls", false)
        set(v) = prefs.edit().putBoolean("use_tls", v).apply()

    var username: String
        get() = prefs.getString("username", "") ?: ""
        set(v) = prefs.edit().putString("username", v).apply()

    var password: String
        get() = prefs.getString("password", "") ?: ""
        set(v) = prefs.edit().putString("password", v).apply()

    var aesKey: String
        get() = prefs.getString("aes_key", "") ?: ""
        set(v) = prefs.edit().putString("aes_key", v).apply()

    var deviceId: String
        get() = prefs.getString("device_id", "") ?: ""
        set(v) = prefs.edit().putString("device_id", v).apply()

    var fileTransferLevel: Int
        get() = prefs.getInt("file_transfer_level", 3)
        set(v) = prefs.edit().putInt("file_transfer_level", v).apply()

    var maxTransferSizeMb: Int
        get() = prefs.getInt("max_transfer_size_mb", 500)
        set(v) = prefs.edit().putInt("max_transfer_size_mb", v).apply()

    var lastWsStatus: String
        get() = prefs.getString("last_ws_status", "") ?: ""
        set(v) = prefs.edit().putString("last_ws_status", v).apply()

    /** Cached JWT access token. May be empty / expired and is refreshed on demand. */
    var accessToken: String
        get() = prefs.getString("access_token", "") ?: ""
        set(v) = prefs.edit().putString("access_token", v).apply()

    var refreshToken: String
        get() = prefs.getString("refresh_token", "") ?: ""
        set(v) = prefs.edit().putString("refresh_token", v).apply()

    var accessTokenExpEpoch: Long
        get() = prefs.getLong("access_token_exp", 0L)
        set(v) = prefs.edit().putLong("access_token_exp", v).apply()

    fun isConfigured(): Boolean =
        serverHost.isNotBlank()
                && serverPort in 1..65535
                && username.isNotBlank()
                && password.isNotBlank()
                && aesKey.length == 64
                && aesKey.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }
                && deviceId.isNotBlank()

    fun httpBaseUrl(): String {
        val scheme = if (useTls) "https" else "http"
        return "$scheme://$serverHost:$serverPort"
    }

    fun webSocketUrl(): String {
        val scheme = if (useTls) "wss" else "ws"
        val path = if (serverPath.startsWith("/")) serverPath else "/$serverPath"
        return "$scheme://$serverHost:$serverPort$path"
    }

    companion object {
        private const val TAG = "SecureConfigStore"
        private const val ENCRYPTED_FILE = "sync_clipboard_secure"
        private const val LEGACY_FILE = "sync_clipboard_config"
        private const val KEY_MIGRATED = "_migrated_v2"
    }
}
