package com.syncclipboard.android.service

import android.content.Context
import androidx.work.CoroutineWorker
import androidx.work.WorkerParameters
import com.syncclipboard.android.App
import com.syncclipboard.android.util.AppLog
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Periodic keep-alive: ensures the access token is fresh and the shared
 * connection is started, so when the app is brought to foreground the
 * clipboard pipeline is already up. Runs strictly inside coroutine
 * cancellation semantics — no naked Thread.sleep / orphan threads.
 */
class SyncWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {

    override suspend fun doWork(): Result {
        if (!App.secureConfig.isConfigured()) return Result.failure()

        val token = App.connectionManager.authRepository().ensureValidToken()
        if (token == null) {
            AppLog.w(TAG, "[SYNC] could not obtain access token")
            return Result.retry()
        }

        // Start the shared connection. ClipboardService will reuse it when it
        // boots; if no service is alive we exit after a short wait so the
        // worker returns instead of holding a socket forever.
        App.connectionManager.start()
        withTimeoutOrNull(5_000L) {
            // Just yield while the connection establishes.
            delay(5_000L)
        }
        return Result.success()
    }

    companion object {
        private const val TAG = "SyncWorker"
    }
}
