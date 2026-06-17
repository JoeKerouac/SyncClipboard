#ifndef SYNC_CLIPBOARD_WS_CLIENT_H
#define SYNC_CLIPBOARD_WS_CLIENT_H

#include "auth_http.h"
#include "config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the {bearer.<jwt>} subprotocol string used for the v2 handshake.
 * Caller-supplied buffer must be at least token length + 16 bytes. */
int ws_build_bearer_protocol(const char *token, char *out, size_t out_len);

/* Convenience: log in (or refresh) tokens against the configured server.
 * Updates *tokens on success. */
int ws_acquire_tokens(const ClientConfig *cfg, AuthTokens *tokens);

#ifdef __cplusplus
}
#endif

#endif
