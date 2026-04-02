package com.syncclipboard.android.util

import com.google.gson.Gson
import com.google.gson.JsonObject
import okhttp3.*
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class WebSocketManager(
    private val config: ConfigManager,
    private val listener: Listener
) {
    interface Listener {
        fun onStatusChanged(status: String)
        fun onClipboardReceived(decryptedContent: String, from: String)
        fun onFileNotify(json: JsonObject) {}
        fun onFilePeerInfo(json: JsonObject) {}
        fun onFileRelayRequest(json: JsonObject) {}
        fun onFileRelayData(json: JsonObject) {}
    }

    companion object {
        private const val TAG = "WebSocketManager"
        const val STATUS_CONNECTING = "connecting"
        const val STATUS_CONNECTED = "connected"
        const val STATUS_AUTH_OK = "auth_ok"
        const val STATUS_LOGGED_IN = "logged_in"
        const val STATUS_DISCONNECTED = "disconnected"
        const val STATUS_AUTH_FAIL = "auth_fail"
        const val STATUS_LOGIN_FAIL = "login_fail"
    }

    private val gson = Gson()
    private var webSocket: WebSocket? = null
    private val shouldReconnect = AtomicBoolean(false)

    @Volatile
    var isLoggedIn = false
        private set

    fun connect() {
        shouldReconnect.set(true)
        doConnect()
    }

    fun disconnect() {
        shouldReconnect.set(false)
        isLoggedIn = false
        webSocket?.close(1000, "Client disconnect")
        webSocket = null
    }

    fun sendClipboard(encryptedContent: String): Boolean {
        if (!isLoggedIn) return false
        val msg = JsonObject().apply {
            addProperty("type", "clipboard")
            addProperty("content", encryptedContent)
        }
        return webSocket?.send(gson.toJson(msg)) ?: false
    }

    fun sendFileOffer(fileId: String, fileName: String, mimeType: String,
                      fileSize: Long, checksum: String, localAddresses: List<String>): Boolean {
        if (!isLoggedIn) return false
        val msg = JsonObject().apply {
            addProperty("type", "file_offer")
            addProperty("fileId", fileId)
            addProperty("fileName", fileName)
            addProperty("mimeType", mimeType)
            addProperty("fileSize", fileSize)
            addProperty("checksum", checksum)
            add("localAddresses", gson.toJsonTree(localAddresses))
        }
        return webSocket?.send(gson.toJson(msg)) ?: false
    }

    fun sendFileRequest(fileId: String, localAddresses: List<String>): Boolean {
        if (!isLoggedIn) return false
        val msg = JsonObject().apply {
            addProperty("type", "file_request")
            addProperty("fileId", fileId)
            add("localAddresses", gson.toJsonTree(localAddresses))
        }
        return webSocket?.send(gson.toJson(msg)) ?: false
    }

    fun sendFileRelayRequest(fileId: String): Boolean {
        if (!isLoggedIn) return false
        val msg = JsonObject().apply {
            addProperty("type", "file_relay")
            addProperty("fileId", fileId)
        }
        return webSocket?.send(gson.toJson(msg)) ?: false
    }

    fun sendFileRelayData(fileId: String, b64Data: String, fileSize: Long, targetDevice: String): Boolean {
        if (!isLoggedIn) return false
        val msg = JsonObject().apply {
            addProperty("type", "file_relay")
            addProperty("fileId", fileId)
            addProperty("fileSize", fileSize)
            addProperty("targetDevice", targetDevice)
            addProperty("data", b64Data)
        }
        return webSocket?.send(gson.toJson(msg)) ?: false
    }

    fun sendTransferResult(fileId: String, method: String, success: Boolean): Boolean {
        val msg = JsonObject().apply {
            addProperty("type", "file_transfer_result")
            addProperty("fileId", fileId)
            addProperty("method", method)
            addProperty("success", success)
        }
        return webSocket?.send(gson.toJson(msg)) ?: false
    }

    private fun doConnect() {
        if (!shouldReconnect.get()) return
        if (webSocket != null) {
            AppLog.i(TAG, "[WS] Connection already exists, skipping doConnect")
            return
        }

        val client = OkHttpClient.Builder()
            .readTimeout(0, TimeUnit.MILLISECONDS)
            .connectTimeout(10, TimeUnit.SECONDS)
            .pingInterval(30, TimeUnit.SECONDS)
            .build()

        val url = config.getWebSocketUrl()
        AppLog.i(TAG, "[CONNECT] 正在连接 $url")
        listener.onStatusChanged(STATUS_CONNECTING)
        val request = Request.Builder().url(url).build()

        webSocket = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                AppLog.i(TAG, "[CONNECT] WebSocket已连接")
                listener.onStatusChanged(STATUS_CONNECTED)
                sendAuth(webSocket)
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                handleMessage(webSocket, text)
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                AppLog.e(TAG, "[ERROR] 连接失败: ${t.message}")
                isLoggedIn = false
                this@WebSocketManager.webSocket = null
                listener.onStatusChanged(STATUS_DISCONNECTED)
                scheduleReconnect()
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                AppLog.i(TAG, "[DISCONNECT] 已关闭 code=$code reason=$reason")
                isLoggedIn = false
                this@WebSocketManager.webSocket = null
                listener.onStatusChanged(STATUS_DISCONNECTED)
                scheduleReconnect()
            }
        })
    }

    private fun scheduleReconnect() {
        if (shouldReconnect.get()) {
            Thread {
                try {
                    Thread.sleep(5000)
                    doConnect()
                } catch (_: InterruptedException) {}
            }.start()
        }
    }

    private fun sendAuth(ws: WebSocket) {
        val msg = JsonObject().apply {
            addProperty("type", "auth")
            addProperty("serverKey", config.serverKey)
        }
        ws.send(gson.toJson(msg))
    }

    private fun sendLogin(ws: WebSocket) {
        val msg = JsonObject().apply {
            addProperty("type", "login")
            addProperty("username", config.username)
            addProperty("password", config.password)
            addProperty("deviceId", config.deviceId)
        }
        ws.send(gson.toJson(msg))
    }

    private fun handleMessage(ws: WebSocket, text: String) {
        try {
            val json = gson.fromJson(text, JsonObject::class.java)
            when (json.get("type")?.asString) {
                "auth_result" -> {
                    if (json.get("success")?.asBoolean == true) {
                        AppLog.i(TAG, "[AUTH] 认证成功")
                        listener.onStatusChanged(STATUS_AUTH_OK)
                        sendLogin(ws)
                    } else {
                        AppLog.e(TAG, "[AUTH] 认证失败: ${json.get("message")?.asString}")
                        listener.onStatusChanged(STATUS_AUTH_FAIL)
                    }
                }
                "login_result" -> {
                    if (json.get("success")?.asBoolean == true) {
                        AppLog.i(TAG, "[LOGIN] 登录成功")
                        isLoggedIn = true
                        listener.onStatusChanged(STATUS_LOGGED_IN)
                    } else {
                        AppLog.e(TAG, "[LOGIN] 登录失败: ${json.get("message")?.asString}")
                        listener.onStatusChanged(STATUS_LOGIN_FAIL)
                    }
                }
                "clipboard" -> {
                    val content = json.get("content")?.asString ?: return
                    val from = json.get("from")?.asString ?: "unknown"
                    AppLog.i(TAG, "[CLIPBOARD] 收到 from=$from, len=${content.length}")
                    val decrypted = CryptoUtil.decrypt(content, config.aesKey)
                    if (decrypted != null) {
                        listener.onClipboardReceived(decrypted, from)
                    } else {
                        AppLog.e(TAG, "[CLIPBOARD] 解密失败")
                    }
                }
                "file_notify" -> listener.onFileNotify(json)
                "file_peer_info" -> listener.onFilePeerInfo(json)
                "file_relay_request" -> listener.onFileRelayRequest(json)
                "file_relay_data" -> listener.onFileRelayData(json)
                "error" -> {
                    AppLog.e(TAG, "[ERROR] 服务器错误: ${json.get("message")?.asString}")
                }
            }
        } catch (e: Exception) {
            AppLog.e(TAG, "[ERROR] 处理消息异常", e)
        }
    }
}
