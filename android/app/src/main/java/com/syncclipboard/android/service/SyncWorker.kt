package com.syncclipboard.android.service

import android.content.Context
import android.util.Log
import androidx.work.CoroutineWorker
import androidx.work.WorkerParameters
import com.google.gson.Gson
import com.google.gson.JsonObject
import com.syncclipboard.android.util.ClipboardCache
import com.syncclipboard.android.util.ConfigManager
import com.syncclipboard.android.util.CryptoUtil
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import okhttp3.*
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume

class SyncWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {

    companion object {
        private const val TAG = "SyncWorker"
    }

    private val gson = Gson()

    override suspend fun doWork(): Result {
        Log.i(TAG, "[SYNC] WorkManager 定时同步开始")
        val config = ConfigManager(applicationContext)

        val result = withTimeoutOrNull(25_000L) {
            suspendCancellableCoroutine { cont ->
                val client = OkHttpClient.Builder()
                    .connectTimeout(10, TimeUnit.SECONDS)
                    .readTimeout(15, TimeUnit.SECONDS)
                    .pingInterval(10, TimeUnit.SECONDS)
                    .build()

                val request = Request.Builder().url(config.getWebSocketUrl()).build()
                var resumed = false

                fun finish(r: Result) {
                    if (!resumed) {
                        resumed = true
                        cont.resume(r)
                    }
                }

                val socket = client.newWebSocket(request, object : WebSocketListener() {
                    override fun onOpen(webSocket: WebSocket, response: Response) {
                        Log.i(TAG, "[SYNC] 已连接，发送认证")
                        val auth = JsonObject().apply {
                            addProperty("type", "auth")
                            addProperty("serverKey", config.serverKey)
                        }
                        webSocket.send(gson.toJson(auth))
                    }

                    override fun onMessage(webSocket: WebSocket, text: String) {
                        try {
                            val json = gson.fromJson(text, JsonObject::class.java)
                            when (json.get("type")?.asString) {
                                "auth_result" -> {
                                    if (json.get("success")?.asBoolean == true) {
                                        Log.i(TAG, "[SYNC] 认证成功，发送登录")
                                        val login = JsonObject().apply {
                                            addProperty("type", "login")
                                            addProperty("username", config.username)
                                            addProperty("password", config.password)
                                            addProperty("deviceId", config.deviceId)
                                        }
                                        webSocket.send(gson.toJson(login))
                                    } else {
                                        webSocket.close(1000, "auth_fail")
                                        finish(Result.failure())
                                    }
                                }
                                "login_result" -> {
                                    if (json.get("success")?.asBoolean == true) {
                                        Log.i(TAG, "[SYNC] 登录成功，等待服务器下发缓存")
                                        Thread {
                                            Thread.sleep(3000)
                                            webSocket.close(1000, "sync_done")
                                            finish(Result.success())
                                        }.start()
                                    } else {
                                        webSocket.close(1000, "login_fail")
                                        finish(Result.failure())
                                    }
                                }
                                "clipboard" -> {
                                    val content = json.get("content")?.asString
                                    val from = json.get("from")?.asString ?: "unknown"
                                    if (content != null) {
                                        val decrypted = CryptoUtil.decrypt(content, config.aesKey)
                                        if (decrypted != null) {
                                            ClipboardCache.store(applicationContext, decrypted, from)
                                            Log.i(TAG, "[SYNC] 获取到缓存剪切板 from=$from, len=${decrypted.length}")
                                        }
                                    }
                                    webSocket.close(1000, "got_clipboard")
                                    finish(Result.success())
                                }
                            }
                        } catch (e: Exception) {
                            Log.e(TAG, "[SYNC] 处理消息异常", e)
                        }
                    }

                    override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                        Log.e(TAG, "[SYNC] 连接失败: ${t.message}")
                        finish(Result.retry())
                    }

                    override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                        Log.i(TAG, "[SYNC] 连接关闭 code=$code")
                        finish(Result.success())
                    }
                })

                cont.invokeOnCancellation { socket.cancel() }
            }
        } ?: Result.retry()

        Log.i(TAG, "[SYNC] WorkManager 定时同步结束: $result")
        return result
    }
}
