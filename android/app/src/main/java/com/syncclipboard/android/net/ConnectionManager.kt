package com.syncclipboard.android.net

import com.syncclipboard.android.data.SecureConfigStore
import com.syncclipboard.android.util.AppLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * Process-wide WebSocket lifecycle. Owns the single OkHttpClient instance,
 * exposes connection state via StateFlow and inbound messages via SharedFlow,
 * and handles reconnect + token refresh in one place.
 */
class ConnectionManager(private val store: SecureConfigStore) {

    enum class State {
        IDLE, CONNECTING, CONNECTED, DISCONNECTED, AUTH_FAILED
    }

    private val httpClient: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .pingInterval(30, TimeUnit.SECONDS)
        .build()

    private val authRepo = AuthRepository(store, httpClient)

    private val _state = MutableStateFlow(State.IDLE)
    val state: StateFlow<State> = _state.asStateFlow()

    private val _incoming = MutableSharedFlow<String>(extraBufferCapacity = 64)
    val incoming: SharedFlow<String> = _incoming.asSharedFlow()

    private val socketRef = AtomicReference<WebSocket?>(null)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var connectJob: Job? = null

    @Volatile private var shouldReconnect = false

    fun start() {
        if (shouldReconnect) return
        shouldReconnect = true
        connectJob?.cancel()
        connectJob = scope.launch { connectLoop() }
    }

    fun stop() {
        shouldReconnect = false
        connectJob?.cancel()
        socketRef.getAndSet(null)?.cancel()
        _state.value = State.IDLE
    }

    fun shutdown() {
        stop()
        scope.cancel()
        httpClient.dispatcher.executorService.shutdown()
        httpClient.connectionPool.evictAll()
    }

    fun send(text: String): Boolean = socketRef.get()?.send(text) == true

    fun authRepository(): AuthRepository = authRepo

    private suspend fun connectLoop() {
        var attempt = 0
        while (shouldReconnect) {
            if (!store.isConfigured()) {
                AppLog.w(TAG, "not configured, idle")
                _state.value = State.IDLE
                delay(5000)
                continue
            }
            _state.value = State.CONNECTING
            val token = authRepo.ensureValidToken()
            if (token == null) {
                _state.value = State.AUTH_FAILED
                AppLog.w(TAG, "could not obtain access token; backing off")
                delay(backoffMs(attempt++))
                continue
            }
            val ok = openSocket(token)
            if (ok) {
                attempt = 0
                var tick = 0
                while (shouldReconnect && socketRef.get() != null) {
                    delay(1000)
                    tick++
                    if (tick % 15 == 0) {
                        socketRef.get()?.send("{\"type\":\"ping\",\"ts\":${System.currentTimeMillis()}}")
                    }
                }
            } else {
                _state.value = State.DISCONNECTED
                delay(backoffMs(attempt++))
            }
        }
    }

    private suspend fun openSocket(token: String): Boolean {
        val url = store.webSocketUrl()
        val request = Request.Builder()
            .url(url)
            .addHeader("Sec-WebSocket-Protocol", "bearer.$token")
            .build()
        val listener = object : WebSocketListener() {
            override fun onOpen(ws: WebSocket, response: Response) {
                AppLog.i(TAG, "socket open code=${response.code}")
                _state.value = State.CONNECTED
                // Send hello immediately to keep connection active and announce client.
                val hello = "{\"type\":\"hello\",\"v\":2,\"clientType\":\"android\",\"deviceId\":\"${store.deviceId}\",\"appVersion\":\"2.0.0\",\"capabilities\":[\"clipboard\",\"file_lan\",\"file_nat\",\"file_relay\"]}"
                ws.send(hello)
            }

            override fun onMessage(ws: WebSocket, text: String) {
                _incoming.tryEmit(text)
            }

            override fun onFailure(ws: WebSocket, t: Throwable, response: Response?) {
                AppLog.w(TAG, "socket failure ${t.javaClass.simpleName}: ${t.message}; http=${response?.code}")
                socketRef.compareAndSet(ws, null)
                _state.value = if (response?.code == 401) State.AUTH_FAILED else State.DISCONNECTED
                if (response?.code == 401) {
                    // Force token refresh on next attempt.
                    store.accessToken = ""
                    store.accessTokenExpEpoch = 0L
                }
            }

            override fun onClosing(ws: WebSocket, code: Int, reason: String) {
                AppLog.i(TAG, "socket closing $code/$reason")
            }

            override fun onClosed(ws: WebSocket, code: Int, reason: String) {
                AppLog.i(TAG, "socket closed $code/$reason")
                socketRef.compareAndSet(ws, null)
                _state.value = State.DISCONNECTED
            }
        }
        socketRef.getAndSet(null)?.cancel()
        val ws = httpClient.newWebSocket(request, listener)
        socketRef.set(ws)
        return true
    }

    private fun backoffMs(attempt: Int): Long {
        val capped = attempt.coerceAtMost(6)
        return (1000L shl capped).coerceAtMost(60_000L)
    }

    companion object {
        private const val TAG = "ConnectionManager"
    }
}
