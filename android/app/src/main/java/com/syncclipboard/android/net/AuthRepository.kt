package com.syncclipboard.android.net

import com.squareup.moshi.Moshi
import com.squareup.moshi.kotlin.reflect.KotlinJsonAdapterFactory
import com.syncclipboard.android.data.SecureConfigStore
import com.syncclipboard.android.util.AppLog
import okhttp3.OkHttpClient
import retrofit2.Retrofit
import retrofit2.converter.moshi.MoshiConverterFactory

/**
 * Handles HTTP login + token refresh against the v2 server. Tokens are
 * persisted in SecureConfigStore so they survive process restarts.
 */
class AuthRepository(
    private val store: SecureConfigStore,
    private val httpClient: OkHttpClient,
) {

    @Volatile
    private var cachedApi: ApiService? = null

    @Volatile
    private var cachedBaseUrl: String = ""

    private val moshi = Moshi.Builder().add(KotlinJsonAdapterFactory()).build()

    private fun api(): ApiService {
        val base = store.httpBaseUrl()
        val current = cachedApi
        if (current != null && cachedBaseUrl == base) return current
        val retrofit = Retrofit.Builder()
            .baseUrl(base.trimEnd('/') + "/")
            .client(httpClient)
            .addConverterFactory(MoshiConverterFactory.create(moshi))
            .build()
        val svc = retrofit.create(ApiService::class.java)
        cachedApi = svc
        cachedBaseUrl = base
        return svc
    }

    sealed interface Result {
        data class Ok(val accessToken: String) : Result
        data class Err(val code: String, val message: String) : Result
        data class Network(val cause: Throwable) : Result
    }

    suspend fun login(): Result {
        return try {
            val baseUrl = store.httpBaseUrl()
            AppLog.i(TAG, "login -> $baseUrl/api/v2/auth/login user=${store.username} device=${store.deviceId}")
            val resp = api().login(LoginRequest(store.username, store.password, store.deviceId))
            handleTokenResponse(resp)
        } catch (e: Exception) {
            AppLog.w(TAG, "login network error: ${e.javaClass.simpleName}: ${e.message}")
            Result.Network(e)
        }
    }

    suspend fun refresh(): Result {
        if (store.refreshToken.isBlank()) return login()
        return try {
            val resp = api().refresh(RefreshRequest(store.refreshToken, store.deviceId))
            if (resp.isSuccessful) {
                handleTokenResponse(resp)
            } else {
                // Refresh tokens expire — fall back to a fresh login.
                login()
            }
        } catch (e: Exception) {
            AppLog.w(TAG, "refresh network error: ${e.javaClass.simpleName}: ${e.message}")
            Result.Network(e)
        }
    }

    private fun handleTokenResponse(resp: retrofit2.Response<TokenPair>): Result {
        if (resp.isSuccessful) {
            val body = resp.body() ?: return Result.Err("MSG_INVALID", "Empty response")
            store.accessToken = body.accessToken
            store.refreshToken = body.refreshToken
            store.accessTokenExpEpoch = System.currentTimeMillis() / 1000 + body.expiresInSec
            AppLog.i(TAG, "login OK, expiresInSec=${body.expiresInSec}")
            return Result.Ok(body.accessToken)
        }
        val errBody = try {
            resp.errorBody()?.string()?.let { moshi.adapter(ErrorBody::class.java).fromJson(it) }
        } catch (_: Exception) { null }
        val code = errBody?.code ?: "AUTH_INVALID"
        val msg = errBody?.message ?: "HTTP ${resp.code()}"
        AppLog.w(TAG, "login failed code=$code, http=${resp.code()}")
        return Result.Err(code, msg)
    }

    fun haveValidToken(): Boolean {
        if (store.accessToken.isBlank()) return false
        val nowSec = System.currentTimeMillis() / 1000
        return store.accessTokenExpEpoch > nowSec + 30
    }

    suspend fun ensureValidToken(): String? {
        if (haveValidToken()) return store.accessToken
        return when (val r = if (store.refreshToken.isNotBlank()) refresh() else login()) {
            is Result.Ok -> r.accessToken
            else -> null
        }
    }

    companion object {
        private const val TAG = "AuthRepository"
    }
}
