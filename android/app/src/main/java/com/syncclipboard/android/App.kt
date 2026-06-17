package com.syncclipboard.android

import android.app.Application
import com.syncclipboard.android.data.SecureConfigStore
import com.syncclipboard.android.net.ConnectionManager

/**
 * Process-wide singletons. Kept to a minimum: SecureConfigStore is shared so
 * sensitive values are read/written through the same encrypted file, and
 * ConnectionManager is shared so the OkHttpClient connection pool / state
 * survives across screens and the foreground service.
 */
class App : Application() {

    override fun onCreate() {
        super.onCreate()
        instance = this
        secureConfig = SecureConfigStore(this)
        connectionManager = ConnectionManager(secureConfig)
    }

    companion object {
        @Volatile lateinit var instance: App
            private set

        @Volatile lateinit var secureConfig: SecureConfigStore
            private set

        @Volatile lateinit var connectionManager: ConnectionManager
            private set
    }
}
