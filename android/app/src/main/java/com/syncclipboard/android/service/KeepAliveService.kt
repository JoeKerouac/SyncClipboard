package com.syncclipboard.android.service

import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.SystemClock
import android.util.Log
import com.syncclipboard.android.ui.MainActivity

class KeepAliveService : Service() {

    companion object {
        private const val TAG = "KeepAliveService"
        private const val CHANNEL_ID = "keep_alive_channel"
        private const val NOTIFICATION_ID = 2

        fun updateNotificationText(service: Service, text: String) {
            val manager = service.getSystemService(NotificationManager::class.java)
            manager.notify(NOTIFICATION_ID, buildNotification(service, text))
        }

        private fun buildNotification(service: Service, text: String): Notification {
            val intent = Intent(service, MainActivity::class.java)
            val pi = PendingIntent.getActivity(
                service, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            return Notification.Builder(service, CHANNEL_ID)
                .setContentTitle("SyncClipboard")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_menu_share)
                .setContentIntent(pi)
                .setOngoing(true)
                .build()
        }
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val text = intent?.getStringExtra("status_text") ?: "剪切板同步运行中"
        startForeground(NOTIFICATION_ID, buildNotification(this, text))
        Log.i(TAG, "保活服务已启动")
        return START_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        Log.w(TAG, "应用被移出最近任务，安排自动重启")
        scheduleRestart()
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        Log.w(TAG, "保活服务被销毁，安排自动重启")
        scheduleRestart()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun scheduleRestart() {
        val restartIntent = Intent(applicationContext, KeepAliveService::class.java)
            .putExtra("status_text", "服务已自动恢复")
        val pi = PendingIntent.getService(
            applicationContext, 1, restartIntent,
            PendingIntent.FLAG_ONE_SHOT or PendingIntent.FLAG_IMMUTABLE
        )
        val am = getSystemService(ALARM_SERVICE) as AlarmManager
        val triggerAt = SystemClock.elapsedRealtime() + 3000
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            am.setAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP, triggerAt, pi)
        } else {
            am.set(AlarmManager.ELAPSED_REALTIME_WAKEUP, triggerAt, pi)
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID, "后台保活",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "保持剪切板同步服务常驻后台"
                setShowBadge(false)
            }
            getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }
}
