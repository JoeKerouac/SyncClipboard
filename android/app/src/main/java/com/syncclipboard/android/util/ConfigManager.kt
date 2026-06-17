package com.syncclipboard.android.util

import android.content.Context
import com.syncclipboard.android.App
import com.syncclipboard.android.data.SecureConfigStore

/**
 * Backwards-compatible facade over [SecureConfigStore]. Existing code that
 * still references ConfigManager keeps working while UI pieces migrate to
 * the new store. New code should depend on SecureConfigStore directly via
 * [App.secureConfig].
 */
class ConfigManager(context: Context) {

    private val store: SecureConfigStore = try {
        App.secureConfig
    } catch (_: UninitializedPropertyAccessException) {
        SecureConfigStore(context.applicationContext)
    }

    var serverHost: String
        get() = store.serverHost
        set(v) { store.serverHost = v }

    var serverPort: Int
        get() = store.serverPort
        set(v) { store.serverPort = v }

    var serverPath: String
        get() = store.serverPath
        set(v) { store.serverPath = v }

    /** v2 has no server-key concept; kept as alias for UI compatibility. */
    var serverKey: String
        get() = ""
        set(_) {}

    var username: String
        get() = store.username
        set(v) { store.username = v }

    var password: String
        get() = store.password
        set(v) { store.password = v }

    var aesKey: String
        get() = store.aesKey
        set(v) { store.aesKey = v }

    var deviceId: String
        get() = store.deviceId
        set(v) { store.deviceId = v }

    var lastWsStatus: String
        get() = store.lastWsStatus
        set(v) { store.lastWsStatus = v }

    var fileTransferLevel: Int
        get() = store.fileTransferLevel
        set(v) { store.fileTransferLevel = v }

    var maxTransferSizeMb: Int
        get() = store.maxTransferSizeMb
        set(v) { store.maxTransferSizeMb = v }

    fun isConfigured(): Boolean = store.isConfigured()

    fun getWebSocketUrl(): String = store.webSocketUrl()
}
