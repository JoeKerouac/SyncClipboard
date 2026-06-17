/*
 * Local interface enumeration, listening socket creation, and the
 * "race a list of candidate addresses" parallel connect helper.
 */

#include "ft_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

ft_sock_t ft_start_server(int *out_port) {
    ft_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == FT_INVALID_SOCK) return FT_INVALID_SOCK;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ft_close(fd); return FT_INVALID_SOCK;
    }

    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &alen);
    *out_port = ntohs(addr.sin_port);

    if (listen(fd, 5) < 0) { ft_close(fd); return FT_INVALID_SOCK; }
    return fd;
}

ft_sock_t ft_accept(ft_sock_t sfd, int timeout_ms) {
    ft_sock_t result = FT_INVALID_SOCK;
#ifdef _WIN32
    fd_set fds; FD_ZERO(&fds); FD_SET(sfd, &fds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (select(0, &fds, NULL, NULL, &tv) > 0) result = accept(sfd, NULL, NULL);
#else
    struct pollfd pfd = { .fd = sfd, .events = POLLIN };
    if (poll(&pfd, 1, timeout_ms) > 0) result = accept(sfd, NULL, NULL);
#endif
    if (result != FT_INVALID_SOCK) ft_optimize_socket(result);
    return result;
}

ft_sock_t ft_connect_any(const char *addrs[], int count, int timeout_ms) {
    if (count <= 0) return FT_INVALID_SOCK;

    ft_sock_t *socks = (ft_sock_t *)calloc(count, sizeof(ft_sock_t));
    for (int i = 0; i < count; i++) socks[i] = FT_INVALID_SOCK;

    for (int i = 0; i < count; i++) {
        struct sockaddr_in sa;
        if (ft_parse_addr(addrs[i], &sa) != 0) continue;
        ft_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == FT_INVALID_SOCK) continue;
        ft_set_nonblocking(s);
        connect(s, (struct sockaddr *)&sa, sizeof(sa));
        socks[i] = s;
    }

    ft_sock_t result = FT_INVALID_SOCK;
    int elapsed = 0, step = 50;
    while (elapsed < timeout_ms && result == FT_INVALID_SOCK) {
#ifdef _WIN32
        fd_set wfds; FD_ZERO(&wfds);
        for (int i = 0; i < count; i++)
            if (socks[i] != FT_INVALID_SOCK) FD_SET(socks[i], &wfds);
        struct timeval tv = { 0, step * 1000 };
        select(0, NULL, &wfds, NULL, &tv);
        for (int i = 0; i < count && result == FT_INVALID_SOCK; i++) {
            if (socks[i] != FT_INVALID_SOCK && FD_ISSET(socks[i], &wfds)) {
                int err = 0; int elen = sizeof(err);
                getsockopt(socks[i], SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
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
                int err = 0; socklen_t elen = sizeof(err);
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
