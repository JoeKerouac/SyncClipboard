package com.syncclipboard.android.service

import android.app.*
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.MediaStore
import android.util.Log
import com.google.gson.Gson
import com.google.gson.JsonObject
import com.syncclipboard.android.ui.MainActivity
import com.syncclipboard.android.util.AppLog
import com.syncclipboard.android.util.ConfigManager
import com.syncclipboard.android.util.CryptoUtil
import com.syncclipboard.android.util.FileTransferManager
import okhttp3.*
import java.util.concurrent.TimeUnit

class ClipboardService : Service(), FileTransferManager.Listener {

    companion object {
        private const val TAG = "ClipboardService"
        private const val CHANNEL_ID = "sync_clipboard_channel"
        private const val NOTIFICATION_ID = 1
        const val ACTION_STATUS_CHANGED = "com.syncclipboard.STATUS_CHANGED"
        const val EXTRA_STATUS = "status"
        const val STATUS_CONNECTING = "connecting"
        const val STATUS_CONNECTED = "connected"
        const val STATUS_AUTH_OK = "auth_ok"
        const val STATUS_LOGGED_IN = "logged_in"
        const val STATUS_DISCONNECTED = "disconnected"
        const val STATUS_AUTH_FAIL = "auth_fail"
        const val STATUS_LOGIN_FAIL = "login_fail"

        @Volatile
        var isRunning = false
            private set

        @Volatile
        var instance: ClipboardService? = null
            private set
    }

    private lateinit var config: ConfigManager
    private lateinit var clipboardManager: ClipboardManager
    private val gson = Gson()
    private lateinit var ftManager: FileTransferManager
    private val handler = Handler(Looper.getMainLooper())
    private var webSocket: WebSocket? = null
    private var isAuthenticated = false
    private var isLoggedIn = false
    private var suppressNext = false
    private var lastClipHash = ""
    private var lastImgHash = ""
    @Volatile private var shouldReconnect = true
    private var serverFileLevel = 3
    private var lastReaderLaunchTime = 0L

    private val clipboardListener = ClipboardManager.OnPrimaryClipChangedListener {
        onClipboardChanged()
    }

    override fun onCreate() {
        super.onCreate()
        config = ConfigManager(this)
        ftManager = FileTransferManager(cacheDir, config.maxTransferSizeMb.toLong() * 1024 * 1024)
        clipboardManager = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        instance = this
        isRunning = true
        shouldReconnect = true
        startForeground(NOTIFICATION_ID, buildNotification("正在连接..."))
        clipboardManager.addPrimaryClipChangedListener(clipboardListener)
        connectWebSocket()
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        instance = null
        isRunning = false
        shouldReconnect = false
        clipboardManager.removePrimaryClipChangedListener(clipboardListener)
        webSocket?.close(1000, "Service stopped")
        ftManager.cleanup()
        super.onDestroy()
    }

    private fun connectWebSocket() {
        val client = OkHttpClient.Builder()
            .readTimeout(0, TimeUnit.MILLISECONDS)
            .connectTimeout(10, TimeUnit.SECONDS)
            .pingInterval(30, TimeUnit.SECONDS)
            .build()

        val url = config.getWebSocketUrl()
        Log.i(TAG, "[CONNECT] 正在连接 $url")
        broadcastStatus(STATUS_CONNECTING)
        val request = Request.Builder().url(url).build()

        webSocket = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(ws: WebSocket, response: Response) {
                Log.i(TAG, "[CONNECT] WebSocket已连接 response=${response.code}")
                broadcastStatus(STATUS_CONNECTED)
                sendAuth(ws)
            }

            override fun onMessage(ws: WebSocket, text: String) {
                Log.d(TAG, "[RECV] $text")
                handleMessage(ws, text)
            }

            override fun onFailure(ws: WebSocket, t: Throwable, response: Response?) {
                Log.e(TAG, "[ERROR] WebSocket连接失败: ${t.javaClass.simpleName}: ${t.message}, response=${response?.code}")
                isAuthenticated = false
                isLoggedIn = false
                broadcastStatus(STATUS_DISCONNECTED)
                if (shouldReconnect) {
                    Thread {
                        Thread.sleep(5000)
                        if (shouldReconnect) connectWebSocket()
                    }.start()
                }
            }

            override fun onClosed(ws: WebSocket, code: Int, reason: String) {
                Log.i(TAG, "[DISCONNECT] WebSocket已关闭 code=$code, reason=$reason")
                isAuthenticated = false
                isLoggedIn = false
                broadcastStatus(STATUS_DISCONNECTED)
                if (shouldReconnect) {
                    Thread {
                        Thread.sleep(5000)
                        if (shouldReconnect) connectWebSocket()
                    }.start()
                }
            }
        })
    }

    private fun sendAuth(ws: WebSocket) {
        val msg = JsonObject().apply {
            addProperty("type", "auth")
            addProperty("serverKey", config.serverKey)
        }
        val json = gson.toJson(msg)
        Log.i(TAG, "[AUTH] 发送认证请求")
        ws.send(json)
    }

    private fun sendLogin(ws: WebSocket) {
        val msg = JsonObject().apply {
            addProperty("type", "login")
            addProperty("username", config.username)
            addProperty("password", config.password)
            addProperty("deviceId", config.deviceId)
        }
        val json = gson.toJson(msg)
        Log.i(TAG, "[LOGIN] 发送登录请求 user=${config.username}, device=${config.deviceId}")
        ws.send(json)
    }

    private fun handleMessage(ws: WebSocket, text: String) {
        try {
            val json = gson.fromJson(text, JsonObject::class.java)
            val type = json.get("type")?.asString
            Log.i(TAG, "[RECV] type=$type")
            when (type) {
                "auth_result" -> {
                    if (json.get("success")?.asBoolean == true) {
                        Log.i(TAG, "[AUTH] 认证成功")
                        isAuthenticated = true
                        broadcastStatus(STATUS_AUTH_OK)
                        sendLogin(ws)
                    } else {
                        val reason = json.get("message")?.asString
                        Log.e(TAG, "[AUTH] 认证失败: $reason")
                        broadcastStatus(STATUS_AUTH_FAIL)
                    }
                }
                "login_result" -> {
                    if (json.get("success")?.asBoolean == true) {
                        Log.i(TAG, "[LOGIN] 登录成功")
                        serverFileLevel = json.get("fileTransferLevel")?.asInt ?: 3
                        isLoggedIn = true
                        broadcastStatus(STATUS_LOGGED_IN)
                    } else {
                        val reason = json.get("message")?.asString
                        Log.e(TAG, "[LOGIN] 登录失败: $reason")
                        broadcastStatus(STATUS_LOGIN_FAIL)
                    }
                }
                "clipboard" -> {
                    val content = json.get("content")?.asString ?: return
                    val from = json.get("from")?.asString ?: "unknown"
                    Log.i(TAG, "[CLIPBOARD] 收到剪切板 from=$from, contentLen=${content.length}")
                    val decrypted = CryptoUtil.decrypt(content, config.aesKey)
                    if (decrypted != null) {
                        Log.i(TAG, "[CLIPBOARD] 解密成功 (${decrypted.length} chars), 写入剪切板")
                        suppressNext = true
                        clipboardManager.setPrimaryClip(
                            ClipData.newPlainText("SyncClipboard", decrypted)
                        )
                        lastClipHash = decrypted.hashCode().toString()
                    } else {
                        Log.e(TAG, "[CLIPBOARD] 解密失败，密钥可能不一致")
                    }
                }
                "file_notify" -> handleFileNotify(json)
                "file_peer_info" -> handleFilePeerInfo(json)
                "file_relay_request" -> handleFileRelayRequest(json)
                "file_relay_data" -> handleFileRelayData(json)
                "error" -> {
                    val reason = json.get("message")?.asString
                    Log.e(TAG, "[ERROR] 服务器返回错误: $reason")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "[ERROR] 处理消息异常", e)
        }
    }

    // ---- file transfer ----

    private fun handleFileNotify(json: JsonObject) {
        if (config.fileTransferLevel <= 0) return
        transferStarted = false
        val fileId = json.get("fileId")?.asString ?: return
        val fileName = json.get("fileName")?.asString ?: "file"
        val mimeType = json.get("mimeType")?.asString ?: "application/octet-stream"
        val fileSize = json.get("fileSize")?.asLong ?: 0
        val checksum = json.get("checksum")?.asString ?: ""
        val maxRelay = json.get("maxRelaySize")?.asLong ?: 0
        val udpPort = json.get("udpPort")?.asInt ?: 0
        val from = json.get("from")?.asString ?: ""
        AppLog.i(TAG, "[FILE-NOTIFY] $fileName ($fileSize bytes) from $from")
        val state = ftManager.startReceiverRequest(fileId, fileName, mimeType, fileSize, checksum, maxRelay, udpPort, from)
        state.aesKey = config.aesKey
        val addrs = ftManager.getLocalAddresses(state.listenPort)
        val req = JsonObject().apply {
            addProperty("type", "file_request")
            addProperty("fileId", fileId)
            add("localAddresses", gson.toJsonTree(addrs))
        }
        webSocket?.send(gson.toJson(req))
    }

    private var transferStarted = false

    private fun handleFilePeerInfo(json: JsonObject) {
        val state = ftManager.currentState ?: return
        if (transferStarted) {
            AppLog.i(TAG, "[PEER-INFO] 忽略重复peer_info(传输已启动)")
            return
        }
        state.peerAddresses = json.getAsJsonArray("peerLocalAddresses")?.map { it.asString } ?: emptyList()
        state.peerPublicAddress = json.get("peerPublicAddress")?.asString ?: ""
        val peerFileLevel = json.get("fileTransferLevel")?.asInt ?: 3
        state.fileTransferLevel = minOf(config.fileTransferLevel, serverFileLevel, peerFileLevel)
        state.sameLan = json.get("sameLan")?.asBoolean ?: false
        AppLog.i(TAG, "[PEER-INFO] addrs=${state.peerAddresses}, public=${state.peerPublicAddress}, level=${state.fileTransferLevel}, sameLan=${state.sameLan}")
        transferStarted = true
        ftManager.startTransfer(state, config.serverHost, this)
    }

    private fun handleFileRelayRequest(json: JsonObject) {
        val requesterId = json.get("requesterId")?.asString ?: return
        val state = ftManager.currentState ?: return
        val data = state.data ?: return
        AppLog.i(TAG, "[RELAY-REQ] 发送中转数据给 $requesterId")
        onSendRelayData(state.fileId, data, state.fileSize, requesterId)
    }

    private fun handleFileRelayData(json: JsonObject) {
        val fileName = json.get("fileName")?.asString ?: "file"
        val mimeType = json.get("mimeType")?.asString ?: "application/octet-stream"
        val b64 = json.get("data")?.asString ?: return
        AppLog.i(TAG, "[RELAY-DATA] 收到中转数据 $fileName")
        ftManager.handleRelayData(b64, fileName, mimeType, this)
    }

    private fun checkAndSendImageClipboard(): Boolean {
        if (config.fileTransferLevel <= 0 || serverFileLevel <= 0) return false
        try {
            val clip = clipboardManager.primaryClip ?: return false
            val item = clip.getItemAt(0) ?: return false
            val uri = item.uri

            var mime: String? = null
            if (clip.description.hasMimeType("image/*")) {
                mime = if (uri != null) contentResolver.getType(uri) ?: "image/png" else "image/png"
            } else if (uri != null) {
                val uriMime = contentResolver.getType(uri)
                if (uriMime?.startsWith("image/") == true) mime = uriMime
            }
            if (mime == null) return false

            val stream = if (uri != null) contentResolver.openInputStream(uri) else null
            if (stream == null) return false
            val data = stream.readBytes()
            stream.close()
            if (data.isEmpty()) return false

            val imgHash = ftManager.sha256(data)
            if (imgHash == lastImgHash) return false
            lastImgHash = imgHash

            val ext = if (mime.contains("jpeg") || mime.contains("jpg")) ".jpg" else ".png"
            val fileName = "clipboard$ext"
            Log.i(TAG, "[IMAGE] 检测到图片 $mime, ${data.size} bytes")
            val fileId = ftManager.generateFileId()
            transferStarted = false
            val info = FileTransferManager.FileInfo(fileId, fileName, mime, data.size.toLong(), imgHash, data)
            val state = ftManager.startSenderOffer(info)
            state.aesKey = config.aesKey
            val addrs = ftManager.getLocalAddresses(state.listenPort)
            val msg = JsonObject().apply {
                addProperty("type", "file_offer")
                addProperty("fileId", fileId)
                addProperty("fileName", fileName)
                addProperty("mimeType", mime)
                addProperty("fileSize", info.fileSize)
                addProperty("checksum", imgHash)
                add("localAddresses", gson.toJsonTree(addrs))
            }
            webSocket?.send(gson.toJson(msg))
            return true
        } catch (e: Exception) {
            Log.d(TAG, "[IMAGE] 检测异常: ${e.message}")
            return false
        }
    }

    // ---- FileTransferManager.Listener ----

    private fun mimeToExtension(mime: String): String = when {
        mime.contains("png") -> ".png"
        mime.contains("jpeg") || mime.contains("jpg") -> ".jpg"
        mime.contains("gif") -> ".gif"
        mime.contains("webp") -> ".webp"
        mime.contains("pdf") -> ".pdf"
        else -> ""
    }

    override fun onFileReceived(file: java.io.File, fileName: String, mimeType: String, method: String,
                                connectionMs: Long, transferMs: Long) {
        AppLog.i(TAG, "[FILE-RECV] $fileName ($mimeType, ${file.length()} bytes) via $method, 连接${connectionMs}ms 传输${transferMs}ms")
        handler.post {
            try {
                val isImage = mimeType.startsWith("image/")
                var clipUri: Uri? = null
                var clipMime = mimeType

                if (isImage) {
                    clipUri = saveImageToMediaStore(file)
                    if (clipUri != null) clipMime = "image/png"
                }

                suppressNext = true
                if (clipUri != null) {
                    val clipData = ClipData.newUri(contentResolver, "SyncClipboard", clipUri)
                    clipboardManager.setPrimaryClip(clipData)
                    Log.i(TAG, "[FILE-RECV] 图片已保存到MediaStore并写入剪切板: $clipUri")
                } else {
                    val ext = mimeToExtension(mimeType)
                    val safeName = if (ext.isNotEmpty() && !fileName.endsWith(ext))
                        fileName.substringBeforeLast(".") + ext else fileName
                    val tmpFile = java.io.File(cacheDir, "syncclip_$safeName")
                    if (file.absolutePath != tmpFile.absolutePath) file.renameTo(tmpFile)
                    val uri = androidx.core.content.FileProvider.getUriForFile(
                        this, "$packageName.fileprovider", tmpFile)
                    val clipData = ClipData(
                        "SyncClipboard", arrayOf(clipMime),
                        ClipData.Item(uri)
                    )
                    clipboardManager.setPrimaryClip(clipData)
                    Log.i(TAG, "[FILE-RECV] 已写入剪切板: $safeName ($clipMime)")
                }

                val result = JsonObject().apply {
                    addProperty("type", "file_transfer_result")
                    addProperty("fileId", ftManager.currentState?.fileId ?: "")
                    addProperty("method", method)
                    addProperty("success", true)
                    addProperty("connectionMs", connectionMs)
                    addProperty("transferMs", transferMs)
                }
                webSocket?.send(gson.toJson(result))
            } catch (e: Exception) {
                AppLog.e(TAG, "[FILE-RECV] 写入失败", e)
            }
        }
    }

    private fun saveImageToMediaStore(file: java.io.File): Uri? {
        try {
            var bitmap = BitmapFactory.decodeFile(file.absolutePath)
            if (bitmap == null) {
                AppLog.w(TAG, "[FILE-RECV] BitmapFactory无法解码, 尝试手动BMP解码")
                bitmap = decodeBmpFallback(file)
            }
            if (bitmap == null) {
                AppLog.e(TAG, "[FILE-RECV] 所有图片解码方式均失败")
                return null
            }
            try {
                val displayName = "SyncClip_${System.currentTimeMillis()}.png"
                val values = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, displayName)
                    put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/SyncClipboard")
                        put(MediaStore.Images.Media.IS_PENDING, 1)
                    }
                }
                val uri = contentResolver.insert(
                    MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                if (uri == null) {
                    AppLog.e(TAG, "[FILE-RECV] MediaStore insert失败")
                    return null
                }
                contentResolver.openOutputStream(uri)?.use { os ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, os)
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    contentResolver.update(uri, ContentValues().apply {
                        put(MediaStore.Images.Media.IS_PENDING, 0)
                    }, null, null)
                }
                file.delete()
                return uri
            } finally {
                bitmap.recycle()
            }
        } catch (e: Exception) {
            AppLog.e(TAG, "[FILE-RECV] 保存到MediaStore异常", e)
            return null
        }
    }

    private fun decodeBmpFallback(file: java.io.File): Bitmap? {
        try {
            val data = file.readBytes()
            if (data.size < 54) return null
            if (data[0] != 'B'.code.toByte() || data[1] != 'M'.code.toByte()) return null

            val pixelDataOffset = readLE32(data, 10)
            val headerSize = readLE32(data, 14)
            val width = readLE32(data, 18)
            val rawHeight = readLE32(data, 22)
            val bitsPerPixel = readLE16(data, 28)
            val compression = readLE32(data, 30)

            val absHeight = kotlin.math.abs(rawHeight)
            val topDown = rawHeight < 0

            if (width <= 0 || absHeight <= 0) return null
            if (bitsPerPixel != 24 && bitsPerPixel != 32) return null

            val bytesPerPixel = bitsPerPixel / 8
            val rowStride = ((width * bitsPerPixel + 31) / 32) * 4

            val calcOffset = when {
                compression == 3 && headerSize == 40 -> 14 + 40 + 12
                else -> 14 + headerSize
            }
            val actualOffset = if (pixelDataOffset >= calcOffset) pixelDataOffset else calcOffset

            if (actualOffset + rowStride.toLong() * absHeight > data.size) return null

            val pixels = IntArray(width * absHeight)
            var hasNonZeroAlpha = false

            for (y in 0 until absHeight) {
                val srcRow = if (topDown) y else (absHeight - 1 - y)
                val rowStart = actualOffset + srcRow * rowStride
                for (x in 0 until width) {
                    val off = rowStart + x * bytesPerPixel
                    if (off + bytesPerPixel > data.size) break
                    val b = data[off].toInt() and 0xFF
                    val g = data[off + 1].toInt() and 0xFF
                    val r = data[off + 2].toInt() and 0xFF
                    val a = if (bytesPerPixel == 4) {
                        val raw = data[off + 3].toInt() and 0xFF
                        if (raw != 0) hasNonZeroAlpha = true
                        raw
                    } else 255
                    pixels[y * width + x] = (a shl 24) or (r shl 16) or (g shl 8) or b
                }
            }

            if (bytesPerPixel == 4 && !hasNonZeroAlpha) {
                for (i in pixels.indices) {
                    pixels[i] = pixels[i] or (0xFF shl 24)
                }
            }

            val bitmap = Bitmap.createBitmap(width, absHeight, Bitmap.Config.ARGB_8888)
            bitmap.setPixels(pixels, 0, width, 0, 0, width, absHeight)
            AppLog.i(TAG, "[BMP] 手动解码成功 ${width}x${absHeight} ${bitsPerPixel}bpp compression=$compression")
            return bitmap
        } catch (e: Exception) {
            AppLog.e(TAG, "[BMP] 手动解码失败", e)
            return null
        }
    }

    private fun readLE16(data: ByteArray, offset: Int): Int {
        return (data[offset].toInt() and 0xFF) or
                ((data[offset + 1].toInt() and 0xFF) shl 8)
    }

    private fun readLE32(data: ByteArray, offset: Int): Int {
        return (data[offset].toInt() and 0xFF) or
                ((data[offset + 1].toInt() and 0xFF) shl 8) or
                ((data[offset + 2].toInt() and 0xFF) shl 16) or
                ((data[offset + 3].toInt() and 0xFF) shl 24)
    }

    override fun onTransferFailed(fileId: String) {
        AppLog.w(TAG, "[FILE] 传输失败 fileId=$fileId")
    }

    override fun onNeedRelay(fileId: String) {
        val req = JsonObject().apply {
            addProperty("type", "file_relay")
            addProperty("fileId", fileId)
        }
        webSocket?.send(gson.toJson(req))
    }

    override fun onSendRelayData(fileId: String, data: ByteArray, fileSize: Long, targetDevice: String) {
        val b64 = ftManager.base64Encode(data)
        val msg = JsonObject().apply {
            addProperty("type", "file_relay")
            addProperty("fileId", fileId)
            addProperty("fileSize", fileSize)
            addProperty("targetDevice", targetDevice)
            addProperty("data", b64)
        }
        webSocket?.send(gson.toJson(msg))
    }

    /**
     * Called by ClipboardAccessibilityService to forward detected clipboard text.
     */
    fun sendClipboardFromExternal(text: String) {
        if (!isLoggedIn) return
        val hash = text.hashCode().toString()
        if (hash == lastClipHash) return
        lastClipHash = hash
        Log.i(TAG, "[EXTERNAL] 收到外部剪切板 (${text.length} chars), 加密并发送...")
        val encrypted = CryptoUtil.encrypt(text, config.aesKey) ?: return
        val msg = JsonObject().apply {
            addProperty("type", "clipboard")
            addProperty("content", encrypted)
        }
        webSocket?.send(gson.toJson(msg))
    }

    private fun onClipboardChanged() {
        if (!isLoggedIn) {
            Log.d(TAG, "[CLIPBOARD] 剪切板变化，但尚未登录，忽略")
            return
        }

        if (suppressNext) {
            suppressNext = false
            Log.d(TAG, "[CLIPBOARD] 抑制本次剪切板变化（来自远程同步）")
            return
        }

        val clip = try { clipboardManager.primaryClip } catch (_: Exception) { null }
        if (clip == null || clip.itemCount == 0) {
            if (!ClipboardAccessibilityService.isRunning) {
                Log.d(TAG, "[CLIPBOARD] 后台无法读取剪切板且无障碍服务未运行, 尝试启动Reader")
                launchClipboardReaderIfNeeded()
            }
            return
        }

        if (checkAndSendImageClipboard()) return

        val text = clip.getItemAt(0).text?.toString() ?: return
        if (text.isEmpty()) return

        val hash = text.hashCode().toString()
        if (hash == lastClipHash) return
        lastClipHash = hash

        Log.i(TAG, "[CLIPBOARD] 剪切板变化 (${text.length} chars), 加密并发送...")
        val encrypted = CryptoUtil.encrypt(text, config.aesKey)
        if (encrypted == null) {
            Log.e(TAG, "[CLIPBOARD] 加密失败")
            return
        }
        val msg = JsonObject().apply {
            addProperty("type", "clipboard")
            addProperty("content", encrypted)
        }
        val json = gson.toJson(msg)
        val sent = webSocket?.send(json)
        Log.i(TAG, "[CLIPBOARD] 发送结果: $sent, contentLen=${encrypted.length}")
    }

    private fun launchClipboardReaderIfNeeded() {
        val now = System.currentTimeMillis()
        if (now - lastReaderLaunchTime < 2000) return
        lastReaderLaunchTime = now
        try {
            val intent = Intent(this, ClipboardReaderActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
            }
            startActivity(intent)
        } catch (e: Exception) {
            AppLog.w(TAG, "[CLIPBOARD] 无法启动Reader: ${e.message}")
        }
    }

    fun sendImageFromExternal(data: ByteArray, mimeType: String) {
        if (!isLoggedIn) return
        val imgHash = ftManager.sha256(data)
        if (imgHash == lastImgHash) return
        lastImgHash = imgHash

        val ext = if (mimeType.contains("jpeg") || mimeType.contains("jpg")) ".jpg" else ".png"
        val fileName = "clipboard$ext"
        AppLog.i(TAG, "[IMAGE-EXT] 外部图片 $mimeType, ${data.size} bytes")
        val fileId = ftManager.generateFileId()
        transferStarted = false
        val info = FileTransferManager.FileInfo(fileId, fileName, mimeType, data.size.toLong(), imgHash, data)
        val state = ftManager.startSenderOffer(info)
        state.aesKey = config.aesKey
        val addrs = ftManager.getLocalAddresses(state.listenPort)
        val msg = JsonObject().apply {
            addProperty("type", "file_offer")
            addProperty("fileId", fileId)
            addProperty("fileName", fileName)
            addProperty("mimeType", mimeType)
            addProperty("fileSize", info.fileSize)
            addProperty("checksum", imgHash)
            add("localAddresses", gson.toJsonTree(addrs))
        }
        webSocket?.send(gson.toJson(msg))
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID, "剪切板同步服务",
                NotificationManager.IMPORTANCE_MIN
            ).apply {
                description = "剪切板同步服务运行状态"
                setShowBadge(false)
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(text: String): Notification {
        val intent = Intent(this, MainActivity::class.java)
        val pendingIntent = PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("SyncClipboard")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }

    private fun broadcastStatus(status: String) {
        sendBroadcast(Intent(ACTION_STATUS_CHANGED).setPackage(packageName).putExtra(EXTRA_STATUS, status))
    }
}
