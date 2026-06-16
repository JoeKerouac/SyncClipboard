#include "file_transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <net/if.h>
#include <sys/time.h>
#endif

#ifdef _WIN32
#include <mstcpip.h>
#else
#include <netinet/tcp.h>
#endif

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>

static uint64_t g_ft_max_size = FT_MAX_FILE_SIZE;

void ft_set_max_file_size(uint64_t max_bytes) {
    g_ft_max_size = max_bytes;
}

/* ---- helpers ---- */

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
        /* fallback: should not happen with OpenSSL properly initialized */
        for (int i = 0; i < 16; i++) buf[i] = (unsigned char)(rand() & 0xFF);
    }
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             buf[0],buf[1],buf[2],buf[3], buf[4],buf[5], buf[6],buf[7],
             buf[8],buf[9], buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
}

static int parse_addr(const char *s, struct sockaddr_in *sa) {
    char ip[64];
    strncpy(ip, s, sizeof(ip)-1); ip[63] = '\0';
    char *colon = strrchr(ip, ':');
    if (!colon) return -1;
    *colon = '\0';
    int port = atoi(colon+1);
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &sa->sin_addr) != 1) return -1;
    return 0;
}

static void ft_optimize_socket(ft_sock_t sock) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
    int bufsize = 512 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));
#ifdef _WIN32
    DWORD timeout = 30000;
#else
    struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
#endif
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
}

static int send_all(ft_sock_t sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, (const char*)(p+sent), (int)(len-sent), 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_all(ft_sock_t sock, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        int n = recv(sock, (char*)(p+got), (int)(len-got), 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* ---- local addresses ---- */

int ft_get_local_addresses(FtAddrList *list, int port) {
    list->count = 0;
#ifdef _WIN32
    ULONG blen = 15000;
    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(blen);
    if (!addrs) return 0;
    if (GetAdaptersAddresses(AF_INET, 0, NULL, addrs, &blen) == ERROR_BUFFER_OVERFLOW) {
        free(addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)malloc(blen);
        if (!addrs) return 0;
    }
    if (GetAdaptersAddresses(AF_INET, 0, NULL, addrs, &blen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a && list->count < FT_MAX_ADDRS; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS ua = a->FirstUnicastAddress;
                 ua && list->count < FT_MAX_ADDRS; ua = ua->Next) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ua->Address.lpSockaddr;
                if (sa->sin_family != AF_INET) continue;
                char ip[64];
                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                if (strcmp(ip, "127.0.0.1") == 0) continue;
                snprintf(list->addrs[list->count], 64, "%s:%d", ip, port);
                list->count++;
            }
        }
    }
    free(addrs);
#else
    struct ifaddrs *ifa_list, *ifa;
    if (getifaddrs(&ifa_list) == -1) return 0;
    for (ifa = ifa_list; ifa && list->count < FT_MAX_ADDRS; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[64];
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        snprintf(list->addrs[list->count], 64, "%s:%d", ip, port);
        list->count++;
    }
    freeifaddrs(ifa_list);
#endif
    return list->count;
}

/* ---- TCP server ---- */

ft_sock_t ft_start_server(int *out_port) {
    ft_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == FT_INVALID_SOCK) return FT_INVALID_SOCK;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { ft_close(fd); return FT_INVALID_SOCK; }

    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &alen);
    *out_port = ntohs(addr.sin_port);

    if (listen(fd, 5) < 0) { ft_close(fd); return FT_INVALID_SOCK; }
    return fd;
}

ft_sock_t ft_accept(ft_sock_t sfd, int timeout_ms) {
    ft_sock_t result = FT_INVALID_SOCK;
#ifdef _WIN32
    fd_set fds; FD_ZERO(&fds); FD_SET(sfd, &fds);
    struct timeval tv = { timeout_ms/1000, (timeout_ms%1000)*1000 };
    if (select(0, &fds, NULL, NULL, &tv) > 0) result = accept(sfd, NULL, NULL);
#else
    struct pollfd pfd = { .fd = sfd, .events = POLLIN };
    if (poll(&pfd, 1, timeout_ms) > 0) result = accept(sfd, NULL, NULL);
#endif
    if (result != FT_INVALID_SOCK) ft_optimize_socket(result);
    return result;
}

/* ---- connect any ---- */

ft_sock_t ft_connect_any(const char *addrs[], int count, int timeout_ms) {
    if (count <= 0) return FT_INVALID_SOCK;

    ft_sock_t *socks = (ft_sock_t*)calloc(count, sizeof(ft_sock_t));
    for (int i = 0; i < count; i++) socks[i] = FT_INVALID_SOCK;

    for (int i = 0; i < count; i++) {
        struct sockaddr_in sa;
        if (parse_addr(addrs[i], &sa) != 0) continue;
        ft_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == FT_INVALID_SOCK) continue;
        ft_set_nonblocking(s);
        connect(s, (struct sockaddr*)&sa, sizeof(sa));
        socks[i] = s;
    }

    ft_sock_t result = FT_INVALID_SOCK;
    int elapsed = 0, step = 50;
    while (elapsed < timeout_ms && result == FT_INVALID_SOCK) {
#ifdef _WIN32
        fd_set wfds; FD_ZERO(&wfds);
        for (int i = 0; i < count; i++)
            if (socks[i] != FT_INVALID_SOCK) FD_SET(socks[i], &wfds);
        struct timeval tv = {0, step*1000};
        select(0, NULL, &wfds, NULL, &tv);
        for (int i = 0; i < count && result == FT_INVALID_SOCK; i++) {
            if (socks[i] != FT_INVALID_SOCK && FD_ISSET(socks[i], &wfds)) {
                int err=0; int elen=sizeof(err);
                getsockopt(socks[i], SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
                if (err == 0) { result = socks[i]; socks[i] = FT_INVALID_SOCK; }
            }
        }
#else
        struct pollfd *pfds = calloc(count, sizeof(struct pollfd));
        int nfds = 0;
        int *idx_map = calloc(count, sizeof(int));
        for (int i = 0; i < count; i++) {
            if (socks[i] != FT_INVALID_SOCK) {
                pfds[nfds].fd = socks[i]; pfds[nfds].events = POLLOUT;
                idx_map[nfds] = i; nfds++;
            }
        }
        if (nfds == 0) { free(pfds); free(idx_map); break; }
        poll(pfds, nfds, step);
        for (int i = 0; i < nfds && result == FT_INVALID_SOCK; i++) {
            if (pfds[i].revents & POLLOUT) {
                int err=0; socklen_t elen=sizeof(err);
                getsockopt(pfds[i].fd, SOL_SOCKET, SO_ERROR, &err, &elen);
                if (err == 0) {
                    result = pfds[i].fd;
                    socks[idx_map[i]] = FT_INVALID_SOCK;
                }
            }
        }
        free(pfds); free(idx_map);
#endif
        elapsed += step;
    }

    for (int i = 0; i < count; i++)
        if (socks[i] != FT_INVALID_SOCK) ft_close(socks[i]);
    free(socks);

    if (result != FT_INVALID_SOCK) {
        ft_set_blocking(result);
        ft_optimize_socket(result);
    }
    return result;
}

/* ---- bidirectional LAN + optional NAT (all parallel in one poll loop) ---- */

ft_sock_t ft_lan_transfer(ft_sock_t listen_fd,
                          const char *peer_addrs[], int peer_count,
                          ft_sock_t nat_sock,
                          int timeout_ms) {
    ft_sock_t *conn_socks = NULL;
    if (peer_count > 0) {
        conn_socks = calloc(peer_count, sizeof(ft_sock_t));
        for (int i = 0; i < peer_count; i++) {
            struct sockaddr_in sa;
            if (parse_addr(peer_addrs[i], &sa) != 0) { conn_socks[i] = FT_INVALID_SOCK; continue; }
            ft_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == FT_INVALID_SOCK) { conn_socks[i] = FT_INVALID_SOCK; continue; }
            ft_set_nonblocking(s);
            connect(s, (struct sockaddr*)&sa, sizeof(sa));
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
        struct timeval tv = {0, step*1000};
        select(0, &rfds, &wfds, NULL, &tv);
        if (listen_fd != FT_INVALID_SOCK && FD_ISSET(listen_fd, &rfds)) {
            ft_sock_t c = accept(listen_fd, NULL, NULL);
            if (c != FT_INVALID_SOCK) result = c;
        }
        for (int i = 0; i < peer_count && result == FT_INVALID_SOCK; i++) {
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK && FD_ISSET(conn_socks[i], &wfds)) {
                int err=0; int elen=sizeof(err);
                getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
                if (err == 0) { result = conn_socks[i]; conn_socks[i] = FT_INVALID_SOCK; }
            }
        }
        if (result == FT_INVALID_SOCK && has_nat && nat_sock != FT_INVALID_SOCK && FD_ISSET(nat_sock, &wfds)) {
            int err=0; int elen=sizeof(err);
            getsockopt(nat_sock, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
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
                int err=0; socklen_t elen=sizeof(err);
                getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, &err, &elen);
                if (err == 0) { result = conn_socks[i]; conn_socks[i] = FT_INVALID_SOCK; }
            }
            idx++;
        }
        if (result == FT_INVALID_SOCK && nat_idx >= 0 && (pfds[nat_idx].revents & POLLOUT)) {
            int err=0; socklen_t elen=sizeof(err);
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

/* ---- helpers for ft_lan_transfer_auth ---- */

static long long ft_now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timeval _tv;
    gettimeofday(&_tv, NULL);
    return (long long)_tv.tv_sec * 1000 + _tv.tv_usec / 1000;
#endif
}

static void ft_set_sock_timeout(ft_sock_t sock, int ms) {
#ifdef _WIN32
    DWORD dw = (DWORD)ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dw, sizeof(dw));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&dw, sizeof(dw));
#else
    struct timeval _tv;
    _tv.tv_sec  = ms / 1000;
    _tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &_tv, sizeof(_tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &_tv, sizeof(_tv));
#endif
}

/*
 * Like ft_lan_transfer but with P2P authentication integrated into the poll
 * loop. Each candidate connection is authenticated before being accepted;
 * wrong-peer / dead connections are discarded and the loop keeps trying until
 * a successfully-authenticated socket is found or timeout_ms expires.
 *
 * is_sender: 1 = call ft_auth_send, 0 = call ft_auth_recv
 */
ft_sock_t ft_lan_transfer_auth(ft_sock_t listen_fd,
                                const char *peer_addrs[], int peer_count,
                                ft_sock_t nat_sock,
                                int timeout_ms,
                                const char *file_id,
                                const char *key_hex,
                                int is_sender)
{
    if (!file_id || !key_hex || !key_hex[0])
        return ft_lan_transfer(listen_fd, peer_addrs, peer_count, nat_sock, timeout_ms);

    ft_sock_t *conn_socks = NULL;
    if (peer_count > 0) {
        conn_socks = calloc(peer_count, sizeof(ft_sock_t));
        for (int i = 0; i < peer_count; i++) {
            struct sockaddr_in sa;
            if (parse_addr(peer_addrs[i], &sa) != 0) {
                printf("[AUTH-LOOP] parse_addr FAILED for peer[%d]='%s'\n", i, peer_addrs[i] ? peer_addrs[i] : "(null)");
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
        struct timeval tv = {0, poll_ms * 1000};
        select(0, &rfds, &wfds, &efds, &tv);

        if (listen_fd != FT_INVALID_SOCK && FD_ISSET(listen_fd, &rfds)) {
            ft_sock_t c = accept(listen_fd, NULL, NULL);
            if (c != FT_INVALID_SOCK) {
                printf("[AUTH-LOOP] Accepted incoming connection\n");
                ft_set_blocking(c);
                ft_optimize_socket(c);
                ft_set_sock_timeout(c, 2000);
                int ok = is_sender ? ft_auth_send(c, file_id, key_hex)
                                   : ft_auth_recv(c, file_id, key_hex);
                if (ok == 0) { printf("[AUTH-LOOP] Auth OK (accepted)\n"); ft_set_sock_timeout(c, 0); result = c; }
                else { printf("[AUTH-LOOP] Auth FAILED (accepted), closing\n"); ft_close(c); }
            }
        }
        for (int i = 0; i < peer_count && result == FT_INVALID_SOCK; i++) {
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK) {
                if (FD_ISSET(conn_socks[i], &efds)) {
                    int err = 0; int elen = sizeof(err);
                    getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
                    printf("[AUTH-LOOP] Outgoing connect FAILED to peer_addr[%d] (err=%d)\n", i, err);
                    ft_close(conn_socks[i]);
                    conn_socks[i] = FT_INVALID_SOCK;
                } else if (FD_ISSET(conn_socks[i], &wfds)) {
                    int err = 0; int elen = sizeof(err);
                    getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
                    if (err == 0) {
                        ft_sock_t c = conn_socks[i]; conn_socks[i] = FT_INVALID_SOCK;
                        printf("[AUTH-LOOP] Outgoing connect OK to peer_addr[%d] (fd=%llu)\n", i, (unsigned long long)c);
                        ft_set_blocking(c);
                        ft_optimize_socket(c);
                        ft_set_sock_timeout(c, 2000);
                        int ok = is_sender ? ft_auth_send(c, file_id, key_hex)
                                           : ft_auth_recv(c, file_id, key_hex);
                        if (ok == 0) { printf("[AUTH-LOOP] Auth OK (outgoing[%d])\n", i); ft_set_sock_timeout(c, 0); result = c; }
                        else { printf("[AUTH-LOOP] Auth FAILED (outgoing[%d]), closing\n", i); ft_close(c); }
                    } else {
                        printf("[AUTH-LOOP] Outgoing connect FAILED to peer_addr[%d] (err=%d)\n", i, err);
                        ft_close(conn_socks[i]);
                        conn_socks[i] = FT_INVALID_SOCK;
                    }
                }
            }
        }
        if (result == FT_INVALID_SOCK && has_nat && nat_sock != FT_INVALID_SOCK) {
            if (FD_ISSET(nat_sock, &efds)) {
                int err = 0; int elen = sizeof(err);
                getsockopt(nat_sock, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
                printf("[AUTH-LOOP] NAT connect FAILED (err=%d)\n", err);
                ft_close(nat_sock); nat_sock = FT_INVALID_SOCK;
            } else if (FD_ISSET(nat_sock, &wfds)) {
                int err = 0; int elen = sizeof(err);
                getsockopt(nat_sock, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
                if (err == 0) {
                    ft_set_blocking(nat_sock);
                    ft_optimize_socket(nat_sock);
                    ft_set_sock_timeout(nat_sock, 2000);
                    int ok = is_sender ? ft_auth_send(nat_sock, file_id, key_hex)
                                       : ft_auth_recv(nat_sock, file_id, key_hex);
                    if (ok == 0) { ft_set_sock_timeout(nat_sock, 0); result = nat_sock; nat_sock = FT_INVALID_SOCK; }
                    else { ft_close(nat_sock); nat_sock = FT_INVALID_SOCK; }
                } else {
                    printf("[AUTH-LOOP] NAT connect FAILED (err=%d)\n", err);
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
                if (c != FT_INVALID_SOCK) {
                    printf("[AUTH-LOOP] Accepted incoming connection (fd=%d)\n", c);
                    ft_set_blocking(c);
                    ft_optimize_socket(c);
                    ft_set_sock_timeout(c, 2000);
                    int ok = is_sender ? ft_auth_send(c, file_id, key_hex)
                                       : ft_auth_recv(c, file_id, key_hex);
                    if (ok == 0) { printf("[AUTH-LOOP] Auth OK (accepted)\n"); ft_set_sock_timeout(c, 0); result = c; }
                    else { printf("[AUTH-LOOP] Auth FAILED (accepted), closing\n"); ft_close(c); }
                }
            }
            idx++;
        }
        for (int i = 0; i < peer_count && result == FT_INVALID_SOCK; i++) {
            if (conn_socks && conn_socks[i] != FT_INVALID_SOCK &&
                (pfds[idx].revents & (POLLOUT|POLLERR|POLLHUP))) {
                int err = 0; socklen_t elen = sizeof(err);
                getsockopt(conn_socks[i], SOL_SOCKET, SO_ERROR, &err, &elen);
                if (err == 0) {
                    ft_sock_t c = conn_socks[i]; conn_socks[i] = FT_INVALID_SOCK;
                    printf("[AUTH-LOOP] Outgoing connect OK to peer_addr[%d] (fd=%d)\n", i, c);
                    ft_set_blocking(c);
                    ft_optimize_socket(c);
                    ft_set_sock_timeout(c, 2000);
                    int ok = is_sender ? ft_auth_send(c, file_id, key_hex)
                                       : ft_auth_recv(c, file_id, key_hex);
                    if (ok == 0) { printf("[AUTH-LOOP] Auth OK (outgoing[%d])\n", i); ft_set_sock_timeout(c, 0); result = c; }
                    else { printf("[AUTH-LOOP] Auth FAILED (outgoing[%d]), closing\n", i); ft_close(c); }
                } else {
                    printf("[AUTH-LOOP] Outgoing connect FAILED to peer_addr[%d] (err=%d)\n", i, err);
                    ft_close(conn_socks[i]);
                    conn_socks[i] = FT_INVALID_SOCK;
                }
            }
            idx++;
        }
        if (result == FT_INVALID_SOCK && nat_idx >= 0 &&
            (pfds[nat_idx].revents & (POLLOUT|POLLERR|POLLHUP))) {
            int err = 0; socklen_t elen = sizeof(err);
            getsockopt(nat_sock, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err == 0) {
                ft_set_blocking(nat_sock);
                ft_optimize_socket(nat_sock);
                ft_set_sock_timeout(nat_sock, 2000);
                int ok = is_sender ? ft_auth_send(nat_sock, file_id, key_hex)
                                   : ft_auth_recv(nat_sock, file_id, key_hex);
                if (ok == 0) { ft_set_sock_timeout(nat_sock, 0); result = nat_sock; nat_sock = FT_INVALID_SOCK; }
                else { ft_close(nat_sock); nat_sock = FT_INVALID_SOCK; }
            } else {
                printf("[AUTH-LOOP] NAT connect FAILED (err=%d)\n", err);
                ft_close(nat_sock); nat_sock = FT_INVALID_SOCK;
            }
        }
        free(pfds);
#endif
    }

    if (result == FT_INVALID_SOCK) {
        int elapsed = (int)(ft_now_ms() - t0);
        printf("[AUTH-LOOP] No connection established after %d ms (timeout=%d ms)\n", elapsed, timeout_ms);
    }

    if (conn_socks) {
        for (int i = 0; i < peer_count; i++)
            if (conn_socks[i] != FT_INVALID_SOCK) ft_close(conn_socks[i]);
        free(conn_socks);
    }
    if (has_nat && nat_sock != FT_INVALID_SOCK) ft_close(nat_sock);
    return result;
}

/* ---- UDP public endpoint discovery ---- */

int ft_discover_public(const char *server_host, int udp_port,
                       int local_port, char *out_endpoint, int out_len) {
    ft_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == FT_INVALID_SOCK) return -1;

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = htons(local_port);
    bind(s, (struct sockaddr*)&la, sizeof(la));

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
    sendto(s, msg, (int)strlen(msg), 0, (struct sockaddr*)&sa, sizeof(sa));

#ifdef _WIN32
    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    struct timeval tv = {3, 0};
    int ready = select(0, &rfds, NULL, NULL, &tv);
#else
    struct pollfd pfd = { .fd = s, .events = POLLIN };
    int ready = poll(&pfd, 1, 3000);
#endif

    int ret = -1;
    if (ready > 0) {
        char buf[128];
        int n = recvfrom(s, buf, sizeof(buf)-1, 0, NULL, NULL);
        if (n > 0) { buf[n]='\0'; strncpy(out_endpoint, buf, out_len-1); out_endpoint[out_len-1]='\0'; ret = 0; }
    }
    ft_close(s);
    return ret;
}

/* ---- NAT punch start: UDP punch + non-blocking TCP connect ---- */

ft_sock_t ft_nat_punch_start(const char *peer_public_addr,
                              const char *server_host, int udp_port) {
    struct sockaddr_in peer_sa;
    if (parse_addr(peer_public_addr, &peer_sa) != 0) return FT_INVALID_SOCK;

    ft_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == FT_INVALID_SOCK) return FT_INVALID_SOCK;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = 0;
    bind(sock, (struct sockaddr*)&la, sizeof(la));

    socklen_t alen = sizeof(la);
    getsockname(sock, (struct sockaddr*)&la, &alen);

    /* Send UDP punch packets from same local port */
    ft_sock_t udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp != FT_INVALID_SOCK) {
        setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(udp, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif
        struct sockaddr_in udp_la = la;
        bind(udp, (struct sockaddr*)&udp_la, sizeof(udp_la));
        for (int i = 0; i < 3; i++) {
            sendto(udp, "PUNCH", 5, 0, (struct sockaddr*)&peer_sa, sizeof(peer_sa));
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
    connect(sock, (struct sockaddr*)&peer_sa, sizeof(peer_sa));
    return sock;
}

/* ---- NAT hole punch (best-effort TCP simultaneous open) ---- */

ft_sock_t ft_nat_punch(const char *peer_public_addr,
                       const char *server_host, int udp_port,
                       int timeout_ms) {
    struct sockaddr_in peer_sa;
    if (parse_addr(peer_public_addr, &peer_sa) != 0) return FT_INVALID_SOCK;

    ft_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == FT_INVALID_SOCK) return FT_INVALID_SOCK;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = 0;
    bind(sock, (struct sockaddr*)&la, sizeof(la));

    socklen_t alen = sizeof(la);
    getsockname(sock, (struct sockaddr*)&la, &alen);
    int local_port = ntohs(la.sin_port);

    /* Send UDP packets through the same local port to punch NAT */
    ft_sock_t udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp != FT_INVALID_SOCK) {
        setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(udp, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif
        struct sockaddr_in udp_la = la;
        bind(udp, (struct sockaddr*)&udp_la, sizeof(udp_la));

        for (int i = 0; i < 5; i++) {
            sendto(udp, "PUNCH", 5, 0, (struct sockaddr*)&peer_sa, sizeof(peer_sa));
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50000);
#endif
        }
        ft_close(udp);
    }

    printf("[NAT] Trying TCP connect to %s from local port %d\n", peer_public_addr, local_port);
    ft_set_nonblocking(sock);
    connect(sock, (struct sockaddr*)&peer_sa, sizeof(peer_sa));

#ifdef _WIN32
    fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
    struct timeval tv = { timeout_ms/1000, (timeout_ms%1000)*1000 };
    if (select(0, NULL, &wfds, NULL, &tv) > 0) {
        int err=0; int elen=sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
        if (err == 0) { ft_set_blocking(sock); ft_optimize_socket(sock); return sock; }
    }
#else
    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
        int err=0; socklen_t elen=sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err == 0) { ft_set_blocking(sock); ft_optimize_socket(sock); return sock; }
    }
#endif

    ft_close(sock);
    return FT_INVALID_SOCK;
}

/* ---- file send / receive over TCP ---- */

int ft_send_file(ft_sock_t sock, const FtFileInfo *info) {
    if (send_all(sock, FT_MAGIC, FT_MAGIC_LEN) != 0) return -1;
    if (send_all(sock, info->file_id, FT_UUID_LEN) != 0) return -1;

    uint8_t sb[8]; uint64_t fs = info->file_size;
    for (int i = 7; i >= 0; i--) { sb[i] = fs & 0xFF; fs >>= 8; }
    if (send_all(sock, sb, 8) != 0) return -1;

    if (send_all(sock, info->data, info->data_len) != 0) return -1;
    return 0;
}

int ft_recv_file(ft_sock_t sock, FtFileInfo *info) {
    char magic[FT_MAGIC_LEN];
    if (recv_all(sock, magic, FT_MAGIC_LEN) != 0) return -1;
    if (memcmp(magic, FT_MAGIC, FT_MAGIC_LEN) != 0) return -1;

    char fid[FT_UUID_LEN+1];
    if (recv_all(sock, fid, FT_UUID_LEN) != 0) return -1;
    fid[FT_UUID_LEN] = '\0';
    strncpy(info->file_id, fid, sizeof(info->file_id)-1);

    uint8_t sb[8];
    if (recv_all(sock, sb, 8) != 0) return -1;
    uint64_t fs = 0;
    for (int i = 0; i < 8; i++) fs = (fs << 8) | sb[i];

    if (fs == 0 || fs > g_ft_max_size) {
        fprintf(stderr, "[FT] File size rejected: %llu bytes (max: %llu)\n",
                (unsigned long long)fs, (unsigned long long)g_ft_max_size);
        return -1;
    }

    info->file_size = fs;
    info->data_len  = (size_t)fs;

    info->data = (uint8_t*)malloc(info->data_len);
    if (!info->data) return -1;
    if (recv_all(sock, info->data, info->data_len) != 0) {
        free(info->data); info->data = NULL; return -1;
    }
    return 0;
}

/* ---- Streaming file send/receive ---- */

#define FT_CHUNK_SIZE (256 * 1024)

int ft_send_file_from_path(ft_sock_t sock, const char *file_id,
                           uint64_t file_size, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (send_all(sock, FT_MAGIC, FT_MAGIC_LEN) != 0) { fclose(f); return -1; }
    if (send_all(sock, file_id, FT_UUID_LEN) != 0) { fclose(f); return -1; }

    uint8_t sb[8]; uint64_t fs = file_size;
    for (int i = 7; i >= 0; i--) { sb[i] = fs & 0xFF; fs >>= 8; }
    if (send_all(sock, sb, 8) != 0) { fclose(f); return -1; }

    uint8_t *chunk = (uint8_t*)malloc(FT_CHUNK_SIZE);
    if (!chunk) { fclose(f); return -1; }

    uint64_t remaining = file_size;
    int ret = 0;
    while (remaining > 0) {
        size_t to_read = remaining > FT_CHUNK_SIZE ? FT_CHUNK_SIZE : (size_t)remaining;
        size_t n = fread(chunk, 1, to_read, f);
        if (n != to_read) { ret = -1; break; }
        if (send_all(sock, chunk, n) != 0) { ret = -1; break; }
        remaining -= n;
    }

    free(chunk);
    fclose(f);
    return ret;
}

int ft_recv_file_to_path(ft_sock_t sock, FtFileInfo *info,
                         const char *output_path) {
    char magic[FT_MAGIC_LEN];
    if (recv_all(sock, magic, FT_MAGIC_LEN) != 0) return -1;
    if (memcmp(magic, FT_MAGIC, FT_MAGIC_LEN) != 0) return -1;

    char fid[FT_UUID_LEN+1];
    if (recv_all(sock, fid, FT_UUID_LEN) != 0) return -1;
    fid[FT_UUID_LEN] = '\0';
    strncpy(info->file_id, fid, sizeof(info->file_id)-1);

    uint8_t sb[8];
    if (recv_all(sock, sb, 8) != 0) return -1;
    uint64_t fs = 0;
    for (int i = 0; i < 8; i++) fs = (fs << 8) | sb[i];

    if (fs == 0 || fs > g_ft_max_size) {
        fprintf(stderr, "[FT] File size rejected: %llu bytes (max: %llu)\n",
                (unsigned long long)fs, (unsigned long long)g_ft_max_size);
        return -1;
    }

    info->file_size = fs;
    info->data_len  = 0;
    info->data      = NULL;

    FILE *f = fopen(output_path, "wb");
    if (!f) return -1;

    uint8_t *chunk = (uint8_t*)malloc(FT_CHUNK_SIZE);
    if (!chunk) { fclose(f); return -1; }

    uint64_t remaining = fs;
    int ret = 0;
    while (remaining > 0) {
        size_t to_read = remaining > FT_CHUNK_SIZE ? FT_CHUNK_SIZE : (size_t)remaining;
        if (recv_all(sock, chunk, to_read) != 0) { ret = -1; break; }
        if (fwrite(chunk, 1, to_read, f) != to_read) { ret = -1; break; }
        remaining -= to_read;
    }

    free(chunk);
    fclose(f);
    if (ret != 0) remove(output_path);
    return ret;
}

/* ---- SHA-256 ---- */

void ft_sha256(const uint8_t *data, size_t len, char *out_hex) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(out_hex + i*2, "%02x", hash[i]);
    out_hex[SHA256_DIGEST_LENGTH*2] = '\0';
}

int ft_sha256_file(const char *path, char *out_hex) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    uint8_t buf[FT_CHUNK_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        EVP_DigestUpdate(ctx, buf, n);
    fclose(f);

    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned int hash_len;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(out_hex + i*2, "%02x", hash[i]);
    out_hex[SHA256_DIGEST_LENGTH*2] = '\0';
    return 0;
}

/* ---- Base64 ---- */

char *ft_base64_encode(const uint8_t *data, size_t len, size_t *out_len) {
    size_t b64_len = 4 * ((len+2)/3) + 1;
    char *b64 = (char*)malloc(b64_len);
    if (!b64) return NULL;
    int n = EVP_EncodeBlock((unsigned char*)b64, data, (int)len);
    b64[n] = '\0';
    if (out_len) *out_len = n;
    return b64;
}

uint8_t *ft_base64_decode(const char *b64, size_t b64_len, size_t *out_len) {
    size_t max_len = 3 * b64_len / 4 + 1;
    uint8_t *data = (uint8_t*)malloc(max_len);
    if (!data) return NULL;
    int n = EVP_DecodeBlock(data, (const unsigned char*)b64, (int)b64_len);
    if (n < 0) { free(data); return NULL; }
    if (b64_len > 1 && b64[b64_len-1] == '=') n--;
    if (b64_len > 2 && b64[b64_len-2] == '=') n--;
    if (out_len) *out_len = n;
    return data;
}

/* ---- P2P Authentication ---- */

static int ft_compute_hmac(const char *file_id, const char *key_hex,
                           unsigned char *out_hmac) {
    unsigned char key[32];
    memset(key, 0, sizeof(key));
    /* reuse hex_to_bytes logic */
    int klen = (int)strlen(key_hex) / 2;
    if (klen > 32) klen = 32;
    for (int i = 0; i < klen; i++) {
        unsigned int byte;
        sscanf(key_hex + 2 * i, "%02x", &byte);
        key[i] = (unsigned char)byte;
    }
    unsigned int hmac_len = 0;
    HMAC(EVP_sha256(), key, klen, (const unsigned char *)file_id,
         strlen(file_id), out_hmac, &hmac_len);
    return (hmac_len == 32) ? 0 : -1;
}

int ft_auth_send(ft_sock_t sock, const char *file_id, const char *key_hex) {
    unsigned char hmac[32];
    if (ft_compute_hmac(file_id, key_hex, hmac) != 0) return -1;
    char buf[36];
    memcpy(buf, "AUTH", 4);
    memcpy(buf + 4, hmac, 32);
    if (send_all(sock, buf, 36) != 0) return -1;
    char resp[4];
    if (recv_all(sock, resp, 4) != 0) return -1;
    return (memcmp(resp, "AKOK", 4) == 0) ? 0 : -1;
}

int ft_auth_recv(ft_sock_t sock, const char *file_id, const char *key_hex) {
    char buf[36];
    if (recv_all(sock, buf, 36) != 0) return -1;
    if (memcmp(buf, "AUTH", 4) != 0) {
        send_all(sock, "FAIL", 4);
        return -1;
    }
    unsigned char expected[32];
    if (ft_compute_hmac(file_id, key_hex, expected) != 0) {
        send_all(sock, "FAIL", 4);
        return -1;
    }
    if (memcmp(buf + 4, expected, 32) != 0) {
        send_all(sock, "FAIL", 4);
        return -1;
    }
    if (send_all(sock, "AKOK", 4) != 0) return -1;
    return 0;
}
