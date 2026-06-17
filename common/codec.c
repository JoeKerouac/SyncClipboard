#include "codec.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *sc_base64_encode(const unsigned char *data, size_t len) {
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO *bio = BIO_push(b64, mem);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    if (BIO_write(bio, data, (int)len) <= 0) {
        BIO_free_all(bio);
        return NULL;
    }
    BIO_flush(bio);
    BUF_MEM *bptr;
    BIO_get_mem_ptr(bio, &bptr);
    char *out = (char *)malloc(bptr->length + 1);
    if (!out) { BIO_free_all(bio); return NULL; }
    memcpy(out, bptr->data, bptr->length);
    out[bptr->length] = '\0';
    BIO_free_all(bio);
    return out;
}

/* Convert base64url back to standard base64 (or leave standard intact) and
 * pad with '=' to a multiple of 4. Returns a freshly malloc'd nul-terminated
 * string the caller must free. */
static char *normalise_base64(const char *input) {
    size_t len = strlen(input);
    /* worst case: pad to multiple of 4. */
    size_t cap = len + 4 + 1;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (size_t i = 0; i < len; i++) {
        char c = input[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        buf[i] = c;
    }
    while ((len % 4) != 0) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = '=';
    }
    buf[len] = '\0';
    return buf;
}

unsigned char *sc_base64_decode(const char *input, size_t *out_len) {
    if (!input || !out_len) return NULL;
    char *normalised = normalise_base64(input);
    if (!normalised) return NULL;

    size_t in_len = strlen(normalised);
    size_t max_out = in_len * 3 / 4 + 4;
    unsigned char *buf = (unsigned char *)malloc(max_out);
    if (!buf) { free(normalised); return NULL; }

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new_mem_buf(normalised, (int)in_len);
    BIO *bio = BIO_push(b64, mem);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    int decoded = BIO_read(bio, buf, (int)max_out);
    BIO_free_all(bio);
    free(normalised);
    if (decoded < 0) { free(buf); return NULL; }
    *out_len = (size_t)decoded;
    return buf;
}

int sc_hex_to_bytes(const char *hex, unsigned char *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) return -1;
        out[i] = (unsigned char)byte;
    }
    return 0;
}

void sc_bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex) {
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out_hex[i * 2]     = digits[(bytes[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    out_hex[len * 2] = '\0';
}
