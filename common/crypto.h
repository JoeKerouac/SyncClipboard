#ifndef SYNC_CLIPBOARD_CRYPTO_H
#define SYNC_CLIPBOARD_CRYPTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Legacy AES-256-CBC. Retained only so old peers can be decrypted during the
 * migration window. New code should use the GCM variants below. */
char *aes_encrypt(const char *plaintext, size_t plain_len, const char *key_hex);
char *aes_decrypt(const char *b64_input, const char *key_hex, size_t *out_len);

/* AES-256-GCM (v2 cipher).
 *
 * Frame layout: 0x02 || nonce(12) || ciphertext || tag(16), encoded as
 * base64url without padding so it transports cleanly inside JSON.
 *
 * aes_gcm_encrypt: returns malloc'd base64url string, caller frees.
 * aes_gcm_decrypt: returns malloc'd plaintext (NUL-terminated), caller frees.
 *                  *out_len is set to the plaintext length on success.
 *                  Returns NULL on auth failure or any malformed input.
 */
char *aes_gcm_encrypt(const char *plaintext, size_t plain_len, const char *key_hex);
char *aes_gcm_decrypt(const char *b64_input, const char *key_hex, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
