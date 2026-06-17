package com.syncclipboard.android.service

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.AccessibilityServiceInfo
import android.content.ClipboardManager
import android.content.Intent
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import com.syncclipboard.android.util.AppLog

/**
 * Accessibility service for clipboard detection only.
 * Does NOT manage WebSocket connections — all network operations
 * are delegated to ClipboardService via its static instance.
 *
 * 仅依赖被动回调：OnPrimaryClipChangedListener + AccessibilityEvent。
 * 已移除主动轮询，避免在 Android 12+ 触发"xxx 已访问剪贴板"系统横幅。
 */
class ClipboardAccessibilityService : AccessibilityService() {

    companion object {
        private const val TAG = "ClipboardA11y"
        private const val READER_COOLDOWN_MS = 2000L

        @Volatile
        var isRunning = false
            private set

        @Volatile
        var instance: ClipboardAccessibilityService? = null
            private set
    }

    private lateinit var clipboardManager: ClipboardManager
    private var lastClipHash = ""
    private var lastReaderLaunchTime = 0L
    private val handler = Handler(Looper.getMainLooper())

    private val clipListener = ClipboardManager.OnPrimaryClipChangedListener {
        AppLog.d(TAG, "[LISTENER] 剪切板变化回调触发")
        onClipboardChanged("LISTENER")
    }

    override fun onServiceConnected() {
        super.onServiceConnected()
        instance = this
        isRunning = true
        clipboardManager = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager

        serviceInfo = serviceInfo.apply {
            eventTypes = AccessibilityEvent.TYPE_ANNOUNCEMENT or
                    AccessibilityEvent.TYPE_NOTIFICATION_STATE_CHANGED
            feedbackType = AccessibilityServiceInfo.FEEDBACK_GENERIC
            flags = flags or AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
            notificationTimeout = 100
        }

        clipboardManager.addPrimaryClipChangedListener(clipListener)
        AppLog.i(TAG, "无障碍服务已启动 (被动监听模式)")
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        if (event == null) return
        val eventType = event.eventType
        if (eventType != AccessibilityEvent.TYPE_ANNOUNCEMENT &&
            eventType != AccessibilityEvent.TYPE_NOTIFICATION_STATE_CHANGED
        ) return

        val texts = event.text.joinToString(" ").lowercase()
        if (texts.isEmpty()) return
        if (texts.contains("syncclipboard")) return

        if (texts.contains("复制") || texts.contains("已复制") ||
            texts.contains("剪切") || texts.contains("已剪切") ||
            texts.contains("copy") || texts.contains("copied") ||
            texts.contains("cut") ||
            texts.contains("clipboard") || texts.contains("剪切板") ||
            texts.contains("剪贴板")
        ) {
            AppLog.d(TAG, "[EVENT] 检测到复制/剪切事件: \"$texts\"")
            handler.postDelayed({
                // 优先从无障碍节点选区读取，避免读 primaryClip 触发隐私横幅
                if (readSelectedTextFromWindow()) return@postDelayed
                val directClip = try { clipboardManager.primaryClip } catch (_: Exception) { null }
                if (directClip != null && directClip.itemCount > 0) {
                    val text = directClip.getItemAt(0).text?.toString()
                    if (!text.isNullOrEmpty()) {
                        forwardToClipboardService(text, "EVENT")
                        return@postDelayed
                    }
                }
                launchClipboardReader("EVENT")
            }, 150)
        }
    }

    override fun onInterrupt() {}

    override fun onDestroy() {
        instance = null
        isRunning = false
        handler.removeCallbacksAndMessages(null)
        try { clipboardManager.removePrimaryClipChangedListener(clipListener) } catch (_: Exception) {}
        AppLog.i(TAG, "无障碍服务已停止")
        super.onDestroy()
    }

    private fun onClipboardChanged(source: String) {
        try {
            val clip = clipboardManager.primaryClip
            if (clip != null && clip.itemCount > 0) {
                val item = clip.getItemAt(0)
                val text = item.text?.toString()
                if (!text.isNullOrEmpty()) {
                    forwardToClipboardService(text, source)
                    return
                }
                if (item.uri != null) {
                    AppLog.d(TAG, "[$source] 剪切板包含URI(图片/文件)")
                    forwardImageToClipboardService(item.uri, source)
                    return
                }
            }
            if (!readSelectedTextFromWindow()) {
                launchClipboardReader(source)
            }
        } catch (e: SecurityException) {
            if (!readSelectedTextFromWindow()) {
                launchClipboardReader(source)
            }
        } catch (_: Exception) {}
    }

    fun handleExternalClipboard(text: String) {
        AppLog.d(TAG, "[READER] 透明Activity回传: ${text.length} chars")
        forwardToClipboardService(text, "READER")
    }

    /**
     * Forward detected clipboard text to ClipboardService for network delivery.
     * This service never manages its own WebSocket connection.
     */
    private fun forwardToClipboardService(text: String, source: String) {
        val hash = text.hashCode().toString()
        if (hash == lastClipHash) return
        lastClipHash = hash

        val svc = ClipboardService.instance
        if (svc == null) {
            AppLog.d(TAG, "[$source] ClipboardService未运行, 跳过")
            return
        }
        AppLog.i(TAG, "[$source] 转发到ClipboardService len=${text.length}")
        svc.sendClipboardFromExternal(text)
    }

    private fun forwardImageToClipboardService(uri: Uri, source: String) {
        try {
            val mimeType = contentResolver.getType(uri)
            if (mimeType?.startsWith("image/") != true) return
            val data = contentResolver.openInputStream(uri)?.use { it.readBytes() }
            if (data == null || data.isEmpty()) return
            val svc = ClipboardService.instance ?: return
            AppLog.i(TAG, "[$source] 转发图片到ClipboardService: $mimeType, ${data.size} bytes")
            svc.sendImageFromExternal(data, mimeType)
        } catch (e: Exception) {
            AppLog.d(TAG, "[$source] 转发图片失败: ${e.message}")
        }
    }

    private fun readSelectedTextFromWindow(): Boolean {
        try {
            val root = rootInActiveWindow ?: return false
            val result = findSelectedText(root)
            root.recycle()
            if (!result.isNullOrEmpty()) {
                forwardToClipboardService(result, "A11Y-NODE")
                return true
            }
        } catch (_: Exception) {}
        return false
    }

    private fun findSelectedText(node: AccessibilityNodeInfo): String? {
        try {
            val text = node.text
            if (text != null) {
                val start = node.textSelectionStart
                val end = node.textSelectionEnd
                if (start >= 0 && end > start && end <= text.length) {
                    return text.substring(start, end)
                }
            }
            for (i in 0 until node.childCount) {
                val child = node.getChild(i) ?: continue
                val found = findSelectedText(child)
                child.recycle()
                if (found != null) return found
            }
        } catch (_: Exception) {}
        return null
    }

    private fun launchClipboardReader(source: String) {
        val now = System.currentTimeMillis()
        if (now - lastReaderLaunchTime < READER_COOLDOWN_MS) return
        lastReaderLaunchTime = now
        try {
            val intent = Intent(this, ClipboardReaderActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
            }
            startActivity(intent)
        } catch (_: Exception) {}
    }
}
