#ifndef SYNC_CLIPBOARD_MSG_H
#define SYNC_CLIPBOARD_MSG_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build a v2-compliant message with the given type and the user-supplied
 * cJSON object as the body. The function moves ownership of `body` into the
 * returned root and returns a malloc'd serialised JSON string the caller
 * must free. Returns NULL on allocation failure. */
char *sc_msg_build(const char *type, cJSON *body);

/* Convenience: build a clipboard message wrapping an already-encrypted
 * payload (Base64URL of the GCM frame). */
char *sc_msg_clipboard(const char *encrypted_b64);

/* Convenience: build a hello message announcing the client. */
char *sc_msg_hello(const char *client_type, const char *device_id, const char *app_version);

#ifdef __cplusplus
}
#endif

#endif
