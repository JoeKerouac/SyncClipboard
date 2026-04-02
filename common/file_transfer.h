#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ft_sock_t;
#define FT_INVALID_SOCK INVALID_SOCKET
#else
typedef int ft_sock_t;
#define FT_INVALID_SOCK (-1)
#endif

#define FT_MAX_ADDRS  8
#define FT_MAGIC      "SYNC"
#define FT_MAGIC_LEN  4
#define FT_UUID_LEN   36

#define FT_MAX_FILE_SIZE (500ULL * 1024 * 1024)
void ft_set_max_file_size(uint64_t max_bytes);

typedef struct {
    char     file_id[64];
    char     file_name[256];
    char     mime_type[64];
    uint64_t file_size;
    char     checksum[65];
    uint8_t *data;
    size_t   data_len;
} FtFileInfo;

typedef struct {
    char addrs[FT_MAX_ADDRS][64];
    int  count;
} FtAddrList;

void        ft_close(ft_sock_t sock);
void        ft_set_nonblocking(ft_sock_t sock);
void        ft_set_blocking(ft_sock_t sock);
void        ft_generate_uuid(char *out);

int         ft_get_local_addresses(FtAddrList *list, int port);
ft_sock_t   ft_start_server(int *out_port);
ft_sock_t   ft_accept(ft_sock_t server_fd, int timeout_ms);

/* Try connecting to any address; returns first successful socket */
ft_sock_t   ft_connect_any(const char *addrs[], int count, int timeout_ms);

/* Bidirectional LAN: try connect to peer addrs + accept on listen_fd simultaneously.
   nat_sock may be a non-blocking socket already in connect (from ft_nat_punch_start),
   or FT_INVALID_SOCK to skip NAT. */
ft_sock_t   ft_lan_transfer(ft_sock_t listen_fd,
                            const char *peer_addrs[], int peer_count,
                            ft_sock_t nat_sock,
                            int timeout_ms);

/* Like ft_lan_transfer but with P2P auth integrated into the poll loop.
   Each candidate connection is authenticated in-line; wrong-peer / dead
   connections are discarded and the loop keeps trying.
   is_sender: 1 = ft_auth_send, 0 = ft_auth_recv. */
ft_sock_t   ft_lan_transfer_auth(ft_sock_t listen_fd,
                                 const char *peer_addrs[], int peer_count,
                                 ft_sock_t nat_sock,
                                 int timeout_ms,
                                 const char *file_id,
                                 const char *key_hex,
                                 int is_sender);

/* Discover public endpoint via server UDP reflector */
int         ft_discover_public(const char *server_host, int udp_port,
                               int local_port, char *out_endpoint, int out_len);

/* Start NAT hole punch: send UDP packets and begin non-blocking TCP connect.
   Returns a non-blocking socket mid-connect, to be polled by ft_lan_transfer. */
ft_sock_t   ft_nat_punch_start(const char *peer_public_addr,
                               const char *server_host, int udp_port);

/* NAT hole punch: UDP hole punch + TCP simultaneous open (blocking, legacy) */
ft_sock_t   ft_nat_punch(const char *peer_public_addr,
                         const char *server_host, int udp_port,
                         int timeout_ms);

/* Send/receive file over established TCP connection (in-memory) */
int         ft_send_file(ft_sock_t sock, const FtFileInfo *info);
int         ft_recv_file(ft_sock_t sock, FtFileInfo *info);

/* Streaming send/receive (avoids loading entire file into memory) */
int         ft_send_file_from_path(ft_sock_t sock, const char *file_id,
                                   uint64_t file_size, const char *path);
int         ft_recv_file_to_path(ft_sock_t sock, FtFileInfo *info,
                                 const char *output_path);

void        ft_sha256(const uint8_t *data, size_t len, char *out_hex);
int         ft_sha256_file(const char *path, char *out_hex);
char       *ft_base64_encode(const uint8_t *data, size_t len, size_t *out_len);
uint8_t    *ft_base64_decode(const char *b64, size_t b64_len, size_t *out_len);

int ft_auth_send(ft_sock_t sock, const char *file_id, const char *key_hex);
int ft_auth_recv(ft_sock_t sock, const char *file_id, const char *key_hex);

#endif
