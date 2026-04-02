package com.syncclipboard.android.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.ContextCompat

class BootReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "BootReceiver"
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        Log.i(TAG, "开机完成，启动保活服务")
        val serviceIntent = Intent(context, KeepAliveService::class.java)
            .putExtra("status_text", "开机自启，等待无障碍服务...")
        ContextCompat.startForegroundService(context, serviceIntent)
    }
}
