/*
 * P2P file transfer authentication using HMAC-SHA256 over the file_id with
 * the shared AES key as the HMAC key. Establishes mutual proof-of-key
 * before either side trusts the data stream.
 */

#include "ft_internal.h"

#include <openssl/hmac.h>
#include <openssl/crypto.h>

#include <stdio.h>
#include <string.h>

static int ft_compute_hmac(const char *file_id, const char *key_hex,
                           unsigned char *out_hmac) {
    unsigned char key[32];
    memset(key, 0, sizeof(key));
    int klen = (int)strlen(key_hex) / 2;
    if (klen > 32) klen = 32;
    for (int i = 0; i < klen; i++) {
        unsigned int byte;
        if (sscanf(key_hex + 2 * i, "%02x", &byte) != 1) return -1;
        key[i] = (unsigned char)byte;
    }
    if (klen < 32) return -1;
    unsigned int hmac_len = 0;
    HMAC(EVP_sha256(), key, klen, (const unsigned char *)file_id,
         strlen(file_id), out_hmac, &hmac_len);
    return (hmac_len == 32) ? 0 : -1;
}

int ft_auth_send(ft_sock_t sock, const char *file_id, const char *key_hex) {
    unsigned char hmac[32];
    if (ft_compute_hmac(file_id, key_hex, hmac) != 0) return -1;
    char buf[36];
    memcpy(buf, "AUTH", 4);
    memcpy(buf + 4, hmac, 32);
    if (ft_send_all(sock, buf, 36) != 0) return -1;
    char resp[4];
    if (ft_recv_all(sock, resp, 4) != 0) return -1;
    return (memcmp(resp, "AKOK", 4) == 0) ? 0 : -1;
}

int ft_auth_recv(ft_sock_t sock, const char *file_id, const char *key_hex) {
    char buf[36];
    if (ft_recv_all(sock, buf, 36) != 0) return -1;
    if (memcmp(buf, "AUTH", 4) != 0) {
        ft_send_all(sock, "FAIL", 4);
        return -1;
    }
    unsigned char expected[32];
    if (ft_compute_hmac(file_id, key_hex, expected) != 0) {
        ft_send_all(sock, "FAIL", 4);
        return -1;
    }
    if (CRYPTO_memcmp(buf + 4, expected, 32) != 0) {
        ft_send_all(sock, "FAIL", 4);
        return -1;
    }
    if (ft_send_all(sock, "AKOK", 4) != 0) return -1;
    return 0;
}
