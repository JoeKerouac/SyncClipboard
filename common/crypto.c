#include "crypto.h"
#include "codec.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define AES_KEY_LEN 32
#define AES_IV_LEN  16

static int hex_to_bytes(const char *hex, unsigned char *out, int max_len) {
    int len = (int)strlen(hex) / 2;
    if (len > max_len) len = max_len;
    for (int i = 0; i < len; i++) {
        unsigned int byte;
        sscanf(hex + 2 * i, "%02x", &byte);
        out[i] = (unsigned char)byte;
    }
    return len;
}

static char *base64_encode(const unsigned char *data, int len) {
    BIO *bio, *b64;
    BUF_MEM *bptr;

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, data, len);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bptr);

    char *result = (char *)malloc(bptr->length + 1);
    if (result) {
        memcpy(result, bptr->data, bptr->length);
        result[bptr->length] = '\0';
    }
    BIO_free_all(bio);
    return result;
}

static unsigned char *base64_decode(const char *input, int *out_len) {
    int input_len = (int)strlen(input);
    int max_len = input_len * 3 / 4 + 4;
    unsigned char *buf = (unsigned char *)malloc(max_len);
    if (!buf) return NULL;

    BIO *bio, *b64;
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new_mem_buf(input, input_len);
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    *out_len = BIO_read(bio, buf, max_len);
    BIO_free_all(bio);

    if (*out_len < 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

char *aes_encrypt(const char *plaintext, size_t plain_len, const char *key_hex) {
    unsigned char key[AES_KEY_LEN];
    unsigned char iv[AES_IV_LEN];

    memset(key, 0, sizeof(key));
    hex_to_bytes(key_hex, key, AES_KEY_LEN);
    RAND_bytes(iv, AES_IV_LEN);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return NULL;
    }

    int max_out = (int)plain_len + EVP_CIPHER_block_size(EVP_aes_256_cbc());
    unsigned char *ciphertext = (unsigned char *)malloc(max_out);
    if (!ciphertext) { EVP_CIPHER_CTX_free(ctx); return NULL; }

    int out_len = 0, final_len = 0;
    EVP_EncryptUpdate(ctx, ciphertext, &out_len, (const unsigned char *)plaintext, (int)plain_len);
    EVP_EncryptFinal_ex(ctx, ciphertext + out_len, &final_len);
    out_len += final_len;
    EVP_CIPHER_CTX_free(ctx);

    /* prepend IV to ciphertext: IV(16) + ciphertext */
    int total = AES_IV_LEN + out_len;
    unsigned char *combined = (unsigned char *)malloc(total);
    if (!combined) { free(ciphertext); return NULL; }
    memcpy(combined, iv, AES_IV_LEN);
    memcpy(combined + AES_IV_LEN, ciphertext, out_len);
    free(ciphertext);

    char *b64 = base64_encode(combined, total);
    free(combined);
    return b64;
}

char *aes_decrypt(const char *b64_input, const char *key_hex, size_t *out_len) {
    int decoded_len = 0;
    unsigned char *decoded = base64_decode(b64_input, &decoded_len);
    if (!decoded || decoded_len < AES_IV_LEN + 1) {
        free(decoded);
        return NULL;
    }

    unsigned char key[AES_KEY_LEN];
    memset(key, 0, sizeof(key));
    hex_to_bytes(key_hex, key, AES_KEY_LEN);

    unsigned char *iv = decoded;
    unsigned char *ciphertext = decoded + AES_IV_LEN;
    int cipher_len = decoded_len - AES_IV_LEN;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(decoded); return NULL; }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        free(decoded);
        return NULL;
    }

    unsigned char *plaintext = (unsigned char *)malloc(cipher_len + 1);
    if (!plaintext) { EVP_CIPHER_CTX_free(ctx); free(decoded); return NULL; }

    int plen = 0, flen = 0;
    if (EVP_DecryptUpdate(ctx, plaintext, &plen, ciphertext, cipher_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        free(decoded);
        free(plaintext);
        return NULL;
    }
    if (EVP_DecryptFinal_ex(ctx, plaintext + plen, &flen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        free(decoded);
        free(plaintext);
        return NULL;
    }
    plen += flen;
    EVP_CIPHER_CTX_free(ctx);
    free(decoded);

    plaintext[plen] = '\0';
    if (out_len) *out_len = (size_t)plen;
    return (char *)plaintext;
}

/* ==== AES-256-GCM (v2 cipher) ==== */

#define GCM_NONCE_LEN 12
#define GCM_TAG_LEN   16
#define GCM_VERSION   0x02

char *aes_gcm_encrypt(const char *plaintext, size_t plain_len, const char *key_hex) {
    unsigned char key[AES_KEY_LEN];
    if (sc_hex_to_bytes(key_hex, key, AES_KEY_LEN) != 0) return NULL;

    unsigned char nonce[GCM_NONCE_LEN];
    if (RAND_bytes(nonce, GCM_NONCE_LEN) != 1) return NULL;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    char *result = NULL;
    unsigned char *ct = NULL;
    unsigned char *frame = NULL;

    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_NONCE_LEN, NULL) != 1) break;
        if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) break;

        ct = (unsigned char *)malloc(plain_len + 16);
        if (!ct) break;
        int out_len = 0, final_len = 0;
        if (EVP_EncryptUpdate(ctx, ct, &out_len,
                              (const unsigned char *)plaintext, (int)plain_len) != 1) break;
        if (EVP_EncryptFinal_ex(ctx, ct + out_len, &final_len) != 1) break;
        out_len += final_len;

        unsigned char tag[GCM_TAG_LEN];
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, tag) != 1) break;

        size_t frame_len = 1 + GCM_NONCE_LEN + (size_t)out_len + GCM_TAG_LEN;
        frame = (unsigned char *)malloc(frame_len);
        if (!frame) break;
        frame[0] = GCM_VERSION;
        memcpy(frame + 1, nonce, GCM_NONCE_LEN);
        memcpy(frame + 1 + GCM_NONCE_LEN, ct, (size_t)out_len);
        memcpy(frame + 1 + GCM_NONCE_LEN + (size_t)out_len, tag, GCM_TAG_LEN);

        result = sc_base64_encode(frame, frame_len);
    } while (0);

    EVP_CIPHER_CTX_free(ctx);
    free(ct);
    free(frame);
    return result;
}

char *aes_gcm_decrypt(const char *b64_input, const char *key_hex, size_t *out_len) {
    unsigned char key[AES_KEY_LEN];
    if (sc_hex_to_bytes(key_hex, key, AES_KEY_LEN) != 0) return NULL;

    size_t frame_len = 0;
    unsigned char *frame = sc_base64_decode(b64_input, &frame_len);
    if (!frame) return NULL;

    char *plaintext = NULL;
    if (frame_len < 1 + GCM_NONCE_LEN + GCM_TAG_LEN) goto cleanup;
    if (frame[0] != GCM_VERSION) goto cleanup;

    size_t ct_len = frame_len - 1 - GCM_NONCE_LEN - GCM_TAG_LEN;
    unsigned char *nonce = frame + 1;
    unsigned char *ct = frame + 1 + GCM_NONCE_LEN;
    unsigned char *tag = frame + 1 + GCM_NONCE_LEN + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) goto cleanup;

    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_NONCE_LEN, NULL) != 1) break;
        if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) break;

        plaintext = (char *)malloc(ct_len + 1);
        if (!plaintext) break;
        int plen = 0, flen = 0;
        if (EVP_DecryptUpdate(ctx, (unsigned char *)plaintext, &plen, ct, (int)ct_len) != 1) {
            free(plaintext); plaintext = NULL; break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, tag) != 1) {
            free(plaintext); plaintext = NULL; break;
        }
        if (EVP_DecryptFinal_ex(ctx, (unsigned char *)plaintext + plen, &flen) != 1) {
            free(plaintext); plaintext = NULL; break;
        }
        plen += flen;
        plaintext[plen] = '\0';
        if (out_len) *out_len = (size_t)plen;
    } while (0);

    EVP_CIPHER_CTX_free(ctx);
cleanup:
    free(frame);
    return plaintext;
}
