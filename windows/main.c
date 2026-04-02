#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libwebsockets.h>
#include "../common/config.h"
#include "../common/crypto.h"
#include "../common/cJSON.h"
#include "../common/file_transfer.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include <stdarg.h>

/* ---- Logging ---- */
#define LOG_LVL_DEBUG 0
#define LOG_LVL_INFO  1
#define LOG_LVL_WARN  2
#define LOG_LVL_ERROR 3

static FILE            *g_logfile = NULL;
static CRITICAL_SECTION g_log_cs;
static int              g_log_inited = 0;
static int              g_log_level = LOG_LVL_INFO;
static long             g_max_log_bytes = 10L * 1024 * 1024;
static char             g_log_path[MAX_PATH];
static char             g_log_path_old[MAX_PATH];
static int              g_log_to_console = 0;

static void log_init(void) {
    InitializeCriticalSection(&g_log_cs);
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *sep = strrchr(exe_dir, '\\');
    if (sep) *(sep + 1) = '\0'; else strcpy(exe_dir, ".\\");
    snprintf(g_log_path, MAX_PATH, "%ssync_clipboard.log", exe_dir);
    snprintf(g_log_path_old, MAX_PATH, "%ssync_clipboard.log.1", exe_dir);
    g_logfile = fopen(g_log_path, "a");
    if (g_logfile) setvbuf(g_logfile, NULL, _IONBF, 0);
    g_log_to_console = (GetConsoleWindow() != NULL);
    g_log_inited = 1;
}

static void log_rotate(void) {
    if (!g_logfile) return;
    fclose(g_logfile);
    remove(g_log_path_old);
    rename(g_log_path, g_log_path_old);
    g_logfile = fopen(g_log_path, "a");
    if (g_logfile) setvbuf(g_logfile, NULL, _IONBF, 0);
}

static void log_write(int level, const char *fmt, ...) {
    if (level < g_log_level || !g_log_inited) return;
    static const char *tags[] = {"DBG", "INF", "WRN", "ERR"};
    const char *tag = tags[level < 4 ? level : 3];
    EnterCriticalSection(&g_log_cs);
    SYSTEMTIME st;
    GetLocalTime(&st);
    va_list ap;
    if (g_logfile) {
        va_start(ap, fmt);
        fprintf(g_logfile, "%02d:%02d:%02d.%03d [%s] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag);
        vfprintf(g_logfile, fmt, ap);
        fprintf(g_logfile, "\n");
        va_end(ap);
        fflush(g_logfile);
        if (ftell(g_logfile) > g_max_log_bytes) log_rotate();
    }
    if (g_log_to_console) {
        va_start(ap, fmt);
        fprintf(stdout, "%02d:%02d:%02d.%03d [%s] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag);
        vfprintf(stdout, fmt, ap);
        fprintf(stdout, "\n");
        va_end(ap);
        fflush(stdout);
    }
    LeaveCriticalSection(&g_log_cs);
}

#define LOG_D(...) log_write(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_I(...) log_write(LOG_LVL_INFO,  __VA_ARGS__)
#define LOG_W(...) log_write(LOG_LVL_WARN,  __VA_ARGS__)
#define LOG_E(...) log_write(LOG_LVL_ERROR, __VA_ARGS__)

#define MAX_CLIPBOARD_SIZE (1024 * 1024)

static ClientConfig g_config;
static volatile int g_running = 1;
static volatile int g_authenticated = 0;
static volatile int g_logged_in = 0;
static volatile int g_connected = 0;
static volatile int g_suppress_next = 0;
static volatile int g_server_file_level = 3;

static char *g_recv_buf = NULL;
static size_t g_recv_len = 0;
static size_t g_recv_cap = 0;

typedef struct SendNode {
    char *msg;
    struct SendNode *next;
} SendNode;
static SendNode *g_send_head = NULL;
static SendNode *g_send_tail = NULL;
static CRITICAL_SECTION g_send_cs;

static struct lws *g_wsi = NULL;
static struct lws_context *g_context = NULL;
static HWND g_hwnd = NULL;
static HWND g_next_clipboard_viewer = NULL;
static char g_last_clip_hash[65] = {0};

/* ---- File transfer state ---- */
typedef struct {
    char     file_id[64];
    char     file_name[256];
    char     mime_type[64];
    uint64_t file_size;
    char     checksum[65];
    uint8_t *data;
    size_t   data_len;
    int      is_sender;
    ft_sock_t listen_fd;
    int      listen_port;
    char     peer_addrs[FT_MAX_ADDRS][64];
    int      peer_addr_count;
    char     peer_public_addr[64];
    char     from_device[128];
    long     max_relay_size;
    int      udp_port;
    int      eff_file_level;
    int      same_lan;
    volatile int phase;
    volatile int success;
} TransferState;

static TransferState g_xfer = {0};
static CRITICAL_SECTION g_xfer_cs;
static volatile LONG g_xfer_active_count = 0;
static int g_reconnect_delay = 1;

static int effective_file_level(void) {
    int s = g_server_file_level, c = g_config.file_transfer_level;
    return s < c ? s : c;
}

/* ---- stb_image helpers for PNG conversion ---- */
typedef struct { uint8_t *data; size_t len; size_t cap; } DynBuf;

static void dynbuf_write(void *ctx, void *data, int size) {
    DynBuf *b = (DynBuf *)ctx;
    if (b->len + size > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 4096;
        while (newcap < b->len + size) newcap *= 2;
        b->data = (uint8_t *)realloc(b->data, newcap);
        b->cap = newcap;
    }
    memcpy(b->data + b->len, data, size);
    b->len += size;
}

static uint8_t *bmp_to_png(const uint8_t *bmp_data, size_t bmp_len,
                           size_t *out_png_len) {
    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(bmp_data, (int)bmp_len, &w, &h, &channels, 4);
    if (!pixels) return NULL;
    DynBuf buf = {0};
    stbi_write_png_to_func(dynbuf_write, &buf, w, h, 4, pixels, w * 4);
    stbi_image_free(pixels);
    if (buf.len == 0) { free(buf.data); return NULL; }
    *out_png_len = buf.len;
    return buf.data;
}

static uint8_t *png_to_dib(const uint8_t *png_data, size_t png_len,
                           size_t *out_dib_len) {
    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(png_data, (int)png_len, &w, &h, &channels, 4);
    if (!pixels) return NULL;
    size_t row_bytes = (size_t)w * 4;
    size_t dib_len = sizeof(BITMAPINFOHEADER) + row_bytes * h;
    uint8_t *dib = (uint8_t *)malloc(dib_len);
    if (!dib) { stbi_image_free(pixels); return NULL; }
    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)dib;
    memset(bih, 0, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = w;
    bih->biHeight = h;  /* positive = bottom-up */
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)(row_bytes * h);
    /* Copy pixels: flip rows (top-down to bottom-up) and RGBA to BGRA */
    uint8_t *dst = dib + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        uint8_t *src_row = pixels + (size_t)(h - 1 - y) * row_bytes;
        uint8_t *dst_row = dst + (size_t)y * row_bytes;
        for (int x = 0; x < w; x++) {
            dst_row[x*4+0] = src_row[x*4+2]; /* B */
            dst_row[x*4+1] = src_row[x*4+1]; /* G */
            dst_row[x*4+2] = src_row[x*4+0]; /* R */
            dst_row[x*4+3] = src_row[x*4+3]; /* A */
        }
    }
    stbi_image_free(pixels);
    *out_dib_len = dib_len;
    return dib;
}

/* ---- Clipboard operations (Win32) ---- */

static char *clipboard_get(void) {
    if (!OpenClipboard(NULL)) return NULL;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) { CloseClipboard(); return NULL; }
    wchar_t *wtext = (wchar_t *)GlobalLock(hData);
    if (!wtext) { CloseClipboard(); return NULL; }
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, NULL, 0, NULL, NULL);
    char *utf8 = (char *)malloc(utf8_len);
    if (utf8) WideCharToMultiByte(CP_UTF8, 0, wtext, -1, utf8, utf8_len, NULL, NULL);
    GlobalUnlock(hData);
    CloseClipboard();
    return utf8;
}

static void clipboard_set(const char *text) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
    if (!hMem) return;
    wchar_t *wtext = (wchar_t *)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);
    GlobalUnlock(hMem);
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hMem);
        CloseClipboard();
    } else {
        GlobalFree(hMem);
    }
}

/* Check if clipboard has a bitmap */
static int clipboard_has_image(void) {
    return IsClipboardFormatAvailable(CF_DIB);
}

/* Read clipboard bitmap as raw BMP/DIB data (simple approach: store as BMP file) */
static uint8_t *clipboard_get_image(size_t *out_len) {
    if (!OpenClipboard(NULL)) return NULL;
    HANDLE hData = GetClipboardData(CF_DIB);
    if (!hData) { CloseClipboard(); return NULL; }

    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)GlobalLock(hData);
    if (!bih) { CloseClipboard(); return NULL; }

    DWORD dib_size = (DWORD)GlobalSize(hData);
    DWORD bfh_size = sizeof(BITMAPFILEHEADER);
    DWORD total = bfh_size + dib_size;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { GlobalUnlock(hData); CloseClipboard(); return NULL; }

    BITMAPFILEHEADER *bfh = (BITMAPFILEHEADER *)buf;
    memset(bfh, 0, sizeof(*bfh));
    bfh->bfType = 0x4D42; /* 'BM' */
    bfh->bfSize = total;
    bfh->bfOffBits = bfh_size + bih->biSize;
    if (bih->biBitCount <= 8) {
        bfh->bfOffBits += (1 << bih->biBitCount) * sizeof(RGBQUAD);
    } else if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) {
        bfh->bfOffBits += 3 * sizeof(DWORD);
    }

    memcpy(buf + bfh_size, bih, dib_size);
    GlobalUnlock(hData);
    CloseClipboard();

    *out_len = total;
    return buf;
}

/* Set clipboard to a BMP file (via CF_DIB) */
static void clipboard_set_image(const uint8_t *data, size_t len) {
    /* Try to detect if it's BMP (starts with 'BM') or raw image */
    const uint8_t *dib_data;
    size_t dib_len;

    /* Check for PNG format and convert to DIB */
    if (len > 4 && data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47) {
        uint8_t *converted = png_to_dib(data, len, &dib_len);
        if (!converted) {
            LOG_E("[CLIPBOARD] Failed to convert PNG to DIB");
            return;
        }
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dib_len);
        if (!hMem) { free(converted); return; }
        void *p = GlobalLock(hMem);
        memcpy(p, converted, dib_len);
        GlobalUnlock(hMem);
        free(converted);
        if (OpenClipboard(g_hwnd)) {
            EmptyClipboard();
            SetClipboardData(CF_DIB, hMem);
            CloseClipboard();
            LOG_I("[CLIPBOARD] PNG image set OK (%zu bytes DIB)", dib_len);
        } else {
            GlobalFree(hMem);
        }
        return;
    }

    /* Original BMP/DIB handling */
    if (len > 14 && data[0] == 'B' && data[1] == 'M') {
        BITMAPFILEHEADER *bfh = (BITMAPFILEHEADER *)data;
        dib_data = data + sizeof(BITMAPFILEHEADER);
        dib_len  = len - sizeof(BITMAPFILEHEADER);
    } else {
        dib_data = data;
        dib_len  = len;
    }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dib_len);
    if (!hMem) return;
    void *p = GlobalLock(hMem);
    memcpy(p, dib_data, dib_len);
    GlobalUnlock(hMem);

    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_DIB, hMem);
        CloseClipboard();
        LOG_I("[CLIPBOARD] Image set OK (%zu bytes)", dib_len);
    } else {
        GlobalFree(hMem);
    }
}

/* Set clipboard to CF_HDROP for a given file path (must already exist on disk) */
static void clipboard_set_file_path(const char *file_path) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, file_path, -1, NULL, 0);
    size_t drop_size = sizeof(DROPFILES) + wlen * sizeof(wchar_t) + sizeof(wchar_t);
    HGLOBAL hDrop = GlobalAlloc(GHND, drop_size);
    if (!hDrop) return;

    DROPFILES *df = (DROPFILES *)GlobalLock(hDrop);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    MultiByteToWideChar(CP_ACP, 0, file_path, -1,
                        (wchar_t *)((char *)df + sizeof(DROPFILES)), wlen);
    GlobalUnlock(hDrop);

    HGLOBAL hEffect = GlobalAlloc(GHND, sizeof(DWORD));
    if (hEffect) {
        DWORD *pEffect = (DWORD *)GlobalLock(hEffect);
        *pEffect = DROPEFFECT_COPY;
        GlobalUnlock(hEffect);
    }

    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_HDROP, hDrop);
        if (hEffect) {
            UINT cfEffect = RegisterClipboardFormatA("Preferred DropEffect");
            SetClipboardData(cfEffect, hEffect);
        }
        CloseClipboard();
        LOG_I("[CLIPBOARD] File set: %s", file_path);
    } else {
        GlobalFree(hDrop);
        if (hEffect) GlobalFree(hEffect);
    }
}

/* Save received file and set clipboard as CF_HDROP */
static void clipboard_set_file(const uint8_t *data, size_t len, const char *filename) {
    char tmp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_dir);
    char tmp_path[MAX_PATH];
    snprintf(tmp_path, MAX_PATH, "%ssyncclip_%s", tmp_dir, filename);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);

    clipboard_set_file_path(tmp_path);
}

/* ---- JSON builders ---- */

static char *build_auth_msg(const char *sk) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "auth");
    cJSON_AddStringToObject(j, "serverKey", sk);
    char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}

static char *build_login_msg(const char *u, const char *p, const char *d) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "login");
    cJSON_AddStringToObject(j, "username", u);
    cJSON_AddStringToObject(j, "password", p);
    cJSON_AddStringToObject(j, "deviceId", d);
    char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}

static char *build_clipboard_msg(const char *enc) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "clipboard");
    cJSON_AddStringToObject(j, "content", enc);
    char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}

static char *build_file_offer(const char *fid, const char *fn, const char *mt,
                              uint64_t fs, const char *ck, FtAddrList *addrs) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "file_offer");
    cJSON_AddStringToObject(j, "fileId", fid);
    cJSON_AddStringToObject(j, "fileName", fn);
    cJSON_AddStringToObject(j, "mimeType", mt);
    cJSON_AddNumberToObject(j, "fileSize", (double)fs);
    cJSON_AddStringToObject(j, "checksum", ck);
    cJSON *arr = cJSON_AddArrayToObject(j, "localAddresses");
    for (int i = 0; i < addrs->count; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(addrs->addrs[i]));
    char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}

static char *build_file_request(const char *fid, FtAddrList *addrs) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "file_request");
    cJSON_AddStringToObject(j, "fileId", fid);
    cJSON *arr = cJSON_AddArrayToObject(j, "localAddresses");
    for (int i = 0; i < addrs->count; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(addrs->addrs[i]));
    char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}

static char *build_file_relay_request(const char *fid) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "file_relay");
    cJSON_AddStringToObject(j, "fileId", fid);
    char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}

static char *build_file_relay_data(const char *fid, const uint8_t *data, size_t len,
                                   uint64_t fsize, const char *target) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "file_relay");
    cJSON_AddStringToObject(j, "fileId", fid);
    cJSON_AddNumberToObject(j, "fileSize", (double)fsize);
    cJSON_AddStringToObject(j, "targetDevice", target);
    size_t b64_len;
    char *b64 = ft_base64_encode(data, len, &b64_len);
    if (b64) { cJSON_AddStringToObject(j, "data", b64); free(b64); }
    char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}

static LARGE_INTEGER g_perf_freq = {0};

static long long win_now_ms(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (long long)(now.QuadPart * 1000 / g_perf_freq.QuadPart);
}

static char *build_transfer_result(const char *fid, const char *method, int success,
                                   long long conn_ms, long long xfer_ms) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "file_transfer_result");
    cJSON_AddStringToObject(j, "fileId", fid);
    cJSON_AddStringToObject(j, "method", method);
    cJSON_AddBoolToObject(j, "success", success);
    cJSON_AddNumberToObject(j, "connectionMs", (double)conn_ms);
    cJSON_AddNumberToObject(j, "transferMs", (double)xfer_ms);
    char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}

static void queue_send(const char *msg) {
    SendNode *node = (SendNode *)malloc(sizeof(SendNode));
    if (!node) return;
    node->msg = _strdup(msg);
    node->next = NULL;
    EnterCriticalSection(&g_send_cs);
    if (g_send_tail) {
        g_send_tail->next = node;
    } else {
        g_send_head = node;
    }
    g_send_tail = node;
    LeaveCriticalSection(&g_send_cs);
    if (g_wsi) lws_callback_on_writable(g_wsi);
    if (g_context) lws_cancel_service(g_context);
}

/* ---- P2P receiver thread ---- */

static DWORD WINAPI receiver_thread(LPVOID arg) {
    InterlockedIncrement(&g_xfer_active_count);
    TransferState *st = (TransferState *)arg;
    LOG_I("[P2P-RECV] fileId=%s, sameLan=%d", st->file_id, st->same_lan);

    const char *peer_ptrs[FT_MAX_ADDRS];
    for (int i = 0; i < st->peer_addr_count; i++) peer_ptrs[i] = st->peer_addrs[i];

    long long t0 = win_now_ms();
    const char *method = "failed";

    ft_sock_t nat_sock = FT_INVALID_SOCK;
    if (!st->same_lan && st->peer_public_addr[0] && st->eff_file_level >= 2) {
        LOG_D("[P2P-RECV] NAT punch start -> %s", st->peer_public_addr);
        nat_sock = ft_nat_punch_start(st->peer_public_addr, g_config.server_host, st->udp_port);
    }

    LOG_D("[P2P-RECV] LAN+NAT parallel (%d addrs, nat=%s, 5000ms)...",
          st->peer_addr_count, nat_sock != FT_INVALID_SOCK ? "yes" : "no");
    ft_sock_t sock = ft_lan_transfer_auth(st->listen_fd, peer_ptrs, st->peer_addr_count,
                                          nat_sock, 5000,
                                          st->file_id, g_config.aes_key, 0);
    if (sock != FT_INVALID_SOCK) method = "p2p";

    long long conn_ms = win_now_ms() - t0;

    if (sock != FT_INVALID_SOCK) {
        LOG_I("[P2P-RECV] Connected via %s (%lld ms)", method, conn_ms);
        long long t_xfer = win_now_ms();
        FtFileInfo ri = {0};
        if (ft_recv_file(sock, &ri) == 0) {
            long long xfer_ms = win_now_ms() - t_xfer;
            LOG_I("[P2P-RECV] Received %s (%llu bytes) in %lld ms",
                  st->file_name, (unsigned long long)ri.file_size, xfer_ms);
            g_suppress_next = 1;
            if (strstr(st->mime_type, "image/") == st->mime_type) {
                clipboard_set_image(ri.data, ri.data_len);
            } else {
                clipboard_set_file(ri.data, ri.data_len, st->file_name);
            }
            free(ri.data);
            st->success = 1;
            char *r = build_transfer_result(st->file_id, method, 1, conn_ms, xfer_ms);
            queue_send(r); free(r);
        }
        ft_close(sock);
    }

    if (!st->success && st->eff_file_level >= 3 && (long long)st->file_size <= st->max_relay_size) {
        LOG_I("[P2P-RECV] Requesting relay");
        char *req = build_file_relay_request(st->file_id); queue_send(req); free(req);
    } else if (!st->success) {
        char *r = build_transfer_result(st->file_id, "failed", 0, win_now_ms() - t0, 0);
        queue_send(r); free(r);
    }

    if (st->listen_fd != FT_INVALID_SOCK) ft_close(st->listen_fd);
    free(st);
    InterlockedDecrement(&g_xfer_active_count);
    return 0;
}

/* ---- P2P sender thread ---- */

static DWORD WINAPI sender_thread_func(LPVOID arg) {
    InterlockedIncrement(&g_xfer_active_count);
    TransferState *st = (TransferState *)arg;
    LOG_I("[P2P-SEND] fileId=%s, sameLan=%d", st->file_id, st->same_lan);

    const char *peer_ptrs[FT_MAX_ADDRS];
    for (int i = 0; i < st->peer_addr_count; i++) peer_ptrs[i] = st->peer_addrs[i];

    for (int i = 0; i < st->peer_addr_count; i++)
        LOG_I("[P2P-SEND]   peer_addr[%d] = %s", i, st->peer_addrs[i]);

    long long t0 = win_now_ms();
    int total_timeout = 5000;
    int sent_count = 0;

    ft_sock_t nat_sock = FT_INVALID_SOCK;
    if (!st->same_lan && st->peer_public_addr[0] && st->eff_file_level >= 2) {
        LOG_I("[P2P-SEND] NAT punch start -> %s", st->peer_public_addr);
        nat_sock = ft_nat_punch_start(st->peer_public_addr, g_config.server_host, st->udp_port);
    }

    LOG_I("[P2P-SEND] listen_fd=%llu, LAN+NAT parallel (%d peer addrs, nat=%s, %dms)...",
          (unsigned long long)st->listen_fd, st->peer_addr_count,
          nat_sock != FT_INVALID_SOCK ? "yes" : "no", total_timeout);
    ft_sock_t sock = ft_lan_transfer_auth(st->listen_fd, peer_ptrs, st->peer_addr_count,
                                          nat_sock, total_timeout,
                                          st->file_id, g_config.aes_key, 1);

    while (sock != FT_INVALID_SOCK) {
        long long conn_ms = win_now_ms() - t0;
        LOG_I("[P2P-SEND] Connected (%lld ms), sending %zu bytes...", conn_ms, st->data_len);
        long long t_xfer = win_now_ms();
        FtFileInfo fi = {0};
        strncpy(fi.file_id, st->file_id, sizeof(fi.file_id)-1);
        fi.file_size = st->file_size; fi.data = st->data; fi.data_len = st->data_len;
        if (ft_send_file(sock, &fi) == 0) {
            long long xfer_ms = win_now_ms() - t_xfer;
            sent_count++;
            LOG_I("[P2P-SEND] Sent #%d in %lld ms", sent_count, xfer_ms);
        } else {
            LOG_W("[P2P-SEND] Send failed after connect");
        }
        ft_close(sock);
        sock = FT_INVALID_SOCK;

        if (st->listen_fd == FT_INVALID_SOCK) break;
        int remaining = total_timeout - (int)(win_now_ms() - t0);
        if (remaining <= 0) break;
        sock = ft_lan_transfer_auth(st->listen_fd, NULL, 0, FT_INVALID_SOCK,
                                    remaining, st->file_id, g_config.aes_key, 1);
    }

    if (sent_count == 0) {
        long long conn_ms = win_now_ms() - t0;
        LOG_W("[P2P-SEND] No connection established after %lld ms, giving up", conn_ms);
    } else {
        st->success = 1;
    }

    free(st);
    InterlockedDecrement(&g_xfer_active_count);
    return 0;
}

/* ---- WebSocket callback ---- */

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    (void)user;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        LOG_I("[WS] Connected"); g_connected = 1; g_reconnect_delay = 1;
        { char *a = build_auth_msg(g_config.server_key); queue_send(a); free(a); }
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        size_t needed = g_recv_len + len;
        if (needed > g_recv_cap) {
            size_t new_cap = g_recv_cap ? g_recv_cap : 4096;
            while (new_cap < needed + 1) new_cap *= 2;
            char *tmp = (char *)realloc(g_recv_buf, new_cap);
            if (!tmp) { g_recv_len = 0; break; }
            g_recv_buf = tmp;
            g_recv_cap = new_cap;
        }
        memcpy(g_recv_buf + g_recv_len, in, len);
        g_recv_len += len;

        if (!lws_is_final_fragment(wsi)) break;

        g_recv_buf[g_recv_len] = '\0';
        char *data = g_recv_buf;
        size_t data_len = g_recv_len;
        g_recv_len = 0;

        cJSON *j = cJSON_Parse(data);
        if (!j) { break; }
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
        if (!type) { cJSON_Delete(j); break; }

        if (strcmp(type, "auth_result") == 0) {
            if (cJSON_IsTrue(cJSON_GetObjectItem(j, "success"))) {
                g_authenticated = 1;
                char *l = build_login_msg(g_config.username, g_config.password, g_config.device_id);
                queue_send(l); free(l);
            }
        } else if (strcmp(type, "login_result") == 0) {
            if (cJSON_IsTrue(cJSON_GetObjectItem(j, "success"))) {
                cJSON *ftl = cJSON_GetObjectItem(j, "fileTransferLevel");
                g_server_file_level = ftl ? (int)cJSON_GetNumberValue(ftl) : 3;
                LOG_I("[LOGIN] OK"); g_logged_in = 1;
            }
        } else if (strcmp(type, "clipboard") == 0) {
            const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(j, "content"));
            if (content) {
                size_t dl = 0;
                char *dec = aes_decrypt(content, g_config.aes_key, &dl);
                if (dec) {
                    g_suppress_next = 1;
                    ft_sha256((const uint8_t*)dec, strlen(dec), g_last_clip_hash);
                    clipboard_set(dec); free(dec);
                }
            }
        } else if (strcmp(type, "file_notify") == 0) {
            if (effective_file_level() <= 0) {
                LOG_W("[FILE] File transfer disabled, ignoring file_notify");
                cJSON_Delete(j); break;
            }
            EnterCriticalSection(&g_xfer_cs);
            if (g_xfer.data) { free(g_xfer.data); g_xfer.data = NULL; }
            if (g_xfer.listen_fd != FT_INVALID_SOCK) ft_close(g_xfer.listen_fd);
            memset(&g_xfer, 0, sizeof(g_xfer));
            g_xfer.listen_fd = FT_INVALID_SOCK;
            g_xfer.is_sender = 0;
            const char *fid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileId"));
            const char *fn  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileName"));
            const char *mt  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "mimeType"));
            const char *ck  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "checksum"));
            const char *fr  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "from"));
            if (fid) strncpy(g_xfer.file_id, fid, 63);
            if (fn)  strncpy(g_xfer.file_name, fn, 255);
            if (mt)  strncpy(g_xfer.mime_type, mt, 63);
            if (ck)  strncpy(g_xfer.checksum, ck, 64);
            if (fr)  strncpy(g_xfer.from_device, fr, 127);
            g_xfer.file_size = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "fileSize"));
            g_xfer.max_relay_size = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "maxRelaySize"));
            g_xfer.udp_port = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "udpPort"));
            int port = 0;
            g_xfer.listen_fd = ft_start_server(&port); g_xfer.listen_port = port;
            g_xfer.phase = 1;
            FtAddrList addrs = {0}; ft_get_local_addresses(&addrs, port);
            char *req = build_file_request(fid, &addrs); queue_send(req); free(req);
            LeaveCriticalSection(&g_xfer_cs);

        } else if (strcmp(type, "file_peer_info") == 0) {
            EnterCriticalSection(&g_xfer_cs);
            {
                const char *fid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileId"));
                if (fid && strcmp(fid, g_xfer.file_id) != 0) {
                    LOG_W("[FILE] Ignoring stale peer_info for fileId=%s (current=%s)", fid, g_xfer.file_id);
                    LeaveCriticalSection(&g_xfer_cs);
                    cJSON_Delete(j); break;
                }
            }
            if (!g_xfer.is_sender && (g_xfer_active_count > 0 || g_xfer.phase >= 2)) {
                LOG_D("[FILE] Ignoring duplicate peer_info (receiver transfer already active)");
                LeaveCriticalSection(&g_xfer_cs);
                cJSON_Delete(j); break;
            }
            if (g_xfer.is_sender && g_xfer_active_count > 0) {
                LOG_I("[FILE] Starting parallel sender for additional peer (active=%ld)", g_xfer_active_count);
            }
            const char *pub = cJSON_GetStringValue(cJSON_GetObjectItem(j, "peerPublicAddress"));
            if (pub) strncpy(g_xfer.peer_public_addr, pub, 63);
            cJSON *pa = cJSON_GetObjectItem(j, "peerLocalAddresses");
            g_xfer.peer_addr_count = 0;
            if (cJSON_IsArray(pa)) {
                cJSON *item; int idx = 0;
                cJSON_ArrayForEach(item, pa) {
                    if (idx >= FT_MAX_ADDRS) break;
                    const char *a = cJSON_GetStringValue(item);
                    if (a) strncpy(g_xfer.peer_addrs[idx], a, 63);
                    idx++;
                }
                g_xfer.peer_addr_count = idx;
            }
            {
                cJSON *ftl = cJSON_GetObjectItem(j, "fileTransferLevel");
                int srv_level = ftl ? (int)cJSON_GetNumberValue(ftl) : g_server_file_level;
                int eff = g_config.file_transfer_level < srv_level ? g_config.file_transfer_level : srv_level;
                g_xfer.eff_file_level = eff;
            }
            g_xfer.same_lan = cJSON_IsTrue(cJSON_GetObjectItem(j, "sameLan"));
            {
                const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(j, "role"));
                LOG_I("[FILE] peer_info role=%s, peer_public=%s, peer_local_count=%d, effLevel=%d, sameLan=%d",
                      role ? role : "?", g_xfer.peer_public_addr, g_xfer.peer_addr_count,
                      g_xfer.eff_file_level, g_xfer.same_lan);
            }
            g_xfer.phase = 2;
            TransferState *st_copy = (TransferState *)malloc(sizeof(TransferState));
            if (st_copy) {
                memcpy(st_copy, &g_xfer, sizeof(TransferState));
                if (!st_copy->is_sender)
                    g_xfer.listen_fd = FT_INVALID_SOCK;
                if (st_copy->is_sender)
                    CreateThread(NULL, 0, sender_thread_func, st_copy, 0, NULL);
                else
                    CreateThread(NULL, 0, receiver_thread, st_copy, 0, NULL);
            }
            LeaveCriticalSection(&g_xfer_cs);

        } else if (strcmp(type, "file_relay_request") == 0) {
            const char *req_id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "requesterId"));
            if (g_xfer.data && g_xfer.data_len > 0 && req_id) {
                char *rd = build_file_relay_data(g_xfer.file_id, g_xfer.data,
                                                 g_xfer.data_len, g_xfer.file_size, req_id);
                queue_send(rd); free(rd);
            }
        } else if (strcmp(type, "file_relay_data") == 0) {
            const char *fn  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileName"));
            const char *mt  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "mimeType"));
            const char *fid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileId"));
            const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(j, "data"));
            if (b64) {
                size_t dl; uint8_t *dec = ft_base64_decode(b64, strlen(b64), &dl);
                if (dec) {
                    g_suppress_next = 1;
                    if (mt && strstr(mt, "image/") == mt)
                        clipboard_set_image(dec, dl);
                    else
                        clipboard_set_file(dec, dl, fn ? fn : "file");
                    free(dec);
                    char *r = build_transfer_result(fid, "relay", 1, 0, 0); queue_send(r); free(r);
                }
            }
        }

        cJSON_Delete(j);
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        EnterCriticalSection(&g_send_cs);
        SendNode *node = g_send_head;
        if (node) {
            g_send_head = node->next;
            if (!g_send_head) g_send_tail = NULL;
        }
        int has_more = (g_send_head != NULL);
        LeaveCriticalSection(&g_send_cs);
        if (node) {
            size_t mlen = strlen(node->msg);
            unsigned char *buf = (unsigned char *)malloc(LWS_PRE + mlen);
            if (buf) {
                memcpy(buf + LWS_PRE, node->msg, mlen);
                lws_write(wsi, buf + LWS_PRE, mlen, LWS_WRITE_TEXT);
                free(buf);
            }
            free(node->msg);
            free(node);
        }
        if (has_more) lws_callback_on_writable(wsi);
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        g_connected=0; g_authenticated=0; g_logged_in=0; g_wsi=NULL;
        g_recv_len = 0;
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        g_connected=0; g_authenticated=0; g_logged_in=0; g_wsi=NULL;
        g_recv_len = 0;
        break;
    default: break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "sync-clipboard", ws_callback, 0, MAX_CLIPBOARD_SIZE },
    { NULL, NULL, 0, 0 }
};

/* ---- clipboard change handler ---- */

static char g_last_img_hash[65] = {0};
static char g_last_file_hash[65] = {0};

/* Get the first file path from CF_HDROP clipboard */
static char *clipboard_get_file_path(void) {
    if (!IsClipboardFormatAvailable(CF_HDROP)) return NULL;
    if (!OpenClipboard(NULL)) return NULL;
    HANDLE hDrop = GetClipboardData(CF_HDROP);
    if (!hDrop) { CloseClipboard(); return NULL; }
    UINT count = DragQueryFileA(hDrop, 0xFFFFFFFF, NULL, 0);
    if (count == 0) { CloseClipboard(); return NULL; }
    char path[MAX_PATH];
    if (DragQueryFileA(hDrop, 0, path, MAX_PATH) == 0) { CloseClipboard(); return NULL; }
    CloseClipboard();
    return _strdup(path);
}

static const char *win_mime_from_ext(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (_stricmp(ext, "png") == 0) return "image/png";
    if (_stricmp(ext, "jpg") == 0 || _stricmp(ext, "jpeg") == 0) return "image/jpeg";
    if (_stricmp(ext, "gif") == 0) return "image/gif";
    if (_stricmp(ext, "bmp") == 0) return "image/bmp";
    if (_stricmp(ext, "pdf") == 0) return "application/pdf";
    if (_stricmp(ext, "txt") == 0) return "text/plain";
    return "application/octet-stream";
}

static void on_clipboard_changed(void) {
    if (!g_logged_in) return;
    if (g_suppress_next) { g_suppress_next = 0; return; }

    /* Check image first */
    if (effective_file_level() > 0 && clipboard_has_image()) {
        size_t img_len = 0;
        uint8_t *img = clipboard_get_image(&img_len);
        if (img && img_len > 0) {
            char img_hash_buf[65];
            ft_sha256(img, img_len, img_hash_buf);
            if (strcmp(img_hash_buf, g_last_img_hash) != 0) {
                strcpy(g_last_img_hash, img_hash_buf);
                LOG_I("[MONITOR] Image clipboard (%zu bytes)", img_len);

                if (g_xfer_active_count > 0) {
                    LOG_W("[MONITOR] Waiting for previous transfer...");
                    for (int i = 0; i < 50 && g_xfer_active_count > 0; i++) Sleep(10);
                    if (g_xfer_active_count > 0) {
                        LOG_W("[MONITOR] Previous transfer still active, skipping");
                        free(img);
                        return;
                    }
                }

                EnterCriticalSection(&g_xfer_cs);
                if (g_xfer.data) free(g_xfer.data);
                if (g_xfer.listen_fd != FT_INVALID_SOCK) ft_close(g_xfer.listen_fd);
                memset(&g_xfer, 0, sizeof(g_xfer));
                g_xfer.listen_fd = FT_INVALID_SOCK;
                g_xfer.is_sender = 1;
                ft_generate_uuid(g_xfer.file_id);

                /* Convert BMP to PNG for cross-platform compatibility */
                size_t png_len = 0;
                uint8_t *png = bmp_to_png(img, img_len, &png_len);
                free(img);
                if (!png || png_len == 0) {
                    if (png) free(png);
                    LeaveCriticalSection(&g_xfer_cs);
                    return;
                }
                strncpy(g_xfer.file_name, "clipboard.png", sizeof(g_xfer.file_name)-1);
                strncpy(g_xfer.mime_type, "image/png", sizeof(g_xfer.mime_type)-1);
                g_xfer.file_size = png_len;
                g_xfer.data = png;
                g_xfer.data_len = png_len;
                ft_sha256(png, png_len, g_xfer.checksum);

                int port = 0;
                g_xfer.listen_fd = ft_start_server(&port); g_xfer.listen_port = port;
                g_xfer.phase = 1;
                FtAddrList addrs = {0}; ft_get_local_addresses(&addrs, port);
                char *offer = build_file_offer(g_xfer.file_id, g_xfer.file_name,
                                               g_xfer.mime_type, g_xfer.file_size,
                                               g_xfer.checksum, &addrs);
                queue_send(offer); free(offer);
                LeaveCriticalSection(&g_xfer_cs);
            } else {
                free(img);
            }
            return;
        }
        if (img) free(img);
    }

    /* Check file copy (CF_HDROP) */
    if (effective_file_level() > 0 && IsClipboardFormatAvailable(CF_HDROP)) {
        char *fpath = clipboard_get_file_path();
        if (fpath) {
            DWORD attr = GetFileAttributesA(fpath);
            if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                LOG_W("[MONITOR] Not a regular file: %s", fpath);
                free(fpath);
            } else {
                HANDLE hf = CreateFileA(fpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, 0, NULL);
                if (hf == INVALID_HANDLE_VALUE) {
                    LOG_W("[MONITOR] Cannot open file: %s", fpath);
                    free(fpath);
                } else {
                    LARGE_INTEGER li;
                    GetFileSizeEx(hf, &li);
                    long long fsize = li.QuadPart;
                    CloseHandle(hf);

                    if (fsize <= 0 || fsize >= 1024LL * 1024 * g_config.max_transfer_size) {
                        LOG_W("[MONITOR] File skipped (size=%lld): %s", fsize, fpath);
                        free(fpath);
                    } else {
                        const char *basename = strrchr(fpath, '\\');
                        if (!basename) basename = strrchr(fpath, '/');
                        basename = basename ? basename + 1 : fpath;

                        char fhash[65];
                        ft_sha256_file(fpath, fhash);
                        if (strcmp(fhash, g_last_file_hash) == 0) {
                            free(fpath);
                            return;
                        }
                        strcpy(g_last_file_hash, fhash);

                        LOG_I("[MONITOR] File clipboard: %s (%lld bytes)", basename, fsize);

                        uint8_t *fdata = (uint8_t *)malloc((size_t)fsize);
                        if (!fdata) { free(fpath); return; }
                        HANDLE hf2 = CreateFileA(fpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                                 OPEN_EXISTING, 0, NULL);
                        if (hf2 == INVALID_HANDLE_VALUE) { free(fdata); free(fpath); return; }
                        DWORD bytesRead = 0;
                        ReadFile(hf2, fdata, (DWORD)fsize, &bytesRead, NULL);
                        CloseHandle(hf2);
                        if (bytesRead != (DWORD)fsize) { free(fdata); free(fpath); return; }

                        if (g_xfer_active_count > 0) {
                            LOG_W("[MONITOR] Waiting for previous transfer...");
                            for (int i = 0; i < 50 && g_xfer_active_count > 0; i++) Sleep(10);
                            if (g_xfer_active_count > 0) {
                                LOG_W("[MONITOR] Previous transfer still active, skipping");
                                free(fdata);
                                free(fpath);
                                return;
                            }
                        }

                        EnterCriticalSection(&g_xfer_cs);
                        if (g_xfer.data) free(g_xfer.data);
                        if (g_xfer.listen_fd != FT_INVALID_SOCK) ft_close(g_xfer.listen_fd);
                        memset(&g_xfer, 0, sizeof(g_xfer));
                        g_xfer.listen_fd = FT_INVALID_SOCK;
                        g_xfer.is_sender = 1;
                        ft_generate_uuid(g_xfer.file_id);
                        strncpy(g_xfer.file_name, basename, sizeof(g_xfer.file_name)-1);
                        strncpy(g_xfer.mime_type, win_mime_from_ext(basename), sizeof(g_xfer.mime_type)-1);
                        g_xfer.file_size = fsize;
                        g_xfer.data = fdata;
                        g_xfer.data_len = (size_t)fsize;
                        strcpy(g_xfer.checksum, fhash);

                        int port = 0;
                        g_xfer.listen_fd = ft_start_server(&port);
                        g_xfer.listen_port = port;
                        g_xfer.phase = 1;
                        FtAddrList addrs = {0};
                        ft_get_local_addresses(&addrs, port);
                        char *offer = build_file_offer(g_xfer.file_id, g_xfer.file_name,
                                                       g_xfer.mime_type, g_xfer.file_size,
                                                       g_xfer.checksum, &addrs);
                        queue_send(offer); free(offer);
                        LeaveCriticalSection(&g_xfer_cs);
                        free(fpath);
                    }
                }
            }
            return;
        }
    }

    /* Text */
    char *clip = clipboard_get();
    if (!clip || strlen(clip) == 0) { free(clip); return; }
    char text_hash_buf[65];
    ft_sha256((const uint8_t*)clip, strlen(clip), text_hash_buf);
    if (strcmp(text_hash_buf, g_last_clip_hash) == 0) { free(clip); return; }
    strcpy(g_last_clip_hash, text_hash_buf);
    LOG_I("[MONITOR] Text clipboard (%zu bytes)", strlen(clip));
    char *enc = aes_encrypt(clip, strlen(clip), g_config.aes_key);
    if (enc) { char *m = build_clipboard_msg(enc); queue_send(m); free(m); free(enc); }
    free(clip);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CHANGECBCHAIN:
        if ((HWND)wParam == g_next_clipboard_viewer)
            g_next_clipboard_viewer = (HWND)lParam;
        else if (g_next_clipboard_viewer)
            SendMessage(g_next_clipboard_viewer, msg, wParam, lParam);
        return 0;
    case WM_DRAWCLIPBOARD:
        on_clipboard_changed();
        if (g_next_clipboard_viewer)
            SendMessage(g_next_clipboard_viewer, msg, wParam, lParam);
        return 0;
    case WM_DESTROY:
        ChangeClipboardChain(hwnd, g_next_clipboard_viewer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static const lws_retry_bo_t ws_retry = {
    .secs_since_valid_ping = 30,
    .secs_since_valid_hangup = 60,
};

static DWORD WINAPI ws_thread(LPVOID param) {
    (void)param;
    struct lws_context_creation_info ci;
    memset(&ci, 0, sizeof(ci));
    ci.port = CONTEXT_PORT_NO_LISTEN;
    ci.protocols = protocols;
    struct lws_context *ctx = lws_create_context(&ci);
    if (!ctx) { g_running = 0; return 1; }
    g_context = ctx;
    while (g_running) {
        if (!g_connected && !g_wsi) {
            struct lws_client_connect_info conn;
            memset(&conn, 0, sizeof(conn));
            conn.context = ctx; conn.address = g_config.server_host;
            conn.port = g_config.server_port; conn.path = g_config.server_path;
            conn.host = g_config.server_host; conn.origin = g_config.server_host;
            conn.protocol = protocols[0].name;
            conn.retry_and_idle_policy = &ws_retry;
            g_wsi = lws_client_connect_via_info(&conn);
            if (!g_wsi) {
                Sleep(g_reconnect_delay * 1000);
                if (g_reconnect_delay < 60) g_reconnect_delay *= 2;
                if (g_reconnect_delay > 60) g_reconnect_delay = 60;
                continue;
            }
        }
        lws_service(ctx, 250);
    }
    lws_context_destroy(ctx);
    return 0;
}

/* ---- Auto-start management (registry Run key) ---- */

static const char *REG_RUN_KEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char *REG_VALUE_NAME = "SyncClipboard";

static int install_autostart(void) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);

    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof(cmd), "\"%s\"", exe_path);

    HKEY hk;
    LONG rc = RegOpenKeyExA(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hk);
    if (rc != ERROR_SUCCESS) {
        LOG_E("Failed to open registry key (error %ld)", rc);
        return 1;
    }
    rc = RegSetValueExA(hk, REG_VALUE_NAME, 0, REG_SZ, (BYTE *)cmd, (DWORD)strlen(cmd) + 1);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS) {
        LOG_E("Failed to set registry value (error %ld)", rc);
        return 1;
    }
    LOG_I("Auto-start installed: %s", cmd);

    /* Launch immediately as detached process */
    STARTUPINFOA si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                       NULL, NULL, &si, &pi)) {
        LOG_I("Started (PID %lu)", pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

static void kill_other_instances(void) {
    DWORD my_pid = GetCurrentProcessId();
    char my_name[MAX_PATH];
    GetModuleFileNameA(NULL, my_name, MAX_PATH);
    const char *exe_name = strrchr(my_name, '\\');
    exe_name = exe_name ? exe_name + 1 : my_name;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe = { .dwSize = sizeof(pe) };
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID != my_pid &&
                _stricmp(pe.szExeFile, exe_name) == 0) {
                HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hp) {
                    TerminateProcess(hp, 0);
                    LOG_I("Terminated running instance (PID %lu)", pe.th32ProcessID);
                    CloseHandle(hp);
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
}

static int uninstall_autostart(void) {
    kill_other_instances();

    HKEY hk;
    LONG rc = RegOpenKeyExA(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hk);
    if (rc != ERROR_SUCCESS) {
        LOG_E("Failed to open registry key (error %ld)", rc);
        return 1;
    }
    rc = RegDeleteValueA(hk, REG_VALUE_NAME);
    RegCloseKey(hk);
    if (rc == ERROR_SUCCESS) {
        LOG_I("Auto-start removed and running instances stopped.");
        return 0;
    }
    LOG_E("Failed to remove registry value (error %ld)", rc);
    return 1;
}

/* Set working directory to the exe's directory so config.ini is found */
static void set_exe_directory(void) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) {
        *last_sep = '\0';
        SetCurrentDirectoryA(exe_path);
    }
}

static void log_apply_config(void) {
    if (g_config.log_level >= 0 && g_config.log_level <= 3)
        g_log_level = g_config.log_level;
    if (g_config.max_log_size_mb > 0)
        g_max_log_bytes = (long)g_config.max_log_size_mb * 1024L * 1024L;
}

int main(int argc, char **argv) {
    set_exe_directory();
    log_init();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--install") == 0) return install_autostart();
        if (strcmp(argv[i], "--uninstall") == 0) return uninstall_autostart();
    }

    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    QueryPerformanceFrequency(&g_perf_freq);
    const char *cfg = "config.ini";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { cfg = argv[i]; break; }
    }
    InitializeCriticalSection(&g_send_cs);
    InitializeCriticalSection(&g_xfer_cs);
    config_load(cfg, &g_config);
    ft_set_max_file_size((uint64_t)g_config.max_transfer_size * 1024ULL * 1024ULL);
    log_apply_config();

    LOG_I("=== SyncClipboard Windows Client ===");
    LOG_I("Server: %s:%d%s, User: %s, Device: %s",
          g_config.server_host, g_config.server_port, g_config.server_path,
          g_config.username, g_config.device_id);

    CreateThread(NULL, 0, ws_thread, NULL, 0, NULL);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "SyncClipboardWnd";
    RegisterClass(&wc);
    g_hwnd = CreateWindow("SyncClipboardWnd", "SyncClipboard", 0,
                          0,0,0,0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    g_next_clipboard_viewer = SetClipboardViewer(g_hwnd);

    MSG winmsg;
    while (g_running && GetMessage(&winmsg, NULL, 0, 0)) {
        TranslateMessage(&winmsg); DispatchMessage(&winmsg);
    }
    g_running = 0;
    DeleteCriticalSection(&g_send_cs);
    DeleteCriticalSection(&g_xfer_cs);
    if (g_xfer.data) free(g_xfer.data);
    if (g_logfile) fclose(g_logfile);
    DeleteCriticalSection(&g_log_cs);
    WSACleanup();
    return 0;
}
