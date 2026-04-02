package com.syncclipboard.android.service

import android.app.Activity
import android.content.ClipboardManager
import android.os.Bundle
import android.view.WindowManager
import android.widget.FrameLayout
import com.syncclipboard.android.util.AppLog

class ClipboardReaderActivity : Activity() {

    companion object {
        private const val TAG = "ClipboardReader"
    }

    private var hasRead = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(
            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM
        )
        setContentView(FrameLayout(this))
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && !hasRead) {
            hasRead = true
            readClipboard()
            finish()
            @Suppress("DEPRECATION")
            overridePendingTransition(0, 0)
        }
    }

    private fun readClipboard() {
        try {
            val cm = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
            val clip = cm.primaryClip
            if (clip == null || clip.itemCount == 0) {
                AppLog.w(TAG, "primaryClip 仍为 null (hasFocus=true)")
                return
            }

            val item = clip.getItemAt(0)
            val uri = item.uri
            if (uri != null) {
                val mimeType = contentResolver.getType(uri)
                if (mimeType?.startsWith("image/") == true) {
                    try {
                        val data = contentResolver.openInputStream(uri)?.use { it.readBytes() }
                        if (data != null && data.isNotEmpty()) {
                            AppLog.i(TAG, "读取图片成功: $mimeType, ${data.size} bytes")
                            ClipboardService.instance?.sendImageFromExternal(data, mimeType)
                            return
                        }
                    } catch (e: Exception) {
                        AppLog.w(TAG, "读取图片失败: ${e.message}")
                    }
                }
            }

            val text = item.text?.toString()
            if (!text.isNullOrEmpty()) {
                AppLog.i(TAG, "读取文本成功: ${text.length} chars")
                val svc = ClipboardService.instance
                if (svc != null) {
                    svc.sendClipboardFromExternal(text)
                } else {
                    ClipboardAccessibilityService.instance?.handleExternalClipboard(text)
                }
            } else {
                AppLog.w(TAG, "text 为空")
            }
        } catch (e: Exception) {
            AppLog.e(TAG, "读取失败", e)
        }
    }
}
