package com.syncclipboard.android.util

import android.content.Context
import android.content.SharedPreferences

class ConfigManager(context: Context) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences("sync_clipboard_config", Context.MODE_PRIVATE)

    var serverHost: String
        get() = prefs.getString("server_host", "10.0.2.2") ?: "10.0.2.2"
        set(value) = prefs.edit().putString("server_host", value).apply()

    var serverPort: Int
        get() = prefs.getInt("server_port", 8080)
        set(value) = prefs.edit().putInt("server_port", value).apply()

    var serverPath: String
        get() = prefs.getString("server_path", "/ws/clipboard") ?: "/ws/clipboard"
        set(value) = prefs.edit().putString("server_path", value).apply()

    var serverKey: String
        get() = prefs.getString("server_key", "my-secret-server-key") ?: "my-secret-server-key"
        set(value) = prefs.edit().putString("server_key", value).apply()

    var username: String
        get() = prefs.getString("username", "admin") ?: "admin"
        set(value) = prefs.edit().putString("username", value).apply()

    var password: String
        get() = prefs.getString("password", "admin123") ?: "admin123"
        set(value) = prefs.edit().putString("password", value).apply()

    var aesKey: String
        get() = prefs.getString("aes_key",
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
            ?: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        set(value) = prefs.edit().putString("aes_key", value).apply()

    var deviceId: String
        get() = prefs.getString("device_id", "android-device-01") ?: "android-device-01"
        set(value) = prefs.edit().putString("device_id", value).apply()

    var lastWsStatus: String
        get() = prefs.getString("last_ws_status", "") ?: ""
        set(value) = prefs.edit().putString("last_ws_status", value).apply()

    var fileTransferLevel: Int
        get() = prefs.getInt("file_transfer_level", 3)
        set(value) = prefs.edit().putInt("file_transfer_level", value).apply()

    /** P2P (LAN/NAT) 最大传输大小 MB */
    var maxTransferSizeMb: Int
        get() = prefs.getInt("max_transfer_size_mb", 500)
        set(value) = prefs.edit().putInt("max_transfer_size_mb", value).apply()

    fun getWebSocketUrl(): String = "ws://$serverHost:$serverPort$serverPath"
}
