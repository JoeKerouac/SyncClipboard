package com.syncclipboard.android.crypto

import android.util.Base64
import android.util.Log
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * AES-256-GCM authenticated encryption for clipboard payloads.
 *
 * Frame layout: 0x02 || nonce(12) || ciphertext || tag(16) — encoded as
 * Base64URL without padding so it travels safely in JSON.
 *
 * The leading 0x02 byte identifies the v2 cipher; receivers reject any other
 * leading byte instead of silently falling back to legacy CBC.
 */
object AesGcmCipher {

    private const val TAG = "AesGcmCipher"
    private const val ALG = "AES/GCM/NoPadding"
    private const val KEY_LEN = 32
    private const val NONCE_LEN = 12
    private const val TAG_LEN_BITS = 128
    private const val V2_PREFIX: Byte = 0x02

    private val rng = SecureRandom()

    sealed interface DecryptResult {
        data class Success(val plaintext: String) : DecryptResult
        data object InvalidFormat : DecryptResult
        data object InvalidVersion : DecryptResult
        data object AuthFailed : DecryptResult
        data object KeyError : DecryptResult
    }

    fun encrypt(plaintext: String, keyHex: String): String? {
        val key = parseKey(keyHex) ?: return null
        return try {
            val nonce = ByteArray(NONCE_LEN).also { rng.nextBytes(it) }
            val cipher = Cipher.getInstance(ALG)
            cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG_LEN_BITS, nonce))
            val ct = cipher.doFinal(plaintext.toByteArray(Charsets.UTF_8))
            val frame = ByteArray(1 + NONCE_LEN + ct.size)
            frame[0] = V2_PREFIX
            System.arraycopy(nonce, 0, frame, 1, NONCE_LEN)
            System.arraycopy(ct, 0, frame, 1 + NONCE_LEN, ct.size)
            Base64.encodeToString(frame, Base64.NO_WRAP)
        } catch (e: Exception) {
            Log.w(TAG, "encrypt failed: ${e.javaClass.simpleName}")
            null
        }
    }

    fun decrypt(b64: String, keyHex: String): DecryptResult {
        val key = parseKey(keyHex) ?: return DecryptResult.KeyError
        val frame = try {
            Base64.decode(b64, Base64.NO_WRAP)
        } catch (_: IllegalArgumentException) {
            return DecryptResult.InvalidFormat
        }
        if (frame.size < 1 + NONCE_LEN + (TAG_LEN_BITS / 8)) return DecryptResult.InvalidFormat
        if (frame[0] != V2_PREFIX) return DecryptResult.InvalidVersion
        return try {
            val nonce = frame.copyOfRange(1, 1 + NONCE_LEN)
            val ct = frame.copyOfRange(1 + NONCE_LEN, frame.size)
            val cipher = Cipher.getInstance(ALG)
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG_LEN_BITS, nonce))
            DecryptResult.Success(String(cipher.doFinal(ct), Charsets.UTF_8))
        } catch (e: javax.crypto.AEADBadTagException) {
            DecryptResult.AuthFailed
        } catch (e: Exception) {
            Log.w(TAG, "decrypt failed: ${e.javaClass.simpleName}")
            DecryptResult.InvalidFormat
        }
    }

    private fun parseKey(hex: String): ByteArray? {
        if (hex.length != KEY_LEN * 2) return null
        return try {
            ByteArray(KEY_LEN) { i ->
                hex.substring(i * 2, i * 2 + 2).toInt(16).toByte()
            }
        } catch (_: NumberFormatException) {
            null
        }
    }
}
