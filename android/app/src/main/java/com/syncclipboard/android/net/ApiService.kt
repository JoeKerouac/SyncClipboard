package com.syncclipboard.android.net

import com.squareup.moshi.JsonClass
import retrofit2.Response
import retrofit2.http.Body
import retrofit2.http.POST

interface ApiService {

    @POST("/api/v2/auth/login")
    suspend fun login(@Body req: LoginRequest): Response<TokenPair>

    @POST("/api/v2/auth/refresh")
    suspend fun refresh(@Body req: RefreshRequest): Response<TokenPair>
}

@JsonClass(generateAdapter = true)
data class LoginRequest(val username: String, val password: String, val deviceId: String)

@JsonClass(generateAdapter = true)
data class RefreshRequest(val refreshToken: String, val deviceId: String)

@JsonClass(generateAdapter = true)
data class TokenPair(val accessToken: String, val refreshToken: String, val expiresInSec: Long)

@JsonClass(generateAdapter = true)
data class ErrorBody(val code: String?, val message: String?)
