package com.syncclipboard.android.ui

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.syncclipboard.android.databinding.ActivityMainBinding
import com.syncclipboard.android.service.ClipboardAccessibilityService
import com.syncclipboard.android.service.ClipboardService
import com.syncclipboard.android.util.AutoStartHelper
import com.syncclipboard.android.util.ConfigManager

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var config: ConfigManager
    private var serviceRunning = false

    private val fgStatusReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            val status = intent?.getStringExtra(ClipboardService.EXTRA_STATUS) ?: return
            runOnUiThread {
                updateStatusDisplay(status)
                updateButtonStates()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        config = ConfigManager(this)
        loadConfig()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED
            ) {
                ActivityCompat.requestPermissions(
                    this, arrayOf(Manifest.permission.POST_NOTIFICATIONS), 100
                )
            }
        }

        binding.btnSave.setOnClickListener { saveConfig() }
        binding.btnStart.setOnClickListener { startForegroundService() }
        binding.btnStop.setOnClickListener { stopForegroundService() }
        binding.btnA11ySettings.setOnClickListener { openAccessibilitySettings() }
        binding.btnBattery.setOnClickListener { requestIgnoreBatteryOptimization() }
        binding.btnAutoStart.setOnClickListener { openAutoStartSettings() }
        binding.btnGuide.setOnClickListener { showKeepAliveGuide() }
        binding.btnLog.setOnClickListener {
            startActivity(Intent(this, LogViewerActivity::class.java))
        }

        registerReceiver(
            fgStatusReceiver,
            IntentFilter(ClipboardService.ACTION_STATUS_CHANGED),
            RECEIVER_NOT_EXPORTED
        )
    }

    override fun onResume() {
        super.onResume()
        refreshA11yStatus()
        refreshBatteryStatus()
        restoreServiceStatus()
        updateButtonStates()
    }

    override fun onDestroy() {
        unregisterReceiver(fgStatusReceiver)
        super.onDestroy()
    }

    private fun loadConfig() {
        binding.etServerHost.setText(config.serverHost)
        binding.etServerPort.setText(config.serverPort.toString())
        binding.etServerKey.setText(config.serverKey)
        binding.etUsername.setText(config.username)
        binding.etPassword.setText(config.password)
        binding.etAesKey.setText(config.aesKey)
        binding.etDeviceId.setText(config.deviceId)
        binding.spinnerFileLevel.setSelection(config.fileTransferLevel)
    }

    private fun saveConfig() {
        config.serverHost = binding.etServerHost.text.toString().trim()
        config.serverPort = binding.etServerPort.text.toString().trim().toIntOrNull() ?: 8080
        config.serverKey = binding.etServerKey.text.toString().trim()
        config.username = binding.etUsername.text.toString().trim()
        config.password = binding.etPassword.text.toString().trim()
        config.aesKey = binding.etAesKey.text.toString().trim()
        config.deviceId = binding.etDeviceId.text.toString().trim()
        config.fileTransferLevel = binding.spinnerFileLevel.selectedItemPosition
        Toast.makeText(this, "配置已保存", Toast.LENGTH_SHORT).show()
    }

    // ---- 无障碍服务 ----

    private fun isAccessibilityServiceEnabled(): Boolean {
        val service = "${packageName}/${ClipboardAccessibilityService::class.java.canonicalName}"
        val enabledServices = Settings.Secure.getString(
            contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES
        ) ?: return false
        return enabledServices.split(":").any { it.equals(service, ignoreCase = true) }
    }

    private fun refreshA11yStatus() {
        val enabled = isAccessibilityServiceEnabled()
        binding.tvA11yStatus.text = if (enabled) "无障碍服务：已开启 ✓" else "无障碍服务：未开启"
    }

    private fun openAccessibilitySettings() {
        saveConfig()
        startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
    }

    // ---- 电池优化 ----

    private fun isIgnoringBatteryOptimizations(): Boolean {
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        return pm.isIgnoringBatteryOptimizations(packageName)
    }

    private fun refreshBatteryStatus() {
        val ignored = isIgnoringBatteryOptimizations()
        binding.tvBatteryStatus.text = if (ignored) "电池优化：已关闭 ✓" else "电池优化：未关闭（建议关闭）"
    }

    @Suppress("BatteryLife")
    private fun requestIgnoreBatteryOptimization() {
        if (isIgnoringBatteryOptimizations()) {
            Toast.makeText(this, "已关闭电池优化", Toast.LENGTH_SHORT).show()
            return
        }
        val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
            data = Uri.parse("package:$packageName")
        }
        startActivity(intent)
    }

    // ---- 自启动管理 ----

    private fun openAutoStartSettings() {
        val opened = AutoStartHelper.openAutoStartSettings(this)
        if (!opened) {
            Toast.makeText(this, "未找到自启动管理，已跳转应用详情", Toast.LENGTH_SHORT).show()
        }
    }

    // ---- 保活指南 ----

    private fun showKeepAliveGuide() {
        val manufacturer = AutoStartHelper.getManufacturer()
        val isHonor = AutoStartHelper.isHonorOrHuawei()

        val sb = StringBuilder()
        sb.appendLine("当前设备：$manufacturer")
        sb.appendLine()

        if (isHonor) {
            sb.appendLine("【荣耀/华为设备 必做步骤】")
            sb.appendLine()
            sb.appendLine("步骤 1：开启自启动")
            sb.appendLine("设置 → 应用和服务 → 应用启动管理")
            sb.appendLine("→ 找到 SyncClipboard → 关闭「自动管理」")
            sb.appendLine("→ 手动开启「自启动」「关联启动」「后台活动」三个开关")
            sb.appendLine()
            sb.appendLine("步骤 2：关闭电池优化")
            sb.appendLine("设置 → 电池 → 更多电池设置")
            sb.appendLine("→ 关闭「休眠时始终保持网络连接」以外的省电项")
            sb.appendLine("或：设置 → 应用 → SyncClipboard → 电池 → 选择「不受限制」")
            sb.appendLine()
            sb.appendLine("步骤 3：锁定最近任务")
            sb.appendLine("从底部上划打开最近任务 → 找到 SyncClipboard")
            sb.appendLine("→ 长按或下拉卡片 → 点击锁定图标")
            sb.appendLine("→ 锁定后清理后台时不会被清除")
            sb.appendLine()
            sb.appendLine("步骤 4：开启无障碍服务")
            sb.appendLine("设置 → 辅助功能 → 已安装的服务/无障碍")
            sb.appendLine("→ 找到 SyncClipboard → 开启")
            sb.appendLine()
            sb.appendLine("⚠ 以上步骤缺一不可！")
            sb.appendLine("荣耀系统会在清理后台时主动关闭无障碍服务，")
            sb.appendLine("只有完成「应用启动管理」+「锁定任务」后才能避免。")
        } else {
            sb.appendLine("【通用保活步骤】")
            sb.appendLine()
            sb.appendLine("1. 开启无障碍服务")
            sb.appendLine("2. 关闭电池优化（点击上方「电池优化」按钮）")
            sb.appendLine("3. 允许自启动（点击上方「自启动管理」按钮）")
            sb.appendLine("4. 在最近任务中锁定本应用（长按/下拉卡片）")
        }

        AlertDialog.Builder(this)
            .setTitle("后台保活设置指南")
            .setMessage(sb.toString())
            .setPositiveButton("我知道了", null)
            .setNeutralButton("打开应用启动管理") { _, _ -> openAutoStartSettings() }
            .show()
    }

    // ---- 按钮状态 ----

    private fun updateButtonStates() {
        val running = serviceRunning || ClipboardService.isRunning
        binding.btnStart.isEnabled = !running
        binding.btnStop.isEnabled = running
        binding.btnStart.alpha = if (running) 0.5f else 1.0f
        binding.btnStop.alpha = if (running) 1.0f else 0.5f
    }

    // ---- 前台服务 ----

    private fun startForegroundService() {
        if (serviceRunning || ClipboardService.isRunning) {
            Toast.makeText(this, "同步已在运行", Toast.LENGTH_SHORT).show()
            return
        }
        saveConfig()
        val intent = Intent(this, ClipboardService::class.java)
        ContextCompat.startForegroundService(this, intent)
        serviceRunning = true
        binding.tvStatus.text = "状态：前台服务启动中..."
        updateButtonStates()
    }

    private fun stopForegroundService() {
        stopService(Intent(this, ClipboardService::class.java))
        serviceRunning = false
        binding.tvStatus.text = "状态：已停止"
        updateButtonStates()
    }

    // ---- 状态恢复 ----

    private fun restoreServiceStatus() {
        if (ClipboardService.isRunning) {
            serviceRunning = true
            val status = config.lastWsStatus
            if (status.isNotEmpty()) {
                updateStatusDisplay(status)
            } else {
                binding.tvStatus.text = "状态：同步服务运行中..."
            }
        }
    }

    // ---- 状态显示 ----

    private fun updateStatusDisplay(status: String) {
        binding.tvStatus.text = when (status) {
            "connecting" -> "状态：正在连接服务器..."
            "connected" -> "状态：已连接，正在认证..."
            "auth_ok" -> "状态：认证成功，正在登录..."
            "logged_in" -> "状态：同步中 ✓"
            "disconnected" -> "状态：连接断开，等待重连..."
            "auth_fail" -> "状态：认证失败（检查 Server Key）"
            "login_fail" -> "状态：登录失败（检查用户名密码）"
            else -> "状态：$status"
        }
    }
}
