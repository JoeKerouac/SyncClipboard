/*
 * Bidirectional LAN + optional NAT-punched candidate races.
 *
 * ft_lan_transfer       — basic version: returns whichever socket connects
 *                          first.
 * ft_lan_transfer_auth  — same selection algorithm but with HMAC-SHA256 P2P
 *                          authentication integrated into the poll loop.
 *                          Wrong-peer / unauthenticated sockets are dropped
 *                          and the loop keeps trying until a successfully
 *                          authenticated peer is found or timeout expires.
 */

#include "ft_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ft_sock_t ft_lan_transfer(ft_sock_t listen_fd,
                          const char *peer_addrs[], int peer_count,
                          ft_sock_t nat_sock,
                          int timeout_ms) {
    ft_sock_t *conn_socks = NULL;
    if (peer_count > 0) {
        conn_socks = calloc(peer_count, sizeof(ft_sock_t));
        for (int i = 0; i < peer_count; i++) {
            struct sockaddr_in sa;
            if (ft_parse_addr(peer_addrs[i], &sa) != 0) {
                conn_socks[i] = FT_INVALID_SOCK; continue;
            }
            ft_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == FT_INVALID_SOCK) { conn_socks[i] = FT_INVALID_SOCK; continue; }
            ft_set_nonblocking(s);
            connect(s, (struct sockaddr *)&sa, sizeof(sa));
            conn_socks[i] = s;
        }
    }

    if (listen_fd != FT_INVALID_SOCK) ft_set_nonblocking(listen_fd);
    int has_nat = (nat_sock != FT_INVALID_SOCK);

    ft_sock_t result = FT_INVALID_SOCK;
    int elapsed = 0, step = 50;

    while (elapsed < timeout_ms && result == FT_INVALID_SOCK) {
#ifdef _WIN32
        fd_set rfds, wfds; FD_ZERO(&rfds); FD_ZERO(&wfds);
        if (listen_fd != FT_INVALID_SOCK) FD_SET(listen_fd, &rfds);
        for (int i = 0; i < peer_count; i++)
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK) FD_SET(conn_socks[i], &wfds);
        if (has_nat && nat_sock != FT_INVALID_SOCK) FD_SET(nat_sock, &wfds);
        struct timeval tv = { 0, step * 1000 };
        select(0, &rfds, &wfds, NULL, &tv);
        if (listen_fd != FT_INVALID_SOCK && FD_ISSET(listen_fd, &rfds)) {
            ft_sock_t c = accept(listen_fd, NULL, NULL);
            if (c != FT_INVALID_SOCK) result = c;
        }
        for (int i = 0; i < peer_count && result == FT_INVALID_SOCK; i++) {
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK && FD_ISSET(conn_socks[i], &wfds)) {
                int err = 0; int elen = sizeof(err);
                getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
                if (err == 0) { result = conn_socks[i]; conn_socks[i] = FT_INVALID_SOCK; }
            }
        }
        if (result == FT_INVALID_SOCK && has_nat && nat_sock != FT_INVALID_SOCK && FD_ISSET(nat_sock, &wfds)) {
            int err = 0; int elen = sizeof(err);
            getsockopt(nat_sock, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
            if (err == 0) { result = nat_sock; nat_sock = FT_INVALID_SOCK; }
        }
#else
        int total = (listen_fd != FT_INVALID_SOCK ? 1 : 0) + peer_count + (has_nat ? 1 : 0);
        struct pollfd *pfds = calloc(total, sizeof(struct pollfd));
        int idx = 0;
        if (listen_fd != FT_INVALID_SOCK) {
            pfds[idx].fd = listen_fd; pfds[idx].events = POLLIN; idx++;
        }
        for (int i = 0; i < peer_count; i++) {
            pfds[idx].fd = (conn_socks && conn_socks[i] != FT_INVALID_SOCK) ? conn_socks[i] : -1;
            pfds[idx].events = POLLOUT;
            idx++;
        }
        int nat_idx = -1;
        if (has_nat && nat_sock != FT_INVALID_SOCK) {
            nat_idx = idx;
            pfds[idx].fd = nat_sock; pfds[idx].events = POLLOUT; idx++;
        }
        poll(pfds, idx, step);
        idx = 0;
        if (listen_fd != FT_INVALID_SOCK) {
            if (pfds[idx].revents & POLLIN) {
                ft_sock_t c = accept(listen_fd, NULL, NULL);
                if (c != FT_INVALID_SOCK) result = c;
            }
            idx++;
        }
        for (int i = 0; i < peer_count && result == FT_INVALID_SOCK; i++) {
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK && (pfds[idx].revents & POLLOUT)) {
                int err = 0; socklen_t elen = sizeof(err);
                getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, &err, &elen);
                if (err == 0) { result = conn_socks[i]; conn_socks[i] = FT_INVALID_SOCK; }
            }
            idx++;
        }
        if (result == FT_INVALID_SOCK && nat_idx >= 0 && (pfds[nat_idx].revents & POLLOUT)) {
            int err = 0; socklen_t elen = sizeof(err);
            getsockopt(nat_sock, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err == 0) { result = nat_sock; nat_sock = FT_INVALID_SOCK; }
        }
        free(pfds);
#endif
        elapsed += step;
    }

    if (conn_socks) {
        for (int i = 0; i < peer_count; i++)
            if (conn_socks[i] != FT_INVALID_SOCK) ft_close(conn_socks[i]);
        free(conn_socks);
    }
    if (has_nat && nat_sock != FT_INVALID_SOCK) ft_close(nat_sock);
    if (listen_fd != FT_INVALID_SOCK) ft_set_blocking(listen_fd);
    if (result != FT_INVALID_SOCK) {
        ft_set_blocking(result);
        ft_optimize_socket(result);
    }
    return result;
}

/* Promote a freshly-connected socket: blocking + optimised + 2s auth window.
 * Returns 0 on auth success and the caller takes ownership of `c`. */
static int try_authenticate(ft_sock_t c, const char *file_id, const char *key_hex, int is_sender) {
    ft_set_blocking(c);
    ft_optimize_socket(c);
    ft_set_sock_timeout(c, 2000);
    int ok = is_sender ? ft_auth_send(c, file_id, key_hex)
                       : ft_auth_recv(c, file_id, key_hex);
    if (ok == 0) {
        ft_set_sock_timeout(c, 0);
        return 0;
    }
    ft_close(c);
    return -1;
}

ft_sock_t ft_lan_transfer_auth(ft_sock_t listen_fd,
                                const char *peer_addrs[], int peer_count,
                                ft_sock_t nat_sock,
                                int timeout_ms,
                                const char *file_id,
                                const char *key_hex,
                                int is_sender) {
    if (!file_id || !key_hex || !key_hex[0])
        return ft_lan_transfer(listen_fd, peer_addrs, peer_count, nat_sock, timeout_ms);

    ft_sock_t *conn_socks = NULL;
    if (peer_count > 0) {
        conn_socks = calloc(peer_count, sizeof(ft_sock_t));
        for (int i = 0; i < peer_count; i++) {
            struct sockaddr_in sa;
            if (ft_parse_addr(peer_addrs[i], &sa) != 0) {
                conn_socks[i] = FT_INVALID_SOCK; continue;
            }
            ft_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == FT_INVALID_SOCK) { conn_socks[i] = FT_INVALID_SOCK; continue; }
            ft_set_nonblocking(s);
            connect(s, (struct sockaddr *)&sa, sizeof(sa));
            conn_socks[i] = s;
        }
    }

    if (listen_fd != FT_INVALID_SOCK) ft_set_nonblocking(listen_fd);
    int has_nat = (nat_sock != FT_INVALID_SOCK);

    ft_sock_t result = FT_INVALID_SOCK;
    long long t0 = ft_now_ms();
    int step = 50;

    while (result == FT_INVALID_SOCK) {
        int remaining = timeout_ms - (int)(ft_now_ms() - t0);
        if (remaining <= 0) break;
        int poll_ms = step < remaining ? step : remaining;

#ifdef _WIN32
        fd_set rfds, wfds, efds;
        FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
        if (listen_fd != FT_INVALID_SOCK) FD_SET(listen_fd, &rfds);
        for (int i = 0; i < peer_count; i++) {
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK) {
                FD_SET(conn_socks[i], &wfds);
                FD_SET(conn_socks[i], &efds);
            }
        }
        if (has_nat && nat_sock != FT_INVALID_SOCK) {
            FD_SET(nat_sock, &wfds);
            FD_SET(nat_sock, &efds);
        }
        struct timeval tv = { 0, poll_ms * 1000 };
        select(0, &rfds, &wfds, &efds, &tv);

        if (listen_fd != FT_INVALID_SOCK && FD_ISSET(listen_fd, &rfds)) {
            ft_sock_t c = accept(listen_fd, NULL, NULL);
            if (c != FT_INVALID_SOCK && try_authenticate(c, file_id, key_hex, is_sender) == 0)
                result = c;
        }
        for (int i = 0; i < peer_count && result == FT_INVALID_SOCK; i++) {
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK) {
                if (FD_ISSET(conn_socks[i], &efds)) {
                    ft_close(conn_socks[i]); conn_socks[i] = FT_INVALID_SOCK;
                } else if (FD_ISSET(conn_socks[i], &wfds)) {
                    int err = 0; int elen = sizeof(err);
                    getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
                    if (err == 0) {
                        ft_sock_t c = conn_socks[i]; conn_socks[i] = FT_INVALID_SOCK;
                        if (try_authenticate(c, file_id, key_hex, is_sender) == 0) result = c;
                    } else {
                        ft_close(conn_socks[i]); conn_socks[i] = FT_INVALID_SOCK;
                    }
                }
            }
        }
        if (result == FT_INVALID_SOCK && has_nat && nat_sock != FT_INVALID_SOCK) {
            if (FD_ISSET(nat_sock, &efds)) {
                ft_close(nat_sock); nat_sock = FT_INVALID_SOCK;
            } else if (FD_ISSET(nat_sock, &wfds)) {
                int err = 0; int elen = sizeof(err);
                getsockopt(nat_sock, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
                if (err == 0) {
                    ft_sock_t n = nat_sock; nat_sock = FT_INVALID_SOCK;
                    if (try_authenticate(n, file_id, key_hex, is_sender) == 0) result = n;
                } else {
                    ft_close(nat_sock); nat_sock = FT_INVALID_SOCK;
                }
            }
        }
#else
        int total = (listen_fd != FT_INVALID_SOCK ? 1 : 0) + peer_count + (has_nat ? 1 : 0);
        struct pollfd *pfds = calloc(total, sizeof(struct pollfd));
        int idx = 0;
        if (listen_fd != FT_INVALID_SOCK) {
            pfds[idx].fd = listen_fd; pfds[idx].events = POLLIN; idx++;
        }
        for (int i = 0; i < peer_count; i++) {
            pfds[idx].fd = (conn_socks && conn_socks[i] != FT_INVALID_SOCK) ? conn_socks[i] : -1;
            pfds[idx].events = POLLOUT;
            idx++;
        }
        int nat_idx = -1;
        if (has_nat && nat_sock != FT_INVALID_SOCK) {
            nat_idx = idx;
            pfds[idx].fd = nat_sock; pfds[idx].events = POLLOUT; idx++;
        }
        poll(pfds, idx, poll_ms);

        idx = 0;
        if (listen_fd != FT_INVALID_SOCK) {
            if (pfds[idx].revents & POLLIN) {
                ft_sock_t c = accept(listen_fd, NULL, NULL);
                if (c != FT_INVALID_SOCK && try_authenticate(c, file_id, key_hex, is_sender) == 0)
                    result = c;
            }
            idx++;
        }
        for (int i = 0; i < peer_count && result == FT_INVALID_SOCK; i++) {
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK &&
                (pfds[idx].revents & (POLLOUT | POLLERR | POLLHUP))) {
                int err = 0; socklen_t elen = sizeof(err);
                getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, &err, &elen);
                if (err == 0) {
                    ft_sock_t c = conn_socks[i]; conn_socks[i] = FT_INVALID_SOCK;
                    if (try_authenticate(c, file_id, key_hex, is_sender) == 0) result = c;
                } else {
                    ft_close(conn_socks[i]); conn_socks[i] = FT_INVALID_SOCK;
                }
            }
            idx++;
        }
        if (result == FT_INVALID_SOCK && nat_idx >= 0 &&
            (pfds[nat_idx].revents & (POLLOUT | POLLERR | POLLHUP))) {
            int err = 0; socklen_t elen = sizeof(err);
            getsockopt(nat_sock, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err == 0) {
                ft_sock_t n = nat_sock; nat_sock = FT_INVALID_SOCK;
                if (try_authenticate(n, file_id, key_hex, is_sender) == 0) result = n;
            } else {
                ft_close(nat_sock); nat_sock = FT_INVALID_SOCK;
            }
        }
        free(pfds);
#endif
    }

    if (conn_socks) {
        for (int i = 0; i < peer_count; i++)
            if (conn_socks[i] != FT_INVALID_SOCK) ft_close(conn_socks[i]);
        free(conn_socks);
    }
    if (has_nat && nat_sock != FT_INVALID_SOCK) ft_close(nat_sock);
    return result;
}
