#ifndef SYNC_CLIPBOARD_CODEC_H
#define SYNC_CLIPBOARD_CODEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard base64 (no newlines). Caller frees the returned string. */
char *sc_base64_encode(const unsigned char *data, size_t len);

/* Decodes either standard or URL-safe base64, with or without '=' padding.
 * Returns malloc'd buffer of length *out_len, or NULL on error. */
unsigned char *sc_base64_decode(const char *input, size_t *out_len);

/* Hex helpers. */
int sc_hex_to_bytes(const char *hex, unsigned char *out, size_t out_len);
void sc_bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex);

#ifdef __cplusplus
}
#endif

#endif
