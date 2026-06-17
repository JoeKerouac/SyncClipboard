#include "auth_http.h"
#include "log.h"
#include "cJSON.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} HttpBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    HttpBuf *buf = (HttpBuf *)userdata;
    if (buf->len + total + 1 > buf->cap) {
        size_t new_cap = buf->cap == 0 ? 1024 : buf->cap * 2;
        while (new_cap < buf->len + total + 1) new_cap *= 2;
        char *p = (char *)realloc(buf->data, new_cap);
        if (!p) return 0;
        buf->data = p;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static int post_json(const ClientConfig *cfg, const char *url, const char *body, HttpBuf *out, long *http_code) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    out->data = NULL; out->len = 0; out->cap = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (cfg->use_tls) {
        if (cfg->skip_tls_verify) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        } else if (cfg->ca_cert_path[0]) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, cfg->ca_cert_path);
        }
    }

    CURLcode rc = curl_easy_perform(curl);
    if (http_code) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        LOG_WARN("auth http error: %s", curl_easy_strerror(rc));
        free(out->data);
        out->data = NULL;
        return -1;
    }
    return 0;
}

static int parse_token_response(const char *json_text, AuthTokens *out) {
    cJSON *root = cJSON_Parse(json_text);
    if (!root) return -1;
    int rc = -1;
    cJSON *access = cJSON_GetObjectItemCaseSensitive(root, "accessToken");
    cJSON *refresh = cJSON_GetObjectItemCaseSensitive(root, "refreshToken");
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expiresInSec");
    if (cJSON_IsString(access) && access->valuestring &&
        cJSON_IsString(refresh) && refresh->valuestring) {
        strncpy(out->access_token, access->valuestring, sizeof(out->access_token) - 1);
        out->access_token[sizeof(out->access_token) - 1] = '\0';
        strncpy(out->refresh_token, refresh->valuestring, sizeof(out->refresh_token) - 1);
        out->refresh_token[sizeof(out->refresh_token) - 1] = '\0';
        out->expires_in_sec = cJSON_IsNumber(expires) ? (long)expires->valuedouble : 0;
        rc = 0;
    }
    cJSON_Delete(root);
    return rc;
}

static int build_url(const ClientConfig *cfg, const char *path, char *out, size_t out_len) {
    const char *scheme = cfg->use_tls ? "https" : "http";
    int n = snprintf(out, out_len, "%s://%s:%d%s", scheme, cfg->server_host, cfg->server_port, path);
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

int auth_http_login(const ClientConfig *cfg, AuthTokens *out) {
    char url[512];
    if (build_url(cfg, "/api/v2/auth/login", url, sizeof(url)) != 0) return -1;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "username", cfg->username);
    cJSON_AddStringToObject(body, "password", cfg->password);
    cJSON_AddStringToObject(body, "deviceId", cfg->device_id);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return -1;

    HttpBuf buf;
    long http_code = 0;
    int rc = post_json(cfg, url, body_str, &buf, &http_code);
    free(body_str);
    if (rc != 0) return -1;

    if (http_code != 200) {
        LOG_WARN("login HTTP %ld", http_code);
        free(buf.data);
        return -1;
    }
    int parsed = parse_token_response(buf.data, out);
    free(buf.data);
    return parsed;
}

int auth_http_refresh(const ClientConfig *cfg, AuthTokens *out) {
    if (out->refresh_token[0] == '\0') {
        return auth_http_login(cfg, out);
    }
    char url[512];
    if (build_url(cfg, "/api/v2/auth/refresh", url, sizeof(url)) != 0) return -1;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "refreshToken", out->refresh_token);
    cJSON_AddStringToObject(body, "deviceId", cfg->device_id);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return -1;

    HttpBuf buf;
    long http_code = 0;
    int rc = post_json(cfg, url, body_str, &buf, &http_code);
    free(body_str);
    if (rc != 0) return -1;

    if (http_code != 200) {
        LOG_INFO("refresh failed (%ld); falling back to login", http_code);
        free(buf.data);
        return auth_http_login(cfg, out);
    }
    int parsed = parse_token_response(buf.data, out);
    free(buf.data);
    return parsed;
}
