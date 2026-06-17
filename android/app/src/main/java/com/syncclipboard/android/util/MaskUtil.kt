package com.syncclipboard.android.util

/**
 * Masking helpers used before any sensitive payload is written to logs or
 * surfaced to the user. These are pure best-effort heuristics — never rely
 * on them as the only sensitive-data control.
 */
object MaskUtil {

    private val SENSITIVE_KEYS = setOf(
        "password", "passwd", "pwd",
        "serverkey", "server_key",
        "aeskey", "aes_key",
        "token", "accesstoken", "access_token",
        "refreshtoken", "refresh_token",
        "authorization", "secret"
    )

    /** Replaces a value with a fixed-length placeholder showing its length only. */
    fun maskValue(raw: String?): String =
        if (raw.isNullOrEmpty()) "<empty>" else "<masked:${raw.length}>"

    /**
     * Best-effort scrub of a JSON-like text. Replaces values of any field whose
     * key matches a known sensitive name and shortens any obvious base64 blob
     * over 64 chars to "<base64:N>".
     */
    fun maskJson(json: String?): String {
        if (json.isNullOrEmpty()) return "<empty>"
        val keyValueRegex = Regex("\"(\\w+)\"\\s*:\\s*\"([^\"]*)\"")
        return keyValueRegex.replace(json) { match ->
            val key = match.groupValues[1]
            val value = match.groupValues[2]
            val replacement = when {
                SENSITIVE_KEYS.contains(key.lowercase()) -> "<masked:${value.length}>"
                value.length > 64 -> "<long:${value.length}>"
                else -> value
            }
            "\"$key\":\"$replacement\""
        }
    }

    /** Trims a possibly large blob to a short marker including its length. */
    fun shorten(value: String?, maxLen: Int = 32): String {
        if (value == null) return "<null>"
        if (value.length <= maxLen) return value
        return value.substring(0, maxLen) + "...<+${value.length - maxLen}>"
    }
}
