#include "crypto.h"
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
