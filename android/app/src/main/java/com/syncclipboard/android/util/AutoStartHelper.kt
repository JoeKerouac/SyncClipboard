package com.syncclipboard.android.util

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.util.Log

object AutoStartHelper {

    private const val TAG = "AutoStartHelper"

    private val LAUNCH_MANAGER_INTENTS = listOf(
        // Honor (MagicOS)
        Intent().setComponent(ComponentName(
            "com.hihonor.systemmanager",
            "com.hihonor.systemmanager.startupmgr.ui.StartupNormalAppListActivity"
        )),
        // Honor fallback
        Intent().setComponent(ComponentName(
            "com.hihonor.systemmanager",
            "com.hihonor.systemmanager.optimize.process.ProtectActivity"
        )),
        // Huawei
        Intent().setComponent(ComponentName(
            "com.huawei.systemmanager",
            "com.huawei.systemmanager.startupmgr.ui.StartupNormalAppListActivity"
        )),
        Intent().setComponent(ComponentName(
            "com.huawei.systemmanager",
            "com.huawei.systemmanager.optimize.process.ProtectActivity"
        )),
        // Xiaomi
        Intent().setComponent(ComponentName(
            "com.miui.securitycenter",
            "com.miui.permcenter.autostart.AutoStartManagementActivity"
        )),
        // OPPO
        Intent().setComponent(ComponentName(
            "com.coloros.safecenter",
            "com.coloros.safecenter.startupapp.StartupAppListActivity"
        )),
        // Vivo
        Intent().setComponent(ComponentName(
            "com.vivo.permissionmanager",
            "com.vivo.permissionmanager.activity.BgStartUpManagerActivity"
        )),
        // Samsung
        Intent().setComponent(ComponentName(
            "com.samsung.android.lool",
            "com.samsung.android.sm.battery.ui.BatteryActivity"
        )),
    )

    /**
     * 尝试打开厂商的应用启动管理页面
     * @return true if successfully launched
     */
    fun openAutoStartSettings(context: Context): Boolean {
        for (intent in LAUNCH_MANAGER_INTENTS) {
            try {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                if (intent.resolveActivity(context.packageManager) != null) {
                    context.startActivity(intent)
                    Log.i(TAG, "打开自启动管理: ${intent.component}")
                    return true
                }
            } catch (e: Exception) {
                Log.d(TAG, "尝试 ${intent.component} 失败: ${e.message}")
            }
        }
        // Fallback: open app details
        openAppDetails(context)
        return false
    }

    fun openAppDetails(context: Context) {
        val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
            data = Uri.parse("package:${context.packageName}")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(intent)
    }

    fun isHonorOrHuawei(): Boolean {
        val manufacturer = Build.MANUFACTURER.lowercase()
        return manufacturer.contains("honor") || manufacturer.contains("huawei")
    }

    fun getManufacturer(): String = Build.MANUFACTURER
}
