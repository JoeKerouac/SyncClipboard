#include "ws_client.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

int ws_build_bearer_protocol(const char *token, char *out, size_t out_len) {
    int n = snprintf(out, out_len, "bearer.%s", token);
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

int ws_acquire_tokens(const ClientConfig *cfg, AuthTokens *tokens) {
    if (!cfg || !tokens) return -1;
    if (tokens->refresh_token[0] != '\0') {
        if (auth_http_refresh(cfg, tokens) == 0) {
            LOG_INFO("token refreshed");
            return 0;
        }
    }
    if (auth_http_login(cfg, tokens) == 0) {
        LOG_INFO("login successful (expires in %lds)", tokens->expires_in_sec);
        return 0;
    }
    LOG_WARN("login failed");
    return -1;
}
