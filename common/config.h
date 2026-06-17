#ifndef SYNC_CLIPBOARD_CONFIG_H
#define SYNC_CLIPBOARD_CONFIG_H

#define MAX_URL_LEN     256
#define MAX_KEY_LEN     256
#define MAX_USER_LEN    128
#define MAX_PASS_LEN    128
#define MAX_PATH_LEN    512

typedef struct {
    char server_host[MAX_URL_LEN];
    int  server_port;
    char server_path[MAX_URL_LEN];
    char server_key[MAX_KEY_LEN];
    char username[MAX_USER_LEN];
    char password[MAX_PASS_LEN];
    char aes_key[MAX_KEY_LEN];    /* hex-encoded AES-256 key (64 hex chars) */
    char device_id[MAX_USER_LEN];
    int  file_transfer_level;     /* 0=off, 1=LAN, 2=+NAT, 3=+relay(full) */
    int  max_transfer_size;       /* 文件传输最大大小，单位MB */
    int  use_tls;                 /* 0=http/ws, 1=https/wss */
    int  log_level;               /* 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR */
    int  max_log_size_mb;         /* 单个日志文件最大MB, 超过自动滚动 */
} ClientConfig;

/**
 * Load config from a file (INI-like format).
 * Returns 0 on success, -1 on failure.
 */
int config_load(const char *path, ClientConfig *cfg);

/**
 * Set default values for config.
 */
void config_set_defaults(ClientConfig *cfg);

#endif
