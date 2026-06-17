package com.syncclipboard.android.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
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
import com.google.gson.JsonObject
import com.syncclipboard.android.App
import com.syncclipboard.android.crypto.AesGcmCipher
import com.syncclipboard.android.data.SecureConfigStore
import com.syncclipboard.android.net.ConnectionManager
import com.syncclipboard.android.net.MessageCodec
import com.syncclipboard.android.ui.MainActivity
import com.syncclipboard.android.util.AppLog
import com.syncclipboard.android.util.FileTransferManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicReference

/**
 * Foreground service that bridges the Android system clipboard and the v2
 * WebSocket connection. Authentication, reconnection and the OkHttpClient
 * lifecycle are owned by [ConnectionManager]; this class only forwards
 * clipboard events and handles inbound messages.
 */
class ClipboardService : Service(), FileTransferManager.Listener {

    companion object {
        private const val TAG = "ClipboardService"
        private const val CHANNEL_ID = "sync_clipboard_channel"
        private const val NOTIFICATION_ID = 1
        const val ACTION_STATUS_CHANGED = "com.syncclipboard.STATUS_CHANGED"
        const val EXTRA_STATUS = "status"
        const val STATUS_CONNECTING = "connecting"
        const val STATUS_CONNECTED = "connected"
        const val STATUS_LOGGED_IN = "logged_in"
        const val STATUS_DISCONNECTED = "disconnected"
        const val STATUS_AUTH_FAIL = "auth_fail"

        @Volatile var isRunning = false; private set
        @Volatile var instance: ClipboardService? = null; private set
    }

    private lateinit var store: SecureConfigStore
    private lateinit var connection: ConnectionManager
    private lateinit var clipboardManager: ClipboardManager
    private lateinit var ftManager: FileTransferManager
    private val handler = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var jobs: Job? = null

    @Volatile private var loggedIn = false
    @Volatile private var serverFileLevel = 3
    @Volatile private var suppressNext = false
    private val lastClipHash = AtomicReference("")
    private val lastImgHash = AtomicReference("")
    @Volatile private var transferStarted = false
    @Volatile private var lastReaderLaunchTime = 0L

    private val clipboardListener = ClipboardManager.OnPrimaryClipChangedListener { onClipboardChanged() }

    override fun onCreate() {
        super.onCreate()
        store = App.secureConfig
        connection = App.connectionManager
        ftManager = FileTransferManager(cacheDir, store.maxTransferSizeMb.toLong() * 1024 * 1024)
        clipboardManager = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        createNotificationChannel()
        cleanupStaleCacheFiles()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        instance = this
        isRunning = true
        startForeground(NOTIFICATION_ID, buildNotification("正在连接..."))
        clipboardManager.addPrimaryClipChangedListener(clipboardListener)
        connection.start()
        jobs = scope.launch {
            launch { observeState() }
            launch { observeIncoming() }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        instance = null
        isRunning = false
        clipboardManager.removePrimaryClipChangedListener(clipboardListener)
        jobs?.cancel()
        scope.cancel()
        connection.stop()
        ftManager.cleanup()
        super.onDestroy()
    }

    private suspend fun observeState() {
        connection.state.collectLatest { state ->
            when (state) {
                ConnectionManager.State.CONNECTING -> {
                    updateNotification("正在连接...")
                    broadcastStatus(STATUS_CONNECTING)
                }
                ConnectionManager.State.CONNECTED -> {
                    loggedIn = true
                    updateNotification("已连接")
                    broadcastStatus(STATUS_LOGGED_IN)
                }
                ConnectionManager.State.DISCONNECTED -> {
                    loggedIn = false
                    updateNotification("已断开，正在重连...")
                    broadcastStatus(STATUS_DISCONNECTED)
                }
                ConnectionManager.State.AUTH_FAILED -> {
                    loggedIn = false
                    updateNotification("认证失败，请检查账号密码")
                    broadcastStatus(STATUS_AUTH_FAIL)
                }
                ConnectionManager.State.IDLE -> {
                    updateNotification("未启动")
                }
            }
        }
    }

    private fun updateNotification(text: String) {
        try {
            val mgr = getSystemService(NotificationManager::class.java)
            mgr.notify(NOTIFICATION_ID, buildNotification(text))
        } catch (_: Exception) { /* best-effort */ }
    }

    private suspend fun observeIncoming() {
        connection.incoming.collectLatest { text ->
            handleMessage(text)
        }
    }

    private fun handleMessage(text: String) {
        Log.d(TAG, "[RECV] len=${text.length}")
        val json = MessageCodec.parse(text) ?: return
        val type = json.get("type")?.asString ?: return
        when (type) {
            "hello_ack" -> {
                serverFileLevel = json.get("serverFileLevel")?.asInt ?: 3
                AppLog.i(TAG, "hello_ack serverFileLevel=$serverFileLevel")
            }
            "clipboard" -> handleClipboardMsg(json)
            "file_notify" -> handleFileNotify(json)
            "file_peer_info" -> handleFilePeerInfo(json)
            "file_relay_request" -> handleFileRelayRequest(json)
            "file_relay_data" -> handleFileRelayData(json)
            "error" -> AppLog.w(TAG, "server error code=${json.get("code")?.asString}: ${json.get("message")?.asString}")
        }
    }

    private fun handleClipboardMsg(json: JsonObject) {
        val content = json.get("content")?.asString ?: return
        val from = json.get("from")?.asString ?: "unknown"
        AppLog.i(TAG, "[CLIP-IN] from=$from len=${content.length}")
        when (val r = AesGcmCipher.decrypt(content, store.aesKey)) {
            is AesGcmCipher.DecryptResult.Success -> {
                suppressNext = true
                clipboardManager.setPrimaryClip(ClipData.newPlainText("SyncClipboard", r.plaintext))
                lastClipHash.set(r.plaintext.hashCode().toString())
            }
            AesGcmCipher.DecryptResult.AuthFailed ->
                AppLog.w(TAG, "[CLIP-IN] auth tag invalid; payload tampered or wrong key")
            AesGcmCipher.DecryptResult.InvalidVersion ->
                AppLog.w(TAG, "[CLIP-IN] unsupported cipher version (peer running v1?)")
            AesGcmCipher.DecryptResult.InvalidFormat,
            AesGcmCipher.DecryptResult.KeyError ->
                AppLog.w(TAG, "[CLIP-IN] cannot decrypt: ${r.javaClass.simpleName}")
        }
    }

    // ---- file transfer (logic preserved from v1, only network side updated) ----

    private fun handleFileNotify(json: JsonObject) {
        if (store.fileTransferLevel <= 0) return
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
        state.aesKey = store.aesKey
        val addrs = ftManager.getLocalAddresses(state.listenPort)
        connection.send(MessageCodec.message("file_request") {
            addProperty("fileId", fileId)
            add("localAddresses", com.google.gson.Gson().toJsonTree(addrs))
        })
    }

    private fun handleFilePeerInfo(json: JsonObject) {
        val state = ftManager.currentState ?: return
        if (transferStarted) {
            AppLog.i(TAG, "[PEER-INFO] dup ignored")
            return
        }
        state.peerAddresses = json.getAsJsonArray("peerLocalAddresses")?.map { it.asString } ?: emptyList()
        state.peerPublicAddress = json.get("peerPublicAddress")?.asString ?: ""
        val peerFileLevel = json.get("fileTransferLevel")?.asInt ?: 3
        state.fileTransferLevel = minOf(store.fileTransferLevel, serverFileLevel, peerFileLevel)
        state.sameLan = json.get("sameLan")?.asBoolean ?: false
        AppLog.i(TAG, "[PEER-INFO] addrs=${state.peerAddresses.size}, level=${state.fileTransferLevel}, sameLan=${state.sameLan}")
        transferStarted = true
        ftManager.startTransfer(state, store.serverHost, this)
    }

    private fun handleFileRelayRequest(json: JsonObject) {
        val requesterId = json.get("requesterId")?.asString ?: return
        val state = ftManager.currentState ?: return
        val data = state.data ?: return
        AppLog.i(TAG, "[RELAY-REQ] -> $requesterId")
        onSendRelayData(state.fileId, data, state.fileSize, requesterId)
    }

    private fun handleFileRelayData(json: JsonObject) {
        val fileName = json.get("fileName")?.asString ?: "file"
        val mimeType = json.get("mimeType")?.asString ?: "application/octet-stream"
        val b64 = json.get("data")?.asString ?: return
        AppLog.i(TAG, "[RELAY-DATA] $fileName")
        ftManager.handleRelayData(b64, fileName, mimeType, this)
    }

    private fun checkAndSendImageClipboard(): Boolean {
        if (store.fileTransferLevel <= 0 || serverFileLevel <= 0) return false
        return try {
            val clip = clipboardManager.primaryClip ?: return false
            val item = clip.getItemAt(0) ?: return false
            val uri = item.uri
            val mime = pickImageMime(clip, uri) ?: return false
            val data = uri?.let {
                contentResolver.openInputStream(it)?.use { stream -> stream.readBytes() }
            } ?: return false
            if (data.isEmpty()) return false
            val imgHash = ftManager.sha256(data)
            if (imgHash == lastImgHash.get()) return false
            lastImgHash.set(imgHash)
            sendImageOffer(imgHash, mime, data, fromExternal = false)
            true
        } catch (e: Exception) {
            Log.d(TAG, "[IMAGE] check failed: ${e.message}")
            false
        }
    }

    private fun pickImageMime(clip: ClipData, uri: Uri?): String? {
        if (clip.description.hasMimeType("image/*")) {
            return uri?.let { contentResolver.getType(it) ?: "image/png" } ?: "image/png"
        }
        if (uri != null) {
            val uriMime = contentResolver.getType(uri)
            if (uriMime?.startsWith("image/") == true) return uriMime
        }
        return null
    }

    private fun sendImageOffer(imgHash: String, mime: String, data: ByteArray, fromExternal: Boolean) {
        val ext = if (mime.contains("jpeg") || mime.contains("jpg")) ".jpg" else ".png"
        val fileName = "clipboard$ext"
        val fileId = ftManager.generateFileId()
        transferStarted = false
        val info = FileTransferManager.FileInfo(fileId, fileName, mime, data.size.toLong(), imgHash, data)
        val state = ftManager.startSenderOffer(info)
        state.aesKey = store.aesKey
        val addrs = ftManager.getLocalAddresses(state.listenPort)
        AppLog.i(TAG, "[IMAGE-OFFER] external=$fromExternal mime=$mime size=${data.size}")
        connection.send(MessageCodec.message("file_offer") {
            addProperty("fileId", fileId)
            addProperty("fileName", fileName)
            addProperty("mimeType", mime)
            addProperty("fileSize", info.fileSize)
            addProperty("checksum", imgHash)
            add("localAddresses", com.google.gson.Gson().toJsonTree(addrs))
        })
    }

    // ---- FileTransferManager.Listener ----

    override fun onFileReceived(file: java.io.File, fileName: String, mimeType: String, method: String,
                                connectionMs: Long, transferMs: Long) {
        AppLog.i(TAG, "[FILE-RECV] $fileName ($mimeType, ${file.length()}B) via $method")
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
                    clipboardManager.setPrimaryClip(ClipData.newUri(contentResolver, "SyncClipboard", clipUri))
                } else {
                    val ext = mimeToExtension(mimeType)
                    val safeName = if (ext.isNotEmpty() && !fileName.endsWith(ext))
                        fileName.substringBeforeLast(".") + ext else fileName
                    val tmpFile = java.io.File(cacheDir, "syncclip_$safeName")
                    if (file.absolutePath != tmpFile.absolutePath) file.renameTo(tmpFile)
                    val uri = androidx.core.content.FileProvider.getUriForFile(
                        this, "$packageName.fileprovider", tmpFile)
                    clipboardManager.setPrimaryClip(ClipData("SyncClipboard", arrayOf(clipMime), ClipData.Item(uri)))
                }
                connection.send(MessageCodec.message("file_transfer_result") {
                    addProperty("fileId", ftManager.currentState?.fileId ?: "")
                    addProperty("method", method)
                    addProperty("success", true)
                    addProperty("connectionMs", connectionMs)
                    addProperty("transferMs", transferMs)
                })
            } catch (e: Exception) {
                AppLog.e(TAG, "[FILE-RECV] write failed", e)
            }
        }
    }

    override fun onTransferFailed(fileId: String) {
        AppLog.w(TAG, "[FILE] failed fileId=$fileId")
    }

    override fun onNeedRelay(fileId: String) {
        connection.send(MessageCodec.message("file_relay") { addProperty("fileId", fileId) })
    }

    override fun onSendRelayData(fileId: String, data: ByteArray, fileSize: Long, targetDevice: String) {
        val b64 = ftManager.base64Encode(data)
        connection.send(MessageCodec.message("file_relay") {
            addProperty("fileId", fileId)
            addProperty("fileSize", fileSize)
            addProperty("targetDevice", targetDevice)
            addProperty("data", b64)
        })
    }

    fun sendClipboardFromExternal(text: String) {
        if (!loggedIn) return
        val hash = text.hashCode().toString()
        if (hash == lastClipHash.get()) return
        lastClipHash.set(hash)
        val encrypted = AesGcmCipher.encrypt(text, store.aesKey) ?: return
        connection.send(MessageCodec.message("clipboard") { addProperty("content", encrypted) })
    }

    fun sendImageFromExternal(data: ByteArray, mimeType: String) {
        if (!loggedIn) return
        val imgHash = ftManager.sha256(data)
        if (imgHash == lastImgHash.get()) return
        lastImgHash.set(imgHash)
        sendImageOffer(imgHash, mimeType, data, fromExternal = true)
    }

    private fun onClipboardChanged() {
        if (!loggedIn) return
        if (suppressNext) {
            suppressNext = false
            return
        }
        val clip = try { clipboardManager.primaryClip } catch (_: SecurityException) { null } catch (_: Exception) { null }
        if (clip == null || clip.itemCount == 0) {
            if (!ClipboardAccessibilityService.isRunning) launchClipboardReaderIfNeeded()
            return
        }
        if (checkAndSendImageClipboard()) return
        val text = clip.getItemAt(0).text?.toString() ?: return
        if (text.isEmpty()) return
        val hash = text.hashCode().toString()
        if (hash == lastClipHash.get()) return
        lastClipHash.set(hash)
        val encrypted = AesGcmCipher.encrypt(text, store.aesKey) ?: run {
            AppLog.w(TAG, "[CLIP-OUT] encrypt failed (key invalid?)")
            return
        }
        connection.send(MessageCodec.message("clipboard") { addProperty("content", encrypted) })
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
            AppLog.w(TAG, "reader launch failed: ${e.message}")
        }
    }

    private fun saveImageToMediaStore(file: java.io.File): Uri? {
        return try {
            var bitmap = BitmapFactory.decodeFile(file.absolutePath)
            if (bitmap == null) bitmap = decodeBmpFallback(file)
            if (bitmap == null) return null
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
                val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: return null
                contentResolver.openOutputStream(uri)?.use { os -> bitmap.compress(Bitmap.CompressFormat.PNG, 100, os) }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    contentResolver.update(uri, ContentValues().apply {
                        put(MediaStore.Images.Media.IS_PENDING, 0)
                    }, null, null)
                }
                file.delete()
                uri
            } finally {
                bitmap.recycle()
            }
        } catch (e: Exception) {
            AppLog.e(TAG, "[FILE-RECV] mediastore save failed", e)
            null
        }
    }

    private fun decodeBmpFallback(file: java.io.File): Bitmap? {
        return try {
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
            val calcOffset = if (compression == 3 && headerSize == 40) 14 + 40 + 12 else 14 + headerSize
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
                for (i in pixels.indices) pixels[i] = pixels[i] or (0xFF shl 24)
            }
            Bitmap.createBitmap(width, absHeight, Bitmap.Config.ARGB_8888).apply {
                setPixels(pixels, 0, width, 0, 0, width, absHeight)
            }
        } catch (e: Exception) {
            AppLog.e(TAG, "[BMP] decode failed", e)
            null
        }
    }

    private fun readLE16(data: ByteArray, offset: Int): Int =
        (data[offset].toInt() and 0xFF) or ((data[offset + 1].toInt() and 0xFF) shl 8)

    private fun readLE32(data: ByteArray, offset: Int): Int =
        (data[offset].toInt() and 0xFF) or
                ((data[offset + 1].toInt() and 0xFF) shl 8) or
                ((data[offset + 2].toInt() and 0xFF) shl 16) or
                ((data[offset + 3].toInt() and 0xFF) shl 24)

    private fun mimeToExtension(mime: String): String = when {
        mime.contains("png") -> ".png"
        mime.contains("jpeg") || mime.contains("jpg") -> ".jpg"
        mime.contains("gif") -> ".gif"
        mime.contains("webp") -> ".webp"
        mime.contains("pdf") -> ".pdf"
        else -> ""
    }

    private fun cleanupStaleCacheFiles() {
        try {
            val cutoff = System.currentTimeMillis() - 60L * 60L * 1000L
            cacheDir?.listFiles()
                ?.filter { it.name.startsWith("syncclip_") && it.lastModified() < cutoff }
                ?.forEach { runCatching { it.delete() } }
        } catch (_: Exception) { /* best-effort */ }
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
            getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
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
