#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void config_set_defaults(ClientConfig *cfg) {
    memset(cfg, 0, sizeof(ClientConfig));
    strncpy(cfg->server_host, "127.0.0.1", MAX_URL_LEN - 1);
    cfg->server_port = 8080;
    strncpy(cfg->server_path, "/ws/clipboard", MAX_URL_LEN - 1);
    strncpy(cfg->server_key, "my-secret-server-key", MAX_KEY_LEN - 1);
    strncpy(cfg->username, "admin", MAX_USER_LEN - 1);
    strncpy(cfg->password, "admin123", MAX_PASS_LEN - 1);
    /* 64 hex chars = 32 bytes = AES-256 key */
    strncpy(cfg->aes_key, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", MAX_KEY_LEN - 1);
    strncpy(cfg->device_id, "default-device", MAX_USER_LEN - 1);
    cfg->file_transfer_level = 3;  /* default: fully open */
    cfg->max_transfer_size = 500;
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

int config_load(const char *path, ClientConfig *cfg) {
    config_set_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Config file not found: %s, using defaults\n", path);
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
        else if (strcmp(key, "log_level") == 0)
            cfg->log_level = atoi(val);
        else if (strcmp(key, "max_log_size_mb") == 0)
            cfg->max_log_size_mb = atoi(val);
    }
    fclose(f);
    printf("Config loaded from %s\n", path);
    return 0;
}
