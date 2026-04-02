#ifndef SYNC_CLIPBOARD_CRYPTO_H
#define SYNC_CLIPBOARD_CRYPTO_H

#include <stddef.h>

/**
 * AES-256-CBC encrypt.
 * Returns base64-encoded ciphertext (caller must free), or NULL on failure.
 * IV is randomly generated and prepended to the ciphertext before base64 encoding.
 */
char *aes_encrypt(const char *plaintext, size_t plain_len, const char *key_hex);

/**
 * AES-256-CBC decrypt.
 * Input: base64-encoded string (IV + ciphertext).
 * Returns plaintext (caller must free), or NULL on failure.
 * out_len receives the plaintext length.
 */
char *aes_decrypt(const char *b64_input, const char *key_hex, size_t *out_len);

#endif
