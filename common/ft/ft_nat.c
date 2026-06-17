/*
 * NAT traversal helpers: UDP-based public endpoint discovery and TCP
 * "simultaneous open" hole punching driven by UDP punch packets sent
 * from the same source port.
 */

#include "ft_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ft_discover_public(const char *server_host, int udp_port,
                       int local_port, char *out_endpoint, int out_len) {
    ft_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == FT_INVALID_SOCK) return -1;

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char *)&opt, sizeof(opt));
#endif

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = htons(local_port);
    bind(s, (struct sockaddr *)&la, sizeof(la));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(udp_port);
    if (inet_pton(AF_INET, server_host, &sa.sin_addr) != 1) {
        struct addrinfo hints_ai = {0}, *result_ai;
        hints_ai.ai_family = AF_INET;
        hints_ai.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(server_host, NULL, &hints_ai, &result_ai) != 0) {
            ft_close(s);
            return -1;
        }
        struct sockaddr_in *resolved = (struct sockaddr_in *)result_ai->ai_addr;
        memcpy(&sa.sin_addr, &resolved->sin_addr, sizeof(sa.sin_addr));
        freeaddrinfo(result_ai);
    }

    const char *msg = "DISCOVER";
    sendto(s, msg, (int)strlen(msg), 0, (struct sockaddr *)&sa, sizeof(sa));

#ifdef _WIN32
    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    struct timeval tv = { 3, 0 };
    int ready = select(0, &rfds, NULL, NULL, &tv);
#else
    struct pollfd pfd = { .fd = s, .events = POLLIN };
    int ready = poll(&pfd, 1, 3000);
#endif

    int ret = -1;
    if (ready > 0) {
        char buf[128];
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = '\0';
            strncpy(out_endpoint, buf, out_len - 1);
            out_endpoint[out_len - 1] = '\0';
            ret = 0;
        }
    }
    ft_close(s);
    return ret;
}

ft_sock_t ft_nat_punch_start(const char *peer_public_addr,
                              const char *server_host, int udp_port) {
    (void)server_host; (void)udp_port;
    struct sockaddr_in peer_sa;
    if (ft_parse_addr(peer_public_addr, &peer_sa) != 0) return FT_INVALID_SOCK;

    ft_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == FT_INVALID_SOCK) return FT_INVALID_SOCK;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char *)&opt, sizeof(opt));
#endif

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = 0;
    bind(sock, (struct sockaddr *)&la, sizeof(la));

    socklen_t alen = sizeof(la);
    getsockname(sock, (struct sockaddr *)&la, &alen);

    /* Send UDP punch packets from same local port */
    ft_sock_t udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp != FT_INVALID_SOCK) {
        setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(udp, SOL_SOCKET, SO_REUSEPORT, (const char *)&opt, sizeof(opt));
#endif
        struct sockaddr_in udp_la = la;
        bind(udp, (struct sockaddr *)&udp_la, sizeof(udp_la));
        for (int i = 0; i < 3; i++) {
            sendto(udp, "PUNCH", 5, 0, (struct sockaddr *)&peer_sa, sizeof(peer_sa));
#ifdef _WIN32
            Sleep(20);
#else
            usleep(20000);
#endif
        }
        ft_close(udp);
    }

    /* Start non-blocking TCP connect */
    ft_set_nonblocking(sock);
    connect(sock, (struct sockaddr *)&peer_sa, sizeof(peer_sa));
    return sock;
}

ft_sock_t ft_nat_punch(const char *peer_public_addr,
                       const char *server_host, int udp_port,
                       int timeout_ms) {
    (void)server_host; (void)udp_port;
    struct sockaddr_in peer_sa;
    if (ft_parse_addr(peer_public_addr, &peer_sa) != 0) return FT_INVALID_SOCK;

    ft_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == FT_INVALID_SOCK) return FT_INVALID_SOCK;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char *)&opt, sizeof(opt));
#endif

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = 0;
    bind(sock, (struct sockaddr *)&la, sizeof(la));

    socklen_t alen = sizeof(la);
    getsockname(sock, (struct sockaddr *)&la, &alen);

    /* Punch by sending UDP packets through the same source port. */
    ft_sock_t udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp != FT_INVALID_SOCK) {
        setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(udp, SOL_SOCKET, SO_REUSEPORT, (const char *)&opt, sizeof(opt));
#endif
        struct sockaddr_in udp_la = la;
        bind(udp, (struct sockaddr *)&udp_la, sizeof(udp_la));

        for (int i = 0; i < 5; i++) {
            sendto(udp, "PUNCH", 5, 0, (struct sockaddr *)&peer_sa, sizeof(peer_sa));
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50000);
#endif
        }
        ft_close(udp);
    }

    ft_set_nonblocking(sock);
    connect(sock, (struct sockaddr *)&peer_sa, sizeof(peer_sa));

#ifdef _WIN32
    fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (select(0, NULL, &wfds, NULL, &tv) > 0) {
        int err = 0; int elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
        if (err == 0) { ft_set_blocking(sock); ft_optimize_socket(sock); return sock; }
    }
#else
    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
        int err = 0; socklen_t elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err == 0) { ft_set_blocking(sock); ft_optimize_socket(sock); return sock; }
    }
#endif

    ft_close(sock);
    return FT_INVALID_SOCK;
}
