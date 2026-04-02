package com.syncclipboard.android.util

import android.util.Log
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object AppLog {
    private val buffer = ArrayDeque<String>()
    private const val MAX_SIZE = 500
    private val timeFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault())

    fun i(tag: String, msg: String) {
        add("I", tag, msg)
        Log.i(tag, msg)
    }

    fun w(tag: String, msg: String) {
        add("W", tag, msg)
        Log.w(tag, msg)
    }

    fun d(tag: String, msg: String) {
        add("D", tag, msg)
        Log.d(tag, msg)
    }

    fun e(tag: String, msg: String, t: Throwable? = null) {
        add("E", tag, if (t != null) "$msg [${t.javaClass.simpleName}: ${t.message}]" else msg)
        if (t != null) Log.e(tag, msg, t) else Log.e(tag, msg)
    }

    private fun add(level: String, tag: String, msg: String) {
        val time = timeFormat.format(Date())
        val line = "$time $level/$tag: $msg"
        synchronized(buffer) {
            if (buffer.size >= MAX_SIZE) buffer.removeFirst()
            buffer.addLast(line)
        }
    }

    fun getAll(): String {
        synchronized(buffer) {
            return buffer.joinToString("\n")
        }
    }

    fun clear() {
        synchronized(buffer) { buffer.clear() }
    }
}
