#ifndef SYNC_CLIPBOARD_AUTH_HTTP_H
#define SYNC_CLIPBOARD_AUTH_HTTP_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char access_token[1024];
    char refresh_token[1024];
    long  expires_in_sec;
} AuthTokens;

/* Login against POST /api/v2/auth/login. Returns 0 on success. */
int auth_http_login(const ClientConfig *cfg, AuthTokens *out);

/* Refresh tokens against POST /api/v2/auth/refresh. Falls back to a fresh
 * login when the refresh token is missing/expired. Returns 0 on success. */
int auth_http_refresh(const ClientConfig *cfg, AuthTokens *out);

#ifdef __cplusplus
}
#endif

#endif
