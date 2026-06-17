/*
 * Wire protocol for file transfer over an authenticated socket.
 *
 * Frame layout: FT_MAGIC(4) || file_id(36) || file_size(8 BE) || data(N).
 * The server-side max_size cap is enforced when receiving so a hostile peer
 * cannot allocate arbitrary memory by claiming a huge file.
 */

#include "ft_internal.h"
#include "../log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FT_CHUNK_SIZE (256 * 1024)

int ft_send_file(ft_sock_t sock, const FtFileInfo *info) {
    if (ft_send_all(sock, FT_MAGIC, FT_MAGIC_LEN) != 0) return -1;
    if (ft_send_all(sock, info->file_id, FT_UUID_LEN) != 0) return -1;

    uint8_t sb[8]; uint64_t fs = info->file_size;
    for (int i = 7; i >= 0; i--) { sb[i] = fs & 0xFF; fs >>= 8; }
    if (ft_send_all(sock, sb, 8) != 0) return -1;

    if (ft_send_all(sock, info->data, info->data_len) != 0) return -1;
    return 0;
}

int ft_recv_file(ft_sock_t sock, FtFileInfo *info) {
    char magic[FT_MAGIC_LEN];
    if (ft_recv_all(sock, magic, FT_MAGIC_LEN) != 0) return -1;
    if (memcmp(magic, FT_MAGIC, FT_MAGIC_LEN) != 0) return -1;

    char fid[FT_UUID_LEN + 1];
    if (ft_recv_all(sock, fid, FT_UUID_LEN) != 0) return -1;
    fid[FT_UUID_LEN] = '\0';
    strncpy(info->file_id, fid, sizeof(info->file_id) - 1);

    uint8_t sb[8];
    if (ft_recv_all(sock, sb, 8) != 0) return -1;
    uint64_t fs = 0;
    for (int i = 0; i < 8; i++) fs = (fs << 8) | sb[i];

    if (fs == 0 || fs > g_ft_max_size) {
        LOG_WARN("[FT] File size rejected: %llu bytes (max: %llu)",
                 (unsigned long long)fs, (unsigned long long)g_ft_max_size);
        return -1;
    }

    info->file_size = fs;
    info->data_len  = (size_t)fs;

    info->data = (uint8_t *)malloc(info->data_len);
    if (!info->data) return -1;
    if (ft_recv_all(sock, info->data, info->data_len) != 0) {
        free(info->data); info->data = NULL; return -1;
    }
    return 0;
}

int ft_send_file_from_path(ft_sock_t sock, const char *file_id,
                           uint64_t file_size, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (ft_send_all(sock, FT_MAGIC, FT_MAGIC_LEN) != 0) { fclose(f); return -1; }
    if (ft_send_all(sock, file_id, FT_UUID_LEN) != 0) { fclose(f); return -1; }

    uint8_t sb[8]; uint64_t fs = file_size;
    for (int i = 7; i >= 0; i--) { sb[i] = fs & 0xFF; fs >>= 8; }
    if (ft_send_all(sock, sb, 8) != 0) { fclose(f); return -1; }

    uint8_t *chunk = (uint8_t *)malloc(FT_CHUNK_SIZE);
    if (!chunk) { fclose(f); return -1; }

    uint64_t remaining = file_size;
    int ret = 0;
    while (remaining > 0) {
        size_t to_read = remaining > FT_CHUNK_SIZE ? FT_CHUNK_SIZE : (size_t)remaining;
        size_t n = fread(chunk, 1, to_read, f);
        if (n != to_read) { ret = -1; break; }
        if (ft_send_all(sock, chunk, n) != 0) { ret = -1; break; }
        remaining -= n;
    }

    free(chunk);
    fclose(f);
    return ret;
}

int ft_recv_file_to_path(ft_sock_t sock, FtFileInfo *info,
                         const char *output_path) {
    char magic[FT_MAGIC_LEN];
    if (ft_recv_all(sock, magic, FT_MAGIC_LEN) != 0) return -1;
    if (memcmp(magic, FT_MAGIC, FT_MAGIC_LEN) != 0) return -1;

    char fid[FT_UUID_LEN + 1];
    if (ft_recv_all(sock, fid, FT_UUID_LEN) != 0) return -1;
    fid[FT_UUID_LEN] = '\0';
    strncpy(info->file_id, fid, sizeof(info->file_id) - 1);

    uint8_t sb[8];
    if (ft_recv_all(sock, sb, 8) != 0) return -1;
    uint64_t fs = 0;
    for (int i = 0; i < 8; i++) fs = (fs << 8) | sb[i];

    if (fs == 0 || fs > g_ft_max_size) {
        LOG_WARN("[FT] File size rejected: %llu bytes (max: %llu)",
                 (unsigned long long)fs, (unsigned long long)g_ft_max_size);
        return -1;
    }

    info->file_size = fs;
    info->data_len  = 0;
    info->data      = NULL;

    FILE *f = fopen(output_path, "wb");
    if (!f) return -1;

    uint8_t *chunk = (uint8_t *)malloc(FT_CHUNK_SIZE);
    if (!chunk) { fclose(f); return -1; }

    uint64_t remaining = fs;
    int ret = 0;
    while (remaining > 0) {
        size_t to_read = remaining > FT_CHUNK_SIZE ? FT_CHUNK_SIZE : (size_t)remaining;
        if (ft_recv_all(sock, chunk, to_read) != 0) { ret = -1; break; }
        if (fwrite(chunk, 1, to_read, f) != to_read) { ret = -1; break; }
        remaining -= to_read;
    }

    free(chunk);
    fclose(f);
    if (ret != 0) remove(output_path);
    return ret;
}
