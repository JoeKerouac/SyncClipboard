/*
 * Basic socket helpers and shared internal utilities used across the ft/
 * translation units. Anything declared in ft_internal.h lives here.
 */

#include "ft_internal.h"
#include "../log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/rand.h>

uint64_t g_ft_max_size = FT_MAX_FILE_SIZE;

void ft_set_max_file_size(uint64_t max_bytes) {
    g_ft_max_size = max_bytes;
}

void ft_close(ft_sock_t sock) {
    if (sock == FT_INVALID_SOCK) return;
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

void ft_set_nonblocking(ft_sock_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int fl = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, fl | O_NONBLOCK);
#endif
}

void ft_set_blocking(ft_sock_t sock) {
#ifdef _WIN32
    u_long mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int fl = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

void ft_generate_uuid(char *out) {
    unsigned char buf[16];
    if (RAND_bytes(buf, 16) != 1) {
        for (int i = 0; i < 16; i++) buf[i] = (unsigned char)(rand() & 0xFF);
    }
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             buf[0],buf[1],buf[2],buf[3], buf[4],buf[5], buf[6],buf[7],
             buf[8],buf[9], buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
}

int ft_parse_addr(const char *s, struct sockaddr_in *sa) {
    char ip[64];
    strncpy(ip, s, sizeof(ip) - 1); ip[63] = '\0';
    char *colon = strrchr(ip, ':');
    if (!colon) return -1;
    *colon = '\0';
    int port = atoi(colon + 1);
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &sa->sin_addr) != 1) return -1;
    return 0;
}

void ft_optimize_socket(ft_sock_t sock) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
    int bufsize = 512 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&bufsize, sizeof(bufsize));
#ifdef _WIN32
    DWORD timeout = 30000;
#else
    struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
#endif
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
}

void ft_set_sock_timeout(ft_sock_t sock, int ms) {
#ifdef _WIN32
    DWORD dw = (DWORD)ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dw, sizeof(dw));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&dw, sizeof(dw));
#else
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

int ft_send_all(ft_sock_t sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, (const char *)(p + sent), (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

int ft_recv_all(ft_sock_t sock, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        int n = recv(sock, (char *)(p + got), (int)(len - got), 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

long long ft_now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}
