package com.syncclipboard.android.util

import android.util.Base64
import android.util.Log
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

object CryptoUtil {

    private const val TAG = "CryptoUtil"
    private const val AES_ALGORITHM = "AES/CBC/PKCS5Padding"
    private const val IV_LENGTH = 16

    private fun hexToBytes(hex: String): ByteArray {
        val len = hex.length / 2
        val result = ByteArray(len)
        for (i in 0 until len) {
            result[i] = hex.substring(2 * i, 2 * i + 2).toInt(16).toByte()
        }
        return result
    }

    fun encrypt(plaintext: String, keyHex: String): String {
        val keyBytes = hexToBytes(keyHex)
        val key = SecretKeySpec(keyBytes, "AES")

        val iv = ByteArray(IV_LENGTH)
        SecureRandom().nextBytes(iv)
        val ivSpec = IvParameterSpec(iv)

        val cipher = Cipher.getInstance(AES_ALGORITHM)
        cipher.init(Cipher.ENCRYPT_MODE, key, ivSpec)
        val encrypted = cipher.doFinal(plaintext.toByteArray(Charsets.UTF_8))

        val combined = iv + encrypted
        return Base64.encodeToString(combined, Base64.NO_WRAP)
    }

    fun decrypt(b64Input: String, keyHex: String): String? {
        return try {
            val combined = Base64.decode(b64Input, Base64.NO_WRAP)
            if (combined.size < IV_LENGTH + 1) return null

            val keyBytes = hexToBytes(keyHex)
            val key = SecretKeySpec(keyBytes, "AES")

            val iv = combined.copyOfRange(0, IV_LENGTH)
            val ciphertext = combined.copyOfRange(IV_LENGTH, combined.size)

            val cipher = Cipher.getInstance(AES_ALGORITHM)
            cipher.init(Cipher.DECRYPT_MODE, key, IvParameterSpec(iv))
            String(cipher.doFinal(ciphertext), Charsets.UTF_8)
        } catch (e: Exception) {
            Log.w(TAG, "decrypt failed: ${e.javaClass.simpleName}")
            null
        }
    }
}
