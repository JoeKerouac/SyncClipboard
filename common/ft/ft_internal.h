#ifndef SYNC_CLIPBOARD_FT_INTERNAL_H
#define SYNC_CLIPBOARD_FT_INTERNAL_H

/*
 * Private helpers shared across the ft/ translation units. Not part of the
 * public file_transfer.h API. Anything declared here is implementation
 * detail of the file_transfer module.
 */

#include "../file_transfer.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <net/if.h>
#include <sys/time.h>
#endif

#include <stdint.h>
#include <stddef.h>

extern uint64_t g_ft_max_size;

/* Parse "host:port" into a sockaddr_in. Returns 0 on success. */
int  ft_parse_addr(const char *s, struct sockaddr_in *sa);

/* Set TCP_NODELAY + reasonable buffer sizes + send/recv timeouts. */
void ft_optimize_socket(ft_sock_t sock);

/* Apply a millisecond timeout (0 = blocking forever) to send/recv. */
void ft_set_sock_timeout(ft_sock_t sock, int ms);

/* Loop helpers. send_all returns 0 on success; recv_all expects exactly len. */
int  ft_send_all(ft_sock_t sock, const void *buf, size_t len);
int  ft_recv_all(ft_sock_t sock, void *buf, size_t len);

long long ft_now_ms(void);

#endif
