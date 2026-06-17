#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void config_set_defaults(ClientConfig *cfg) {
    memset(cfg, 0, sizeof(ClientConfig));
    /* All sensitive fields intentionally start blank. config_load() will
     * refuse to start if any of them is missing in the user's config file.
     * This prevents older builds' shared default credentials from leaking
     * across upgrades. */
    cfg->server_port = 8080;
    strncpy(cfg->server_path, "/ws/v2/clipboard", MAX_URL_LEN - 1);
    cfg->file_transfer_level = 3;  /* default: fully open */
    cfg->max_transfer_size = 500;
    cfg->use_tls = 0;             /* default: no TLS */
    cfg->log_level = 1;           /* 0=DBG 1=INF 2=WRN 3=ERR */
    cfg->max_log_size_mb = 10;
}

static void trim(char *s) {
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
}

static int is_hex(const char *s, size_t expected_len) {
    if (strlen(s) != expected_len) return 0;
    for (size_t i = 0; i < expected_len; i++) {
        if (!isxdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

int config_load(const char *path, ClientConfig *cfg) {
    config_set_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[FATAL] Config file not found: %s\n", path);
        fprintf(stderr, "Create it with the required fields: server_host, server_port,\n");
        fprintf(stderr, "  server_key, username, password, aes_key (64 hex chars), device_id\n");
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcmp(key, "server_host") == 0)
            strncpy(cfg->server_host, val, MAX_URL_LEN - 1);
        else if (strcmp(key, "server_port") == 0)
            cfg->server_port = atoi(val);
        else if (strcmp(key, "server_path") == 0)
            strncpy(cfg->server_path, val, MAX_URL_LEN - 1);
        else if (strcmp(key, "server_key") == 0)
            strncpy(cfg->server_key, val, MAX_KEY_LEN - 1);
        else if (strcmp(key, "username") == 0)
            strncpy(cfg->username, val, MAX_USER_LEN - 1);
        else if (strcmp(key, "password") == 0)
            strncpy(cfg->password, val, MAX_PASS_LEN - 1);
        else if (strcmp(key, "aes_key") == 0)
            strncpy(cfg->aes_key, val, MAX_KEY_LEN - 1);
        else if (strcmp(key, "device_id") == 0)
            strncpy(cfg->device_id, val, MAX_USER_LEN - 1);
        else if (strcmp(key, "file_transfer_level") == 0)
            cfg->file_transfer_level = atoi(val);
        else if (strcmp(key, "max_transfer_size") == 0)
            cfg->max_transfer_size = atoi(val);
        else if (strcmp(key, "use_tls") == 0)
            cfg->use_tls = atoi(val);
        else if (strcmp(key, "log_level") == 0)
            cfg->log_level = atoi(val);
        else if (strcmp(key, "max_log_size_mb") == 0)
            cfg->max_log_size_mb = atoi(val);
    }
    fclose(f);

    /* Strict validation: refuse to run with a blank or malformed config so
     * callers cannot silently fall back to insecure defaults. */
    int fatal = 0;
    if (cfg->server_host[0] == '\0') {
        fprintf(stderr, "[FATAL] Required field missing: server_host\n");
        fatal = 1;
    }
    if (cfg->server_port <= 0 || cfg->server_port > 65535) {
        fprintf(stderr, "[FATAL] Invalid server_port: %d\n", cfg->server_port);
        fatal = 1;
    }
    if (cfg->server_key[0] == '\0') {
        fprintf(stderr, "[FATAL] Required field missing: server_key\n");
        fatal = 1;
    }
    if (cfg->username[0] == '\0') {
        fprintf(stderr, "[FATAL] Required field missing: username\n");
        fatal = 1;
    }
    if (cfg->password[0] == '\0') {
        fprintf(stderr, "[FATAL] Required field missing: password\n");
        fatal = 1;
    }
    if (cfg->aes_key[0] == '\0') {
        fprintf(stderr, "[FATAL] Required field missing: aes_key\n");
        fatal = 1;
    } else if (!is_hex(cfg->aes_key, 64)) {
        fprintf(stderr, "[FATAL] aes_key must be 64 hex characters (AES-256)\n");
        fatal = 1;
    }
    if (cfg->device_id[0] == '\0') {
        fprintf(stderr, "[FATAL] Required field missing: device_id\n");
        fatal = 1;
    }
    if (fatal) return -1;

    /* Do not log key material or the full config; just confirm the load. */
    fprintf(stderr, "Config loaded from %s (server=%s:%d, user=%s, device=%s)\n",
            path, cfg->server_host, cfg->server_port, cfg->username, cfg->device_id);
    return 0;
}
