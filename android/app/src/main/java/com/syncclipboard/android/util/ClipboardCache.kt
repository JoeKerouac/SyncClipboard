package com.syncclipboard.android.util

import android.content.Context

object ClipboardCache {
    private const val PREFS = "clipboard_cache"
    private const val KEY_CONTENT = "content"
    private const val KEY_FROM = "from"
    private const val KEY_TIMESTAMP = "timestamp"

    fun store(context: Context, content: String, from: String) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(KEY_CONTENT, content)
            .putString(KEY_FROM, from)
            .putLong(KEY_TIMESTAMP, System.currentTimeMillis())
            .apply()
    }

    fun get(context: Context): String? {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(KEY_CONTENT, null)
    }

    fun clear(context: Context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().clear().apply()
    }
}
