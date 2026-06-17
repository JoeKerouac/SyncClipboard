/*
 * SHA-256 + base64 helpers used by the file_transfer module. Uses OpenSSL
 * EVP under the hood.
 */

#include "ft_internal.h"

#include <openssl/sha.h>
#include <openssl/evp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FT_CHUNK_SIZE (256 * 1024)

void ft_sha256(const uint8_t *data, size_t len, char *out_hex) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(out_hex + i * 2, "%02x", hash[i]);
    out_hex[SHA256_DIGEST_LENGTH * 2] = '\0';
}

int ft_sha256_file(const char *path, char *out_hex) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    uint8_t buf[FT_CHUNK_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        EVP_DigestUpdate(ctx, buf, n);
    fclose(f);

    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned int hash_len;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(out_hex + i * 2, "%02x", hash[i]);
    out_hex[SHA256_DIGEST_LENGTH * 2] = '\0';
    return 0;
}

char *ft_base64_encode(const uint8_t *data, size_t len, size_t *out_len) {
    size_t b64_len = 4 * ((len + 2) / 3) + 1;
    char *b64 = (char *)malloc(b64_len);
    if (!b64) return NULL;
    int n = EVP_EncodeBlock((unsigned char *)b64, data, (int)len);
    b64[n] = '\0';
    if (out_len) *out_len = n;
    return b64;
}

uint8_t *ft_base64_decode(const char *b64, size_t b64_len, size_t *out_len) {
    size_t max_len = 3 * b64_len / 4 + 1;
    uint8_t *data = (uint8_t *)malloc(max_len);
    if (!data) return NULL;
    int n = EVP_DecodeBlock(data, (const unsigned char *)b64, (int)b64_len);
    if (n < 0) { free(data); return NULL; }
    if (b64_len > 1 && b64[b64_len - 1] == '=') n--;
    if (b64_len > 2 && b64[b64_len - 2] == '=') n--;
    if (out_len) *out_len = n;
    return data;
}
