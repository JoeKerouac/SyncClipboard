package com.syncclipboard.android.util

import java.io.*
import java.net.*
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread
import android.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

class FileTransferManager(val cacheDir: java.io.File? = null, var maxTransferBytes: Long = 500L * 1024 * 1024) {
    companion object {
        private const val TAG = "FileTransfer"
        private const val MAGIC = "SYNC"
        private const val UUID_LEN = 36
        private const val P2P_TIMEOUT_MS = 3000
        private const val TCP_BUF_SIZE = 512 * 1024
        private const val CHUNK_SIZE = 256 * 1024
        private const val SOCKET_TIMEOUT_MS = 30000
    }

    data class FileInfo(
        val fileId: String,
        val fileName: String,
        val mimeType: String,
        val fileSize: Long,
        val checksum: String,
        val data: ByteArray
    )

    data class TransferState(
        var fileId: String = "",
        var fileName: String = "",
        var mimeType: String = "",
        var fileSize: Long = 0,
        var checksum: String = "",
        var data: ByteArray? = null,
        var isSender: Boolean = false,
        var serverSocket: ServerSocket? = null,
        var listenPort: Int = 0,
        var peerAddresses: List<String> = emptyList(),
        var peerPublicAddress: String = "",
        var maxRelaySize: Long = 0,
        var udpPort: Int = 0,
        var fromDevice: String = "",
        var fileTransferLevel: Int = 3,
        var sameLan: Boolean = false,
        var aesKey: String = "",
        @Volatile var success: Boolean = false
    )

    interface Listener {
        fun onFileReceived(file: java.io.File, fileName: String, mimeType: String, method: String,
                           connectionMs: Long = 0, transferMs: Long = 0)
        fun onTransferFailed(fileId: String)
        fun onNeedRelay(fileId: String)
        fun onSendRelayData(fileId: String, data: ByteArray, fileSize: Long, targetDevice: String)
    }

    private val executor = Executors.newCachedThreadPool()
    var currentState: TransferState? = null
        private set

    fun getLocalAddresses(port: Int): List<String> {
        val addrs = mutableListOf<String>()
        try {
            for (iface in NetworkInterface.getNetworkInterfaces()) {
                if (iface.isLoopback || !iface.isUp) continue
                for (addr in iface.inetAddresses) {
                    if (addr is Inet4Address && !addr.isLoopbackAddress) {
                        addrs.add("${addr.hostAddress}:$port")
                    }
                }
            }
        } catch (e: Exception) {
            AppLog.e(TAG, "获取本地地址失败", e)
        }
        return addrs
    }

    fun generateFileId(): String = UUID.randomUUID().toString()

    fun sha256(data: ByteArray): String {
        val md = MessageDigest.getInstance("SHA-256")
        return md.digest(data).joinToString("") { "%02x".format(it) }
    }

    fun base64Encode(data: ByteArray): String =
        Base64.encodeToString(data, Base64.NO_WRAP)

    fun base64Decode(str: String): ByteArray =
        Base64.decode(str, Base64.NO_WRAP)

    fun startSenderOffer(info: FileInfo): TransferState {
        val ss = ServerSocket(0)
        val port = ss.localPort
        val state = TransferState(
            fileId = info.fileId,
            fileName = info.fileName,
            mimeType = info.mimeType,
            fileSize = info.fileSize,
            checksum = info.checksum,
            data = info.data,
            isSender = true,
            serverSocket = ss,
            listenPort = port
        )
        currentState = state
        AppLog.i(TAG, "[OFFER] 文件发布 fileId=${info.fileId}, port=$port, size=${info.fileSize}")
        return state
    }

    fun startReceiverRequest(
        fileId: String, fileName: String, mimeType: String,
        fileSize: Long, checksum: String, maxRelaySize: Long, udpPort: Int, from: String
    ): TransferState {
        val ss = ServerSocket(0)
        val port = ss.localPort
        val state = TransferState(
            fileId = fileId, fileName = fileName, mimeType = mimeType,
            fileSize = fileSize, checksum = checksum,
            isSender = false, serverSocket = ss, listenPort = port,
            maxRelaySize = maxRelaySize, udpPort = udpPort, fromDevice = from
        )
        currentState = state
        AppLog.i(TAG, "[REQUEST] 准备接收 fileId=$fileId, port=$port")
        return state
    }

    fun startTransfer(state: TransferState, serverHost: String, listener: Listener) {
        executor.submit {
            try {
                if (state.isSender)
                    runSender(state, serverHost, listener)
                else
                    runReceiver(state, serverHost, listener)
            } catch (e: Exception) {
                AppLog.e(TAG, "传输异常", e)
                listener.onTransferFailed(state.fileId)
            } finally {
                try { state.serverSocket?.close() } catch (_: Exception) {}
            }
        }
    }

    private fun runReceiver(state: TransferState, serverHost: String, listener: Listener) {
        AppLog.i(TAG, "[RECV] 开始接收 fileId=${state.fileId}, sameLan=${state.sameLan}")
        val t0 = System.currentTimeMillis()
        val deadline = t0 + P2P_TIMEOUT_MS + 2000

        val natAddr = if (!state.sameLan && state.fileTransferLevel >= 2 && state.peerPublicAddress.isNotEmpty())
            state.peerPublicAddress else null
        AppLog.i(TAG, "[RECV] LAN+NAT并行 (${state.peerAddresses.size}个地址, nat=${natAddr ?: "none"}, ${P2P_TIMEOUT_MS}ms)")

        var connectedSock: Socket? = null
        var connMethod = "failed"

        while (System.currentTimeMillis() < deadline) {
            val remaining = (deadline - System.currentTimeMillis()).toInt()
            if (remaining <= 0) break
            val timeoutMs = minOf(remaining, P2P_TIMEOUT_MS)

            val (sock, method) = tryParallelConnect(state.serverSocket, state.peerAddresses, natAddr, timeoutMs)
            if (sock == null) {
                AppLog.w(TAG, "[RECV] tryParallelConnect returned null, no connection")
                break
            }

            try {
                if (state.aesKey.isEmpty() || authRecv(sock, state.fileId, state.aesKey)) {
                    connectedSock = sock
                    connMethod = method
                    break
                }
                AppLog.w(TAG, "[RECV] P2P认证失败, 重试...")
                sock.close()
            } catch (e: Exception) {
                AppLog.w(TAG, "[RECV] Auth异常: ${e.message}")
                try { sock.close() } catch (_: Exception) {}
            }
        }

        val connMs = System.currentTimeMillis() - t0

        if (connectedSock != null) {
            try {
                AppLog.i(TAG, "[RECV] 已连接($connMethod, ${connMs}ms), 接收文件...")
                val tXfer = System.currentTimeMillis()
                val tmpFile = java.io.File(cacheDir ?: java.io.File(System.getProperty("java.io.tmpdir")),
                    "syncclip_recv_${state.fileId}")
                val fileSize = recvFileToPath(connectedSock, state.fileId, tmpFile)
                if (fileSize >= 0) {
                    val xferMs = System.currentTimeMillis() - tXfer
                    val speed = if (xferMs > 0) fileSize.toDouble() / 1024 / 1024 / (xferMs / 1000.0) else 0.0
                    AppLog.i(TAG, "[RECV] 接收完成 $fileSize bytes, 传输${xferMs}ms (%.1f MB/s)".format(speed))
                    state.success = true
                    listener.onFileReceived(tmpFile, state.fileName, state.mimeType, connMethod, connMs, xferMs)
                    return
                }
                connectedSock.close()
            } catch (e: Exception) {
                AppLog.e(TAG, "[RECV] 接收失败", e)
                try { connectedSock.close() } catch (_: Exception) {}
            }
        }

        if (!state.success && state.fileTransferLevel >= 3 && state.fileSize <= state.maxRelaySize) {
            AppLog.i(TAG, "[RECV] P2P失败, 请求服务器中转")
            listener.onNeedRelay(state.fileId)
        } else if (!state.success) {
            AppLog.w(TAG, "[RECV] 所有方式失败, 放弃")
            listener.onTransferFailed(state.fileId)
        }
    }

    private fun runSender(state: TransferState, serverHost: String, listener: Listener) {
        AppLog.i(TAG, "[SEND] 开始发送 fileId=${state.fileId}, sameLan=${state.sameLan}")
        val t0 = System.currentTimeMillis()

        val natAddr = if (!state.sameLan && state.fileTransferLevel >= 2 && state.peerPublicAddress.isNotEmpty())
            state.peerPublicAddress else null
        AppLog.i(TAG, "[SEND] LAN+NAT并行 (${state.peerAddresses.size}个地址, nat=${natAddr ?: "none"}, ${P2P_TIMEOUT_MS}ms)")
        val (sock, _) = tryParallelConnect(state.serverSocket, state.peerAddresses, natAddr, P2P_TIMEOUT_MS)

        val connMs = System.currentTimeMillis() - t0

        if (sock != null) {
            try {
                if (state.aesKey.isNotEmpty() && !authSend(sock, state.fileId, state.aesKey)) {
                    AppLog.w(TAG, "[SEND] P2P认证失败")
                } else {
                    AppLog.i(TAG, "[SEND] 已连接(${connMs}ms), 发送文件 ${state.data!!.size} bytes...")
                    val tXfer = System.currentTimeMillis()
                    sendFile(sock, state.fileId, state.data!!)
                    val xferMs = System.currentTimeMillis() - tXfer
                    val speed = if (xferMs > 0) state.data!!.size.toDouble() / 1024 / 1024 / (xferMs / 1000.0) else 0.0
                    state.success = true
                    AppLog.i(TAG, "[SEND] 发送完成 ${xferMs}ms (%.1f MB/s)".format(speed))
                }
            } catch (e: Exception) {
                AppLog.e(TAG, "[SEND] 发送失败", e)
            } finally {
                sock.close()
            }
        }
    }

    private fun optimizeSocket(s: Socket) {
        try {
            s.tcpNoDelay = true
            s.sendBufferSize = TCP_BUF_SIZE
            s.receiveBufferSize = TCP_BUF_SIZE
            s.soTimeout = SOCKET_TIMEOUT_MS
        } catch (_: Exception) {}
    }

    /**
     * LAN accept + connect + optional NAT punch, all in parallel.
     * Returns (socket, method) where method is "lan", "nat", or "failed".
     */
    private fun tryParallelConnect(
        serverSocket: ServerSocket?,
        addresses: List<String>,
        natAddr: String?,
        timeoutMs: Int
    ): Pair<Socket?, String> {
        val result = arrayOfNulls<Socket>(1)
        val resultMethod = arrayOf("failed")
        val done = AtomicBoolean(false)
        val threads = mutableListOf<Thread>()

        if (serverSocket != null) {
            threads.add(thread(start = true) {
                try {
                    serverSocket.soTimeout = timeoutMs
                    val s = serverSocket.accept()
                    if (done.compareAndSet(false, true)) {
                        optimizeSocket(s)
                        synchronized(result) { result[0] = s; resultMethod[0] = "lan" }
                        AppLog.i(TAG, "[LAN] Accept成功 from=${s.remoteSocketAddress}")
                    } else {
                        s.close()
                    }
                } catch (_: Exception) {}
            })
        }

        for (addr in addresses) {
            threads.add(thread(start = true) {
                try {
                    val parts = addr.split(":")
                    if (parts.size != 2) return@thread
                    val s = Socket()
                    s.connect(InetSocketAddress(parts[0], parts[1].toInt()), timeoutMs)
                    if (done.compareAndSet(false, true)) {
                        optimizeSocket(s)
                        synchronized(result) { result[0] = s; resultMethod[0] = "lan" }
                        AppLog.i(TAG, "[LAN] Connect成功 to=$addr")
                    } else {
                        s.close()
                    }
                } catch (_: Exception) {}
            })
        }

        if (natAddr != null) {
            threads.add(thread(start = true) {
                try {
                    val parts = natAddr.split(":")
                    if (parts.size != 2) return@thread
                    val peerIp = parts[0]
                    val peerPort = parts[1].toInt()

                    val udpSock = DatagramSocket()
                    val localPort = udpSock.localPort
                    val peerSa = InetSocketAddress(peerIp, peerPort)
                    repeat(3) {
                        val data = "PUNCH".toByteArray()
                        udpSock.send(DatagramPacket(data, data.size, peerSa))
                        Thread.sleep(20)
                    }
                    udpSock.close()

                    if (done.get()) return@thread
                    val tcpSock = Socket()
                    tcpSock.reuseAddress = true
                    tcpSock.bind(InetSocketAddress(localPort))
                    val remaining = timeoutMs - 60
                    if (remaining <= 0) return@thread
                    tcpSock.connect(InetSocketAddress(peerIp, peerPort), remaining)
                    if (done.compareAndSet(false, true)) {
                        optimizeSocket(tcpSock)
                        synchronized(result) { result[0] = tcpSock; resultMethod[0] = "nat" }
                        AppLog.i(TAG, "[NAT] TCP连接成功 -> $natAddr")
                    } else {
                        tcpSock.close()
                    }
                } catch (e: Exception) {
                    AppLog.d(TAG, "[NAT] 打洞失败: ${e.message}")
                }
            })
        }

        val deadline = System.currentTimeMillis() + timeoutMs + 500
        while (!done.get() && System.currentTimeMillis() < deadline && threads.any { it.isAlive }) {
            Thread.sleep(10)
        }

        if (done.get()) {
        }

        return Pair(result[0], resultMethod[0])
    }

    private fun computeHmac(fileId: String, keyHex: String): ByteArray {
        val klen = minOf(keyHex.length / 2, 32)
        val keyBytes = ByteArray(klen) { i ->
            keyHex.substring(i * 2, i * 2 + 2).toInt(16).toByte()
        }
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(keyBytes, "HmacSHA256"))
        return mac.doFinal(fileId.toByteArray(Charsets.UTF_8))
    }

    private fun authSend(sock: Socket, fileId: String, keyHex: String): Boolean {
        val hmac = computeHmac(fileId, keyHex)
        val out = sock.getOutputStream()
        out.write("AUTH".toByteArray())
        out.write(hmac)
        out.flush()
        val resp = ByteArray(4)
        val inp = sock.getInputStream()
        var read = 0
        while (read < 4) {
            val n = inp.read(resp, read, 4 - read)
            if (n <= 0) return false
            read += n
        }
        return String(resp) == "AKOK"
    }

    private fun authRecv(sock: Socket, fileId: String, keyHex: String): Boolean {
        val buf = ByteArray(36)
        val inp = sock.getInputStream()
        var read = 0
        while (read < 36) {
            val n = inp.read(buf, read, 36 - read)
            if (n <= 0) return false
            read += n
        }
        if (String(buf, 0, 4) != "AUTH") {
            sock.getOutputStream().write("FAIL".toByteArray())
            return false
        }
        val expected = computeHmac(fileId, keyHex)
        if (!buf.sliceArray(4 until 36).contentEquals(expected)) {
            sock.getOutputStream().write("FAIL".toByteArray())
            return false
        }
        val out = sock.getOutputStream()
        out.write("AKOK".toByteArray())
        out.flush()
        return true
    }

    private fun sendFile(sock: Socket, fileId: String, data: ByteArray) {
        val out = DataOutputStream(BufferedOutputStream(sock.getOutputStream(), TCP_BUF_SIZE))
        out.write(MAGIC.toByteArray())
        out.write(fileId.padEnd(UUID_LEN).substring(0, UUID_LEN).toByteArray())
        out.writeLong(data.size.toLong())
        out.write(data)
        out.flush()
    }

    private fun recvFileToPath(sock: Socket, expectedFileId: String, outputFile: java.io.File): Long {
        val inp = DataInputStream(BufferedInputStream(sock.getInputStream(), TCP_BUF_SIZE))
        val magic = ByteArray(4)
        inp.readFully(magic)
        if (String(magic) != MAGIC) return -1

        val fidBytes = ByteArray(UUID_LEN)
        inp.readFully(fidBytes)

        val fileSize = inp.readLong()
        if (fileSize > maxTransferBytes || fileSize <= 0) return -1

        val out = BufferedOutputStream(java.io.FileOutputStream(outputFile), TCP_BUF_SIZE)
        val chunk = ByteArray(CHUNK_SIZE)
        var remaining = fileSize
        try {
            while (remaining > 0) {
                val toRead = minOf(remaining, CHUNK_SIZE.toLong()).toInt()
                inp.readFully(chunk, 0, toRead)
                out.write(chunk, 0, toRead)
                remaining -= toRead
            }
            out.flush()
        } catch (e: Exception) {
            out.close()
            outputFile.delete()
            throw e
        }
        out.close()
        return fileSize
    }

    fun handleRelayData(b64Data: String, fileName: String, mimeType: String, listener: Listener) {
        executor.submit {
            try {
                val data = base64Decode(b64Data)
                AppLog.i(TAG, "[RELAY] 中转数据 ${data.size} bytes")
                val tmpFile = java.io.File(cacheDir ?: java.io.File(System.getProperty("java.io.tmpdir")),
                    "syncclip_relay_$fileName")
                tmpFile.writeBytes(data)
                listener.onFileReceived(tmpFile, fileName, mimeType, "relay")
            } catch (e: Exception) {
                AppLog.e(TAG, "[RELAY] 解码失败", e)
            }
        }
    }

    fun cleanup() {
        try { currentState?.serverSocket?.close() } catch (_: Exception) {}
        currentState = null
    }
}
