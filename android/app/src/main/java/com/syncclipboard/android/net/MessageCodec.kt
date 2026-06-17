package com.syncclipboard.android.net

import com.google.gson.Gson
import com.google.gson.JsonObject

/**
 * Lightweight v2 message helpers. We deliberately keep parsing dynamic via
 * [JsonObject] because the existing service code still routes by reading
 * specific fields per type; this keeps the migration small.
 */
object MessageCodec {

    private val gson = Gson()

    fun parse(text: String): JsonObject? = try {
        gson.fromJson(text, JsonObject::class.java)
    } catch (_: Exception) { null }

    fun toJson(obj: JsonObject): String = gson.toJson(obj)

    fun message(type: String, build: JsonObject.() -> Unit = {}): String {
        val o = JsonObject().apply {
            addProperty("type", type)
            build()
        }
        return gson.toJson(o)
    }
}
