#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <libwebsockets.h>
#include "../common/config.h"
#include "../common/crypto.h"
#include "../common/cJSON.h"
#include "../common/file_transfer.h"
#include "../common/log.h"
#include "../common/auth_http.h"
#include "../common/ws_client.h"
#include "../common/msg.h"
#include "stb_image.h"
#include "stb_image_write.h"

#define MAX_CLIPBOARD_SIZE (1024 * 1024)
#define POLL_INTERVAL_MS   500

static ClientConfig g_config;
static volatile int g_running = 1;
static volatile int g_authenticated = 0;
static volatile int g_logged_in = 0;
static volatile int g_connected = 0;
static volatile int g_server_file_level = 3;

static int effective_file_level(void) {
    int s = g_server_file_level, c = g_config.file_transfer_level;
    return s < c ? s : c;
}

typedef struct SendNode {
    char *msg;
    struct SendNode *next;
} SendNode;
static SendNode *g_send_head = NULL;
static SendNode *g_send_tail = NULL;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_last_clip_hash[65] = {0};
static char g_last_img_hash[65] = {0};
static volatile int g_suppress_next = 0;  /* kept as extra safety layer */
static volatile long long g_suppress_until_ms = 0;

static char *g_recv_buf = NULL;
static size_t g_recv_len = 0;
static size_t g_recv_cap = 0;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void suppress_clipboard_ms(int ms) {
    g_suppress_until_ms = now_ms() + ms;
}

static int is_suppressed(void) {
    return now_ms() < g_suppress_until_ms;
}

static struct lws *g_wsi = NULL;
static struct lws_context *g_context = NULL;
static AuthTokens g_tokens;
static char g_ws_subproto[1200];
static int g_reconnect_delay = 1;

/* ---- File transfer state ---- */

typedef struct {
    char     file_id[64];
    char     file_name[256];
    char     mime_type[64];
    uint64_t file_size;
    char     checksum[65];
    uint8_t *data;
    size_t   data_len;
    char     source_path[512];
    int      is_sender;
    ft_sock_t listen_fd;
    int      listen_port;
    char     peer_addrs[FT_MAX_ADDRS][64];
    int      peer_addr_count;
    char     peer_public_addr[64];
    char     from_device[128];
    long     max_relay_size;
    int      udp_port;
    volatile int phase;   /* 0=idle, 1=waiting, 2=transferring, 3=done */
    volatile int success;
    int      eff_file_level;  /* effective min(server, client) for this transfer */
    int      same_lan;
} TransferState;

static TransferState g_xfer = {0};
static pthread_mutex_t g_xfer_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_xfer_active_count = 0;

static void signal_handler(int sig) { (void)sig; g_running = 0; }

/* ---- Display server detection ---- */

static int g_use_wayland = 0;
static Display *g_xdpy = NULL;    /* shared X11 display for clipboard ops */
static Window   g_xwin = 0;       /* invisible window for selection */
static Atom     g_clip_atom = 0;   /* CLIPBOARD atom */
static Atom     g_prop_atom = 0;   /* property atom for selection transfer */

static void detect_display_server(void) {
    const char *wl = getenv("WAYLAND_DISPLAY");
    if (wl && wl[0]) g_use_wayland = 1;
    LOG_INFO("[DISPLAY] %s session detected\n", g_use_wayland ? "Wayland" : "X11");
}

/* ---- Clipboard operations abstraction ---- */
typedef struct {
    char*    (*get_text)(void);
    void     (*set_text)(const char *text);
    void     (*refresh_types)(char *buf, size_t buf_size);
    uint8_t* (*read_type)(const char *mime, size_t *out_len);
    void     (*set_image)(const uint8_t *data, size_t len, const char *mime);
    void     (*set_file)(const char *filepath);
} ClipboardOps;

static ClipboardOps g_clip_ops;

/* ---- Clipboard operations (xclip — fallback and write backend) ---- */

static char *xclip_clipboard_get_text(void) {
    FILE *fp = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!fp) return NULL;
    char *buf = (char*)malloc(MAX_CLIPBOARD_SIZE);
    if (!buf) { pclose(fp); return NULL; }
    size_t total = 0, n;
    while ((n = fread(buf+total, 1, MAX_CLIPBOARD_SIZE-total-1, fp)) > 0) total += n;
    pclose(fp);
    buf[total] = '\0';
    if (total == 0) { free(buf); return NULL; }
    return buf;
}

static void xclip_clipboard_set_text(const char *text) {
    size_t len = strlen(text);
    FILE *fp = popen("xclip -selection clipboard -i 2>/dev/null", "w");
    if (fp) { fwrite(text, 1, len, fp); pclose(fp); }
    /* Also set PRIMARY so Shift+Insert works in terminals */
    fp = popen("xclip -selection primary -i 2>/dev/null", "w");
    if (fp) { fwrite(text, 1, len, fp); pclose(fp); }
    LOG_INFO("[CLIPBOARD] Text set OK (%zu bytes)\n", len);
}

static char g_cached_types[4096] = {0};

static void xclip_clipboard_refresh_types(char *buf, size_t buf_size) {
    FILE *fp = popen("xclip -selection clipboard -t TARGETS -o 2>/dev/null", "r");
    if (!fp) { buf[0] = '\0'; return; }
    size_t n = fread(buf, 1, buf_size - 1, fp); pclose(fp);
    buf[n] = '\0';
}

static int clipboard_has_image(void) {
    return (strstr(g_cached_types, "image/png") != NULL ||
            strstr(g_cached_types, "image/jpeg") != NULL);
}

static int clipboard_has_file(void) {
    int has_uri   = (strstr(g_cached_types, "text/uri-list") != NULL);
    int has_gnome = (strstr(g_cached_types, "x-special/gnome-copied-files") != NULL);
    return has_uri || has_gnome;
}

static int is_safe_mime(const char *mime) {
    const char *allowed[] = {"image/png", "image/jpeg", "image/bmp", "image/gif",
                             "image/webp", "text/uri-list", "x-special/gnome-copied-files",
                             "TARGETS", NULL};
    for (int i = 0; allowed[i]; i++)
        if (strcmp(mime, allowed[i]) == 0) return 1;
    return 0;
}

static uint8_t *xclip_clipboard_read_type(const char *mime, size_t *out_len) {
    if (!is_safe_mime(mime)) return NULL;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xclip -selection clipboard -t %s -o 2>/dev/null", mime);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 1024*1024, total = 0;
    uint8_t *buf = (uint8_t*)malloc(cap);
    if (!buf) { pclose(fp); return NULL; }
    size_t n;
    while ((n = fread(buf+total, 1, cap-total, fp)) > 0) {
        total += n;
        if (total >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) { pclose(fp); return NULL; } }
    }
    pclose(fp);
    if (total == 0) { free(buf); return NULL; }
    *out_len = total;
    return buf;
}

static uint8_t *clipboard_get_image(size_t *out_len, char *mime_out) {
    uint8_t *data = g_clip_ops.read_type("image/png", out_len);
    if (data) { if (mime_out) strcpy(mime_out, "image/png"); return data; }
    data = g_clip_ops.read_type("image/jpeg", out_len);
    if (data) { if (mime_out) strcpy(mime_out, "image/jpeg"); return data; }
    return NULL;
}

static void url_decode_inplace(char *s) {
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            unsigned int byte;
            if (sscanf(src + 1, "%2x", &byte) == 1) {
                *dst++ = (char)byte;
                src += 3;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static char *clipboard_get_file_uri(void) {
    size_t len = 0;

    /* Try text/uri-list first */
    uint8_t *data = g_clip_ops.read_type("text/uri-list", &len);
    if (data) {
        char *s = (char*)data;
        char *nl = strchr(s, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(s, '\r'); if (cr) *cr = '\0';
        if (strncmp(s, "file://", 7) == 0) {
            char *path = strdup(s + 7);
            free(data);
            url_decode_inplace(path);
            return path;
        }
        free(data);
    }

    /* Try x-special/gnome-copied-files: format is "copy\nfile:///path\n" */
    data = g_clip_ops.read_type("x-special/gnome-copied-files", &len);
    if (data) {
        char *s = (char*)data;
        char *nl = strchr(s, '\n');
        if (nl) {
            nl++;
            char *end = strchr(nl, '\n'); if (end) *end = '\0';
            char *cr = strchr(nl, '\r');  if (cr) *cr = '\0';
            if (strncmp(nl, "file://", 7) == 0) {
                char *path = strdup(nl + 7);
                free(data);
                url_decode_inplace(path);
                return path;
            }
        }
        free(data);
    }
    return NULL;
}

static const char *mime_from_extension(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "webp") == 0) return "image/webp";
    if (strcasecmp(ext, "bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, "pdf") == 0) return "application/pdf";
    if (strcasecmp(ext, "txt") == 0) return "text/plain";
    return "application/octet-stream";
}

/* ---- stb_image helpers for BMP-to-PNG conversion ---- */
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

static uint8_t *bmp_to_png_buf(const uint8_t *bmp_data, size_t bmp_len,
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

/* ---- Wayland clipboard operations (wl-clipboard) ---- */

static char *wl_clipboard_get_text(void) {
    FILE *fp = popen("wl-paste --no-newline 2>/dev/null", "r");
    if (!fp) return NULL;
    char *buf = (char*)malloc(MAX_CLIPBOARD_SIZE);
    if (!buf) { pclose(fp); return NULL; }
    size_t total = 0, n;
    while ((n = fread(buf+total, 1, MAX_CLIPBOARD_SIZE-total-1, fp)) > 0) total += n;
    pclose(fp);
    buf[total] = '\0';
    if (total == 0) { free(buf); return NULL; }
    return buf;
}

static void wl_clipboard_set_text(const char *text) {
    size_t len = strlen(text);
    FILE *fp = popen("wl-copy 2>/dev/null", "w");
    if (fp) { fwrite(text, 1, len, fp); pclose(fp); }
    LOG_INFO("[CLIPBOARD] Text set OK (%zu bytes, wl-copy)\n", len);
}

static void wl_clipboard_refresh_types(char *buf, size_t buf_size) {
    FILE *fp = popen("wl-paste --list-types 2>/dev/null", "r");
    if (!fp) { buf[0] = '\0'; return; }
    size_t n = fread(buf, 1, buf_size - 1, fp);
    pclose(fp);
    buf[n] = '\0';
}

static uint8_t *wl_clipboard_read_type(const char *mime, size_t *out_len) {
    if (!is_safe_mime(mime)) return NULL;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wl-paste --type %s 2>/dev/null", mime);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 1024*1024, total = 0;
    uint8_t *buf = (uint8_t*)malloc(cap);
    if (!buf) { pclose(fp); return NULL; }
    size_t n;
    while ((n = fread(buf+total, 1, cap-total, fp)) > 0) {
        total += n;
        if (total >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) { pclose(fp); return NULL; } }
    }
    pclose(fp);
    if (total == 0) { free(buf); return NULL; }
    *out_len = total;
    return buf;
}

static void wl_clipboard_set_image(const uint8_t *data, size_t len, const char *mime) {
    if (!mime || !mime[0]) mime = "image/png";
    /* If BMP data, convert to PNG */
    uint8_t *converted = NULL;
    if (strcmp(mime, "image/bmp") == 0 && len > 2 && data[0] == 'B' && data[1] == 'M') {
        size_t png_len = 0;
        converted = bmp_to_png_buf(data, len, &png_len);
        if (converted) { data = converted; len = png_len; mime = "image/png"; }
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wl-copy --type %s 2>/dev/null", mime);
    FILE *fp = popen(cmd, "w");
    if (fp) { fwrite(data, 1, len, fp); pclose(fp); }
    LOG_INFO("[CLIPBOARD] Image set OK (%zu bytes, %s, wl-copy)\n", len, mime);
    free(converted);
}

static void wl_clipboard_set_file(const char *filepath) {
    FILE *fp = popen("wl-copy --type x-special/gnome-copied-files 2>/dev/null", "w");
    if (!fp) return;
    fprintf(fp, "copy\n%s", filepath);
    pclose(fp);
    LOG_INFO("[CLIPBOARD] File set OK (%s, wl-copy)\n", filepath);
}

/* ---- X11 Selection API clipboard reads ---- */

static uint8_t *x11_read_selection_ex(Atom selection, Atom target, size_t *out_len) {
    if (!g_xdpy || !g_xwin) return NULL;

    XConvertSelection(g_xdpy, selection, target, g_prop_atom, g_xwin, CurrentTime);
    XFlush(g_xdpy);

    /* Wait for SelectionNotify event with timeout */
    XEvent ev;
    int found = 0;
    for (int i = 0; i < 100 && !found; i++) {  /* 1 second timeout */
        if (XPending(g_xdpy)) {
            XNextEvent(g_xdpy, &ev);
            if (ev.type == SelectionNotify) found = 1;
            /* Ignore other events during this wait */
        } else {
            usleep(10000);
        }
    }

    if (!found || ev.xselection.property == None) return NULL;

    /* Check for INCR (incremental) transfer */
    Atom actual_type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *prop_data = NULL;

    XGetWindowProperty(g_xdpy, g_xwin, g_prop_atom, 0, 0, False,
                       AnyPropertyType, &actual_type, &format,
                       &nitems, &bytes_after, &prop_data);
    if (prop_data) XFree(prop_data);

    Atom incr_atom = XInternAtom(g_xdpy, "INCR", False);
    if (actual_type == incr_atom) {
        /* INCR transfer for large data: fall back to reading full property */
        XDeleteProperty(g_xdpy, g_xwin, g_prop_atom);
        XFlush(g_xdpy);

        size_t total = 0, cap = 65536;
        uint8_t *buf = (uint8_t *)malloc(cap);
        if (!buf) return NULL;

        while (1) {
            /* Wait for PropertyNotify */
            int got_prop = 0;
            for (int i = 0; i < 200 && !got_prop; i++) {
                if (XPending(g_xdpy)) {
                    XNextEvent(g_xdpy, &ev);
                    if (ev.type == PropertyNotify && ev.xproperty.atom == g_prop_atom
                        && ev.xproperty.state == PropertyNewValue) got_prop = 1;
                } else {
                    usleep(10000);
                }
            }
            if (!got_prop) break;

            prop_data = NULL;
            XGetWindowProperty(g_xdpy, g_xwin, g_prop_atom, 0, ~0L, True,
                               AnyPropertyType, &actual_type, &format,
                               &nitems, &bytes_after, &prop_data);
            if (!prop_data || nitems == 0) {
                if (prop_data) XFree(prop_data);
                break;  /* End of INCR transfer */
            }
            size_t chunk_size = nitems * (format == 32 ? sizeof(long) : (format == 16 ? 2 : 1));
            if (total + chunk_size > cap) {
                while (cap < total + chunk_size) cap *= 2;
                buf = (uint8_t *)realloc(buf, cap);
            }
            memcpy(buf + total, prop_data, chunk_size);
            total += chunk_size;
            XFree(prop_data);
        }

        if (total == 0) { free(buf); return NULL; }
        *out_len = total;
        return buf;
    }

    /* Normal (non-INCR) transfer */
    prop_data = NULL;
    XGetWindowProperty(g_xdpy, g_xwin, g_prop_atom, 0, ~0L, True,
                       AnyPropertyType, &actual_type, &format,
                       &nitems, &bytes_after, &prop_data);

    if (!prop_data || nitems == 0) {
        if (prop_data) XFree(prop_data);
        return NULL;
    }

    size_t data_size = nitems * (format == 32 ? sizeof(long) : (format == 16 ? 2 : 1));
    uint8_t *result = (uint8_t *)malloc(data_size);
    if (result) {
        memcpy(result, prop_data, data_size);
        *out_len = data_size;
    }
    XFree(prop_data);
    return result;
}

static uint8_t *x11_read_selection(Atom target, size_t *out_len) {
    return x11_read_selection_ex(g_clip_atom, target, out_len);
}

static char *x11_clipboard_get_text(void) {
    if (!g_xdpy) return NULL;
    Atom utf8 = XInternAtom(g_xdpy, "UTF8_STRING", False);
    size_t len = 0;
    uint8_t *data = x11_read_selection(utf8, &len);
    if (!data) {
        /* Fallback to STRING */
        data = x11_read_selection(XA_STRING, &len);
    }
    if (!data || len == 0) { free(data); return NULL; }
    /* Ensure null termination */
    char *text = (char *)realloc(data, len + 1);
    if (!text) { free(data); return NULL; }
    text[len] = '\0';
    return text;
}

static void x11_clipboard_refresh_types(char *buf, size_t buf_size) {
    if (!g_xdpy) { buf[0] = '\0'; return; }
    Atom targets = XInternAtom(g_xdpy, "TARGETS", False);
    size_t len = 0;
    uint8_t *data = x11_read_selection(targets, &len);
    buf[0] = '\0';
    if (!data || len == 0) { free(data); return; }

    /* data contains array of Atoms */
    Atom *atoms = (Atom *)data;
    int count = len / sizeof(Atom);
    size_t pos = 0;
    for (int i = 0; i < count && pos < buf_size - 64; i++) {
        char *name = XGetAtomName(g_xdpy, atoms[i]);
        if (name) {
            pos += snprintf(buf + pos, buf_size - pos, "%s\n", name);
            XFree(name);
        }
    }
    free(data);
}

static uint8_t *x11_clipboard_read_type(const char *mime, size_t *out_len) {
    if (!g_xdpy || !is_safe_mime(mime)) return NULL;
    Atom target = XInternAtom(g_xdpy, mime, False);
    return x11_read_selection(target, out_len);
}

static void xclip_clipboard_set_image(const uint8_t *data, size_t len, const char *mime) {
    if (!mime || !mime[0]) mime = "image/png";

    /* If BMP data received, convert to PNG for better Linux compatibility */
    uint8_t *converted = NULL;
    if (strcmp(mime, "image/bmp") == 0 && len > 2 &&
        data[0] == 'B' && data[1] == 'M') {
        size_t png_len = 0;
        converted = bmp_to_png_buf(data, len, &png_len);
        if (converted) {
            data = converted;
            len = png_len;
            mime = "image/png";
        }
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xclip -selection clipboard -t %s -i 2>/dev/null", mime);
    FILE *fp = popen(cmd, "w");
    if (!fp) { LOG_WARN("[CLIPBOARD] Failed to set image\n"); free(converted); return; }
    fwrite(data, 1, len, fp);
    pclose(fp);
    LOG_INFO("[CLIPBOARD] Image set OK (%zu bytes, %s)\n", len, mime);
    free(converted);
}

static void xclip_clipboard_set_file(const char *filepath) {
    FILE *fp = popen("xclip -selection clipboard -t x-special/gnome-copied-files -i 2>/dev/null", "w");
    if (!fp) return;
    fprintf(fp, "copy\nfile://%s", filepath);
    pclose(fp);
    LOG_INFO("[CLIPBOARD] File set OK (%s)\n", filepath);
}

/* ---- JSON message builders ---- */

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

static char *build_file_offer(const char *fid, const char *fname, const char *mime,
                              uint64_t fsize, const char *cksum, FtAddrList *addrs) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "file_offer");
    cJSON_AddStringToObject(j, "fileId", fid);
    cJSON_AddStringToObject(j, "fileName", fname);
    cJSON_AddStringToObject(j, "mimeType", mime);
    cJSON_AddNumberToObject(j, "fileSize", (double)fsize);
    cJSON_AddStringToObject(j, "checksum", cksum);
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
    node->msg = strdup(msg);
    node->next = NULL;
    pthread_mutex_lock(&g_send_lock);
    if (g_send_tail) {
        g_send_tail->next = node;
    } else {
        g_send_head = node;
    }
    g_send_tail = node;
    pthread_mutex_unlock(&g_send_lock);
    if (g_wsi) lws_callback_on_writable(g_wsi);
    if (g_context) lws_cancel_service(g_context);
}

/* ---- P2P receiver thread ---- */

static void *receiver_thread(void *arg) {
    TransferState *st = (TransferState*)arg;
    __sync_add_and_fetch(&g_xfer_active_count, 1);
    LOG_INFO("[P2P-RECV] Starting transfer for fileId=%s, sameLan=%d\n", st->file_id, st->same_lan);

    const char *peer_ptrs[FT_MAX_ADDRS];
    for (int i = 0; i < st->peer_addr_count; i++) peer_ptrs[i] = st->peer_addrs[i];

    long long t0 = now_ms();
    const char *method = "failed";

    ft_sock_t nat_sock = FT_INVALID_SOCK;
    if (!st->same_lan && st->peer_public_addr[0] && st->eff_file_level >= 2) {
        LOG_INFO("[P2P-RECV] NAT punch start -> %s\n", st->peer_public_addr);
        nat_sock = ft_nat_punch_start(st->peer_public_addr, g_config.server_host, st->udp_port);
    }

    for (int i = 0; i < st->peer_addr_count; i++)
        LOG_INFO("[P2P-RECV]   peer_addr[%d] = %s\n", i, st->peer_addrs[i]);
    LOG_INFO("[P2P-RECV] listen_fd=%d, LAN+NAT parallel (%d peer addrs, nat=%s, 5000ms)...\n",
           st->listen_fd, st->peer_addr_count, nat_sock != FT_INVALID_SOCK ? "yes" : "no");
    ft_sock_t sock = ft_lan_transfer_auth(st->listen_fd, peer_ptrs, st->peer_addr_count,
                                          nat_sock, 5000,
                                          st->file_id, g_config.aes_key, 0);
    if (sock != FT_INVALID_SOCK) method = "p2p";

    long long t_conn = now_ms();
    long long conn_ms = t_conn - t0;

    if (sock != FT_INVALID_SOCK) {
        LOG_INFO("[P2P-RECV] Connected via %s (%lld ms), receiving file...\n", method, conn_ms);
        char tmp_path[512];
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/syncclip_%s", st->file_name);
        FtFileInfo recv_info = {0};
        if (ft_recv_file_to_path(sock, &recv_info, tmp_path) == 0) {
            long long xfer_ms = now_ms() - t_conn;
            double speed = xfer_ms > 0 ? (double)recv_info.file_size / 1024.0 / 1024.0 / ((double)xfer_ms / 1000.0) : 0;
            LOG_INFO("[P2P-RECV] Received %s (%llu bytes) in %lld ms (%.1f MB/s)\n",
                   st->file_name, (unsigned long long)recv_info.file_size, xfer_ms, speed);
            ft_sha256_file(tmp_path, g_last_img_hash);
            suppress_clipboard_ms(2000);
            if (strstr(st->mime_type, "image/") == st->mime_type) {
                FILE *f = fopen(tmp_path, "rb");
                if (f) {
                    uint8_t *img = (uint8_t*)malloc((size_t)recv_info.file_size);
                    if (img && fread(img, 1, (size_t)recv_info.file_size, f) == (size_t)recv_info.file_size)
                        g_clip_ops.set_image(img, (size_t)recv_info.file_size, st->mime_type);
                    free(img);
                    fclose(f);
                }
                remove(tmp_path);
            } else {
                g_clip_ops.set_file(tmp_path);
                LOG_INFO("[P2P-RECV] File saved to %s\n", tmp_path);
            }
            st->success = 1;
            char *r = build_transfer_result(st->file_id, method, 1, conn_ms, xfer_ms);
            queue_send(r); free(r);
        } else {
            LOG_WARN("[P2P-RECV] Receive failed\n");
        }
        ft_close(sock);
    }

    /* Relay fallback (requires level >= 3) */
    if (!st->success && st->eff_file_level >= 3 && (long long)st->file_size <= st->max_relay_size) {
        LOG_INFO("[P2P-RECV] P2P failed, requesting relay (size=%llu, max=%ld)\n",
               (unsigned long long)st->file_size, st->max_relay_size);
        char *req = build_file_relay_request(st->file_id);
        queue_send(req); free(req);
    } else if (!st->success) {
        LOG_WARN("[P2P-RECV] All methods failed, giving up fileId=%s\n", st->file_id);
        char *r = build_transfer_result(st->file_id, "failed", 0, now_ms() - t0, 0);
        queue_send(r); free(r);
    }

    if (st->listen_fd != FT_INVALID_SOCK) ft_close(st->listen_fd);
    free(st);
    __sync_sub_and_fetch(&g_xfer_active_count, 1);
    return NULL;
}

/* ---- P2P sender thread (for incoming peer connections) ---- */

static void *sender_thread(void *arg) {
    TransferState *st = (TransferState*)arg;
    __sync_add_and_fetch(&g_xfer_active_count, 1);
    LOG_INFO("[P2P-SEND] Starting transfer for fileId=%s, sameLan=%d\n", st->file_id, st->same_lan);

    const char *peer_ptrs[FT_MAX_ADDRS];
    for (int i = 0; i < st->peer_addr_count; i++) peer_ptrs[i] = st->peer_addrs[i];

    long long t0 = now_ms();
    int total_timeout = 5000;
    int sent_count = 0;

    ft_sock_t nat_sock = FT_INVALID_SOCK;
    if (!st->same_lan && st->peer_public_addr[0] && st->eff_file_level >= 2) {
        LOG_INFO("[P2P-SEND] NAT punch start -> %s\n", st->peer_public_addr);
        nat_sock = ft_nat_punch_start(st->peer_public_addr, g_config.server_host, st->udp_port);
    }

    for (int i = 0; i < st->peer_addr_count; i++)
        LOG_INFO("[P2P-SEND]   peer_addr[%d] = %s\n", i, st->peer_addrs[i]);
    LOG_INFO("[P2P-SEND] listen_fd=%d, LAN+NAT parallel (%d peer addrs, nat=%s, %dms)...\n",
           st->listen_fd, st->peer_addr_count, nat_sock != FT_INVALID_SOCK ? "yes" : "no",
           total_timeout);
    ft_sock_t sock = ft_lan_transfer_auth(st->listen_fd, peer_ptrs, st->peer_addr_count,
                                          nat_sock, total_timeout,
                                          st->file_id, g_config.aes_key, 1);

    while (sock != FT_INVALID_SOCK) {
        long long conn_ms = now_ms() - t0;
        LOG_INFO("[P2P-SEND] Connected (%lld ms), sending file (%llu bytes)...\n",
               conn_ms, (unsigned long long)st->file_size);
        long long t_xfer = now_ms();
        int send_ok;
        if (st->source_path[0]) {
            send_ok = (ft_send_file_from_path(sock, st->file_id, st->file_size, st->source_path) == 0);
        } else {
            FtFileInfo fi = {0};
            strncpy(fi.file_id, st->file_id, sizeof(fi.file_id)-1);
            fi.file_size = st->file_size;
            fi.data      = st->data;
            fi.data_len  = st->data_len;
            send_ok = (ft_send_file(sock, &fi) == 0);
        }
        if (send_ok) {
            long long xfer_ms = now_ms() - t_xfer;
            double speed = xfer_ms > 0 ? (double)st->file_size / 1024.0 / 1024.0 / ((double)xfer_ms / 1000.0) : 0;
            sent_count++;
            LOG_INFO("[P2P-SEND] Sent #%d in %lld ms (%.1f MB/s)\n", sent_count, xfer_ms, speed);
        } else {
            LOG_WARN("[P2P-SEND] Send failed\n");
        }
        ft_close(sock);
        sock = FT_INVALID_SOCK;

        if (st->listen_fd == FT_INVALID_SOCK) break;
        int remaining = total_timeout - (int)(now_ms() - t0);
        if (remaining <= 0) break;
        sock = ft_lan_transfer_auth(st->listen_fd, NULL, 0, FT_INVALID_SOCK,
                                    remaining, st->file_id, g_config.aes_key, 1);
    }

    if (sent_count == 0) {
        long long conn_ms = now_ms() - t0;
        LOG_WARN("[P2P-SEND] No connection established after %lld ms, giving up\n", conn_ms);
    } else {
        st->success = 1;
    }

    free(st);
    __sync_sub_and_fetch(&g_xfer_active_count, 1);
    return NULL;
}

/* ---- WebSocket callback ---- */

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    (void)user;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        LOG_INFO("connected to server (v2 authenticated via JWT handshake)");
        g_connected = 1;
        g_authenticated = 1;
        g_logged_in = 1;
        g_reconnect_delay = 1;
        { char *h = sc_msg_hello("linux", g_config.device_id, "2.0.0"); queue_send(h); free(h); }
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

        LOG_INFO("[RECV] type=%s\n", type);

        if (strcmp(type, "hello_ack") == 0) {
            cJSON *ftl = cJSON_GetObjectItem(j, "serverFileLevel");
            g_server_file_level = ftl ? (int)cJSON_GetNumberValue(ftl) : 3;
            LOG_INFO("hello_ack: serverFileLevel=%d", g_server_file_level);
        } else if (strcmp(type, "auth_result") == 0 || strcmp(type, "login_result") == 0) {
            /* Legacy v1 messages — ignore silently if server echoes them. */
        } else if (strcmp(type, "clipboard") == 0) {
            const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(j, "content"));
            if (content) {
                size_t dec_len = 0;
                /* Try v2 GCM first, fallback to legacy CBC. */
                char *dec = aes_gcm_decrypt(content, g_config.aes_key, &dec_len);
                if (!dec) dec = aes_decrypt(content, g_config.aes_key, &dec_len);
                if (dec) {
                    ft_sha256((const uint8_t*)dec, strlen(dec), g_last_clip_hash);
                    suppress_clipboard_ms(2000);
                    g_clip_ops.set_text(dec);
                    free(dec);
                } else {
                    LOG_WARN("clipboard decrypt failed (key mismatch?)");
                }
            }
        } else if (strcmp(type, "file_notify") == 0) {
            if (effective_file_level() <= 0) {
                LOG_INFO("[FILE] File transfer disabled, ignoring file_notify\n");
                cJSON_Delete(j); break;
            }
            /* Another client copied a file/image — start receiving */
            pthread_mutex_lock(&g_xfer_lock);
            if (g_xfer.listen_fd != FT_INVALID_SOCK) { ft_close(g_xfer.listen_fd); g_xfer.listen_fd = FT_INVALID_SOCK; }
            if (g_xfer.data) { free(g_xfer.data); g_xfer.data = NULL; }
            memset(&g_xfer, 0, sizeof(g_xfer));
            g_xfer.listen_fd = FT_INVALID_SOCK;
            g_xfer.is_sender = 0;

            const char *fid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileId"));
            const char *fn  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileName"));
            const char *mt  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "mimeType"));
            const char *ck  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "checksum"));
            const char *fr  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "from"));

            if (fid) strncpy(g_xfer.file_id, fid, sizeof(g_xfer.file_id)-1);
            if (fn)  strncpy(g_xfer.file_name, fn, sizeof(g_xfer.file_name)-1);
            if (mt)  strncpy(g_xfer.mime_type, mt, sizeof(g_xfer.mime_type)-1);
            if (ck)  strncpy(g_xfer.checksum, ck, sizeof(g_xfer.checksum)-1);
            if (fr)  strncpy(g_xfer.from_device, fr, sizeof(g_xfer.from_device)-1);
            g_xfer.file_size = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "fileSize"));
            g_xfer.max_relay_size = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "maxRelaySize"));
            g_xfer.udp_port = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "udpPort"));

            int port = 0;
            g_xfer.listen_fd = ft_start_server(&port);
            g_xfer.listen_port = port;
            g_xfer.phase = 1;

            FtAddrList my_addrs = {0};
            ft_get_local_addresses(&my_addrs, port);

            LOG_INFO("[FILE] Received file_notify: %s (%llu bytes) from %s\n",
                   g_xfer.file_name, (unsigned long long)g_xfer.file_size, g_xfer.from_device);

            char *req = build_file_request(fid, &my_addrs);
            queue_send(req); free(req);
            pthread_mutex_unlock(&g_xfer_lock);

        } else if (strcmp(type, "file_peer_info") == 0) {
            /* Peer info received — start P2P transfer thread */
            pthread_mutex_lock(&g_xfer_lock);
            const char *fid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileId"));
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(j, "role"));
            if (fid && strcmp(fid, g_xfer.file_id) != 0) {
                LOG_INFO("[FILE] Ignoring stale peer_info for fileId=%s (current=%s)\n", fid, g_xfer.file_id);
                pthread_mutex_unlock(&g_xfer_lock);
                cJSON_Delete(j); break;
            }
            if (!g_xfer.is_sender && (g_xfer_active_count > 0 || g_xfer.phase >= 2)) {
                LOG_INFO("[FILE] Ignoring duplicate peer_info (receiver transfer already active)\n");
                pthread_mutex_unlock(&g_xfer_lock);
                cJSON_Delete(j); break;
            }
            if (g_xfer.is_sender && g_xfer_active_count > 0) {
                LOG_INFO("[FILE] Starting parallel sender for additional peer (active=%d)\n", g_xfer_active_count);
            }
            const char *pub = cJSON_GetStringValue(cJSON_GetObjectItem(j, "peerPublicAddress"));
            if (pub) strncpy(g_xfer.peer_public_addr, pub, sizeof(g_xfer.peer_public_addr)-1);

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

            cJSON *ftlj = cJSON_GetObjectItem(j, "fileTransferLevel");
            int srv_level = ftlj ? (int)cJSON_GetNumberValue(ftlj) : g_server_file_level;
            int eff = g_config.file_transfer_level < srv_level ? g_config.file_transfer_level : srv_level;
            g_xfer.eff_file_level = eff;
            g_xfer.same_lan = cJSON_IsTrue(cJSON_GetObjectItem(j, "sameLan"));

            LOG_INFO("[FILE] peer_info role=%s, peer_public=%s, peer_local_count=%d, effLevel=%d, sameLan=%d\n",
                   role ? role : "?", g_xfer.peer_public_addr, g_xfer.peer_addr_count, eff, g_xfer.same_lan);

            g_xfer.phase = 2;
            TransferState *st_copy = (TransferState *)malloc(sizeof(TransferState));
            if (st_copy) {
                memcpy(st_copy, &g_xfer, sizeof(TransferState));
                if (!st_copy->is_sender)
                    g_xfer.listen_fd = FT_INVALID_SOCK;
                pthread_t tid;
                if (st_copy->is_sender)
                    pthread_create(&tid, NULL, sender_thread, st_copy);
                else
                    pthread_create(&tid, NULL, receiver_thread, st_copy);
                pthread_detach(tid);
            }
            pthread_mutex_unlock(&g_xfer_lock);

        } else if (strcmp(type, "file_relay_request") == 0) {
            /* Server asks us (sender) to relay file data */
            const char *requester = cJSON_GetStringValue(cJSON_GetObjectItem(j, "requesterId"));
            LOG_INFO("[RELAY] Relay request from %s for fileId=%s\n",
                   requester ? requester : "?", g_xfer.file_id);
            if (g_xfer.data && g_xfer.data_len > 0 && requester) {
                char *rd = build_file_relay_data(g_xfer.file_id, g_xfer.data,
                                                 g_xfer.data_len, g_xfer.file_size, requester);
                queue_send(rd); free(rd);
                LOG_INFO("[RELAY] Sent relay data (%zu bytes)\n", g_xfer.data_len);
            }
        } else if (strcmp(type, "file_relay_data") == 0) {
            /* Received relayed file data */
            const char *fid  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileId"));
            const char *fn   = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fileName"));
            const char *mt   = cJSON_GetStringValue(cJSON_GetObjectItem(j, "mimeType"));
            const char *b64  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "data"));

            LOG_INFO("[RELAY] Received relay data for %s\n", fn ? fn : "?");
            if (b64) {
                size_t dec_len;
                uint8_t *dec = ft_base64_decode(b64, strlen(b64), &dec_len);
                if (dec) {
                    ft_sha256(dec, dec_len, g_last_img_hash);
                    suppress_clipboard_ms(2000);
                    if (mt && strstr(mt, "image/") == mt) {
                        g_clip_ops.set_image(dec, dec_len, mt);
                    } else {
                        char tmp[512];
                        snprintf(tmp, sizeof(tmp), "/tmp/syncclip_%s", fn ? fn : "file");
                        FILE *f = fopen(tmp, "wb");
                        if (f) { fwrite(dec, 1, dec_len, f); fclose(f); }
                        g_clip_ops.set_file(tmp);
                    }
                    free(dec);
                    char *r = build_transfer_result(fid, "relay", 1, 0, 0);
                    queue_send(r); free(r);
                }
            }
        } else if (strcmp(type, "error") == 0) {
            const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(j, "message"));
            LOG_ERROR("[ERROR] %s\n", m ? m : "unknown");
        }

        cJSON_Delete(j);
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        pthread_mutex_lock(&g_send_lock);
        SendNode *node = g_send_head;
        if (node) {
            g_send_head = node->next;
            if (!g_send_head) g_send_tail = NULL;
        }
        int has_more = (g_send_head != NULL);
        pthread_mutex_unlock(&g_send_lock);
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
        LOG_WARN("[WS] Connection error: %s\n", in ? (char*)in : "?");
        g_connected = 0; g_authenticated = 0; g_logged_in = 0; g_wsi = NULL;
        g_recv_len = 0;
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        LOG_INFO("[WS] Connection closed\n");
        g_connected = 0; g_authenticated = 0; g_logged_in = 0; g_wsi = NULL;
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

/* ---- Process one clipboard change event ---- */

static void process_clipboard_change(void) {
    g_clip_ops.refresh_types(g_cached_types, sizeof(g_cached_types));

    /* Check for image first (requires file transfer enabled) */
    if (effective_file_level() > 0 && clipboard_has_image()) {
        char detected_mime[64] = {0};
        size_t img_len = 0;
        uint8_t *img = clipboard_get_image(&img_len, detected_mime);
        if (img && img_len > 0) {
            char img_hash[65];
            ft_sha256(img, img_len, img_hash);
            if (strcmp(img_hash, g_last_img_hash) == 0) {
                free(img);
                return;
            }
            strcpy(g_last_img_hash, img_hash);
            const char *ext = strstr(detected_mime, "jpeg") ? "clipboard.jpg" : "clipboard.png";
            LOG_INFO("[MONITOR] Image clipboard changed (%zu bytes, %s)\n", img_len, detected_mime);

            if (g_xfer_active_count > 0) {
                LOG_INFO("[MONITOR] Waiting for previous transfer to finish...\n");
                for (int i = 0; i < 50 && g_xfer_active_count > 0; i++) usleep(10000);
                if (g_xfer_active_count > 0) {
                    LOG_INFO("[MONITOR] Previous transfer still active, skipping\n");
                    free(img);
                    return;
                }
            }

            pthread_mutex_lock(&g_xfer_lock);
            if (g_xfer.listen_fd != FT_INVALID_SOCK) { ft_close(g_xfer.listen_fd); g_xfer.listen_fd = FT_INVALID_SOCK; }
            if (g_xfer.data) free(g_xfer.data);
            memset(&g_xfer, 0, sizeof(g_xfer));
            g_xfer.listen_fd = FT_INVALID_SOCK;
            g_xfer.is_sender = 1;
            ft_generate_uuid(g_xfer.file_id);
            strncpy(g_xfer.file_name, ext, sizeof(g_xfer.file_name)-1);
            strncpy(g_xfer.mime_type, detected_mime, sizeof(g_xfer.mime_type)-1);
            g_xfer.file_size = img_len;
            g_xfer.data      = img;
            g_xfer.data_len  = img_len;
            ft_sha256(img, img_len, g_xfer.checksum);

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
            pthread_mutex_unlock(&g_xfer_lock);
            return;
        }
        if (img) free(img);
    }

    /* Check for file copy (requires file transfer enabled) */
    if (effective_file_level() > 0 && clipboard_has_file()) {
        char *fpath = clipboard_get_file_uri();
        if (fpath) {
            struct stat st_buf;
            if (stat(fpath, &st_buf) != 0 || !S_ISREG(st_buf.st_mode)) {
                LOG_INFO("[MONITOR] Cannot stat file: %s\n", fpath);
                free(fpath); return;
            }
            long long fsize = (long long)st_buf.st_size;
            if (fsize <= 0 || fsize >= 1024LL*1024*g_config.max_transfer_size) {
                LOG_INFO("[MONITOR] File skipped (size=%lld): %s\n", fsize, fpath);
                free(fpath); return;
            }

            const char *basename = strrchr(fpath, '/');
            basename = basename ? basename + 1 : fpath;

            /* Wait for any active transfer thread to finish (max 500ms) */
            if (g_xfer_active_count > 0) {
                LOG_INFO("[MONITOR] Waiting for previous transfer to finish...\n");
                for (int i = 0; i < 50 && g_xfer_active_count > 0; i++) usleep(10000);
                if (g_xfer_active_count > 0) {
                    LOG_INFO("[MONITOR] Previous transfer still active, skipping\n");
                    free(fpath);
                    return;
                }
            }

            char fhash[65];
            ft_sha256_file(fpath, fhash);
            LOG_INFO("[MONITOR] File clipboard changed: %s (%lld bytes)\n", basename, fsize);

            pthread_mutex_lock(&g_xfer_lock);
            if (g_xfer.listen_fd != FT_INVALID_SOCK) { ft_close(g_xfer.listen_fd); g_xfer.listen_fd = FT_INVALID_SOCK; }
            if (g_xfer.data) free(g_xfer.data);
            memset(&g_xfer, 0, sizeof(g_xfer));
            g_xfer.listen_fd = FT_INVALID_SOCK;
            g_xfer.is_sender = 1;
            ft_generate_uuid(g_xfer.file_id);
            strncpy(g_xfer.file_name, basename, sizeof(g_xfer.file_name)-1);
            strncpy(g_xfer.mime_type, mime_from_extension(basename), sizeof(g_xfer.mime_type)-1);
            g_xfer.file_size = fsize;
            g_xfer.data      = NULL;
            g_xfer.data_len  = 0;
            strncpy(g_xfer.source_path, fpath, sizeof(g_xfer.source_path)-1);
            strcpy(g_xfer.checksum, fhash);
            strcpy(g_last_img_hash, fhash);

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
            pthread_mutex_unlock(&g_xfer_lock);
            free(fpath);
            return;
        }
    }

    /* Text clipboard */
    char *clip = g_clip_ops.get_text();
    if (!clip) return;
    char hash[65]; ft_sha256((const uint8_t*)clip, strlen(clip), hash);
    if (strcmp(hash, g_last_clip_hash) != 0) {
        strcpy(g_last_clip_hash, hash);
        LOG_INFO("[MONITOR] Text clipboard changed (%zu bytes)\n", strlen(clip));
        char *enc = aes_gcm_encrypt(clip, strlen(clip), g_config.aes_key);
        if (enc) { char *m = build_clipboard_msg(enc); queue_send(m); free(m); free(enc); }
    }
    free(clip);
}

/* ---- PRIMARY selection sync (for terminal Ctrl+C which is SIGINT, not copy) ---- */

static void process_primary_change(void) {
    if (!g_xdpy) return;
    Atom utf8 = XInternAtom(g_xdpy, "UTF8_STRING", False);
    size_t len = 0;
    uint8_t *data = x11_read_selection_ex(XA_PRIMARY, utf8, &len);
    if (!data) {
        data = x11_read_selection_ex(XA_PRIMARY, XA_STRING, &len);
    }
    if (!data || len == 0) { free(data); return; }

    char *text = (char *)realloc(data, len + 1);
    if (!text) { free(data); return; }
    text[len] = '\0';

    if (strlen(text) == 0) { free(text); return; }

    char hash[65];
    ft_sha256((const uint8_t *)text, strlen(text), hash);
    if (strcmp(hash, g_last_clip_hash) != 0) {
        strcpy(g_last_clip_hash, hash);
        LOG_INFO("[MONITOR] PRIMARY text changed (%zu bytes)\n", strlen(text));
        char *enc = aes_gcm_encrypt(text, strlen(text), g_config.aes_key);
        if (enc) {
            char *m = build_clipboard_msg(enc);
            queue_send(m);
            free(m);
            free(enc);
        }
    }
    free(text);
}

/* ---- Clipboard monitoring thread (XFIXES event-driven / Wayland polling) ---- */

/*
 * On X11: Uses XFIXES SelectionNotify to detect clipboard changes,
 * and X11 Selection API for direct reads (no xclip subprocesses).
 * On Wayland: Uses polling with wl-paste via g_clip_ops.
 * Fallback: polling with xclip if XFIXES is unavailable.
 */
static void *clipboard_monitor(void *arg) {
    (void)arg;

    char *initial = g_clip_ops.get_text();
    if (initial) { ft_sha256((const uint8_t*)initial, strlen(initial), g_last_clip_hash); free(initial); }

    if (g_use_wayland) {
        LOG_INFO("[MONITOR] Clipboard monitor started (Wayland polling, interval=%dms)\n", POLL_INTERVAL_MS);
        goto fallback_polling;
    }

    XInitThreads();
    Display *xdpy = XOpenDisplay(NULL);
    if (!xdpy) {
        LOG_WARN("[MONITOR] Cannot open X display (DISPLAY=%s), falling back to polling\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(null)");
        goto fallback_polling;
    }

    int xfixes_event, xfixes_error;
    if (!XFixesQueryExtension(xdpy, &xfixes_event, &xfixes_error)) {
        LOG_WARN("[MONITOR] XFIXES not available, falling back to polling\n");
        XCloseDisplay(xdpy);
        goto fallback_polling;
    }

    XFixesQueryVersion(xdpy, &xfixes_event, &xfixes_error);
    Window xwin = XCreateSimpleWindow(xdpy, DefaultRootWindow(xdpy), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(xdpy, xwin, PropertyChangeMask);

    int xf_ev_base, xf_err_base;
    XFixesQueryExtension(xdpy, &xf_ev_base, &xf_err_base);

    Atom clip_atom = XInternAtom(xdpy, "CLIPBOARD", False);
    XFixesSelectSelectionInput(xdpy, xwin, clip_atom,
        XFixesSetSelectionOwnerNotifyMask |
        XFixesSelectionWindowDestroyNotifyMask |
        XFixesSelectionClientCloseNotifyMask);

    /* Also monitor PRIMARY selection (for terminal text selections) */
    XFixesSelectSelectionInput(xdpy, xwin, XA_PRIMARY,
        XFixesSetSelectionOwnerNotifyMask |
        XFixesSelectionWindowDestroyNotifyMask |
        XFixesSelectionClientCloseNotifyMask);
    XFlush(xdpy);

    /* Set up globals for X11 direct reads */
    g_xdpy = xdpy;
    g_xwin = xwin;
    g_clip_atom = clip_atom;
    g_prop_atom = XInternAtom(xdpy, "SYNC_CLIP_PROP", False);

    /* Upgrade to X11 direct reads (no xclip subprocesses) */
    g_clip_ops.get_text = x11_clipboard_get_text;
    g_clip_ops.refresh_types = x11_clipboard_refresh_types;
    g_clip_ops.read_type = x11_clipboard_read_type;
    /* Keep xclip for writes (set_text, set_image, set_file) */

    LOG_INFO("[MONITOR] Clipboard monitor started (XFIXES event-driven + X11 direct reads, CLIPBOARD+PRIMARY, event_base=%d)\n", xf_ev_base);

    int xfd = ConnectionNumber(xdpy);
    long long primary_pending_ms = 0;

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(xfd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };

        select(xfd + 1, &rfds, NULL, NULL, &tv);

        while (XPending(xdpy)) {
            XEvent ev;
            XNextEvent(xdpy, &ev);

            if (ev.type == xf_ev_base + XFixesSelectionNotify) {
                XFixesSelectionNotifyEvent *sev = (XFixesSelectionNotifyEvent *)&ev;

                if (sev->selection == clip_atom) {
                    usleep(100000);  /* 100ms debounce for content to settle */
                    if (!g_logged_in) continue;
                    if (is_suppressed()) {
                        LOG_INFO("[XFIXES] CLIPBOARD changed (suppressed)\n");
                        continue;
                    }
                    LOG_INFO("[XFIXES] CLIPBOARD changed\n");
                    process_clipboard_change();
                } else if (sev->selection == XA_PRIMARY) {
                    primary_pending_ms = now_ms();
                }
            }
        }

        /* Debounced PRIMARY processing: wait 500ms after last change */
        if (primary_pending_ms > 0 && now_ms() - primary_pending_ms >= 500) {
            primary_pending_ms = 0;
            if (g_logged_in && !is_suppressed()) {
                LOG_INFO("[XFIXES] PRIMARY changed (debounced)\n");
                process_primary_change();
            }
        }
    }

    g_xdpy = NULL;
    g_xwin = 0;
    XDestroyWindow(xdpy, xwin);
    XCloseDisplay(xdpy);
    return NULL;

fallback_polling:
    LOG_INFO("[MONITOR] Clipboard monitor started (polling, interval=%dms)\n", POLL_INTERVAL_MS);
    while (g_running) {
        usleep(POLL_INTERVAL_MS * 1000);
        if (!g_logged_in) continue;
        if (is_suppressed()) continue;
        process_clipboard_change();
    }
    return NULL;
}

static const lws_retry_bo_t ws_retry = {
    .secs_since_valid_ping = 30,
    .secs_since_valid_hangup = 60,
};

int main(int argc, char **argv) {
    const char *config_path = "config.ini";
    if (argc > 1) config_path = argv[1];
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    config_load(config_path, &g_config);
    ft_set_max_file_size((uint64_t)g_config.max_transfer_size * 1024ULL * 1024ULL);

    detect_display_server();

    /* Initialize clipboard backend */
    if (g_use_wayland) {
        /* Check if wl-paste is available */
        if (system("which wl-paste >/dev/null 2>&1") == 0) {
            g_clip_ops.get_text = wl_clipboard_get_text;
            g_clip_ops.set_text = wl_clipboard_set_text;
            g_clip_ops.refresh_types = wl_clipboard_refresh_types;
            g_clip_ops.read_type = wl_clipboard_read_type;
            g_clip_ops.set_image = wl_clipboard_set_image;
            g_clip_ops.set_file = wl_clipboard_set_file;
            LOG_INFO("[CLIPBOARD] Using wl-clipboard backend\n");
        } else {
            LOG_INFO("[CLIPBOARD] wl-clipboard not found, falling back to xclip\n");
            /* Fall through to xclip setup below */
            g_use_wayland = 0;
        }
    }
    if (!g_use_wayland) {
        g_clip_ops.get_text = xclip_clipboard_get_text;
        g_clip_ops.set_text = xclip_clipboard_set_text;
        g_clip_ops.refresh_types = xclip_clipboard_refresh_types;
        g_clip_ops.read_type = xclip_clipboard_read_type;
        g_clip_ops.set_image = xclip_clipboard_set_image;
        g_clip_ops.set_file = xclip_clipboard_set_file;
        LOG_INFO("[CLIPBOARD] Using xclip backend\n");
    }

    LOG_INFO("=== SyncClipboard Linux Client ===\n");
    LOG_INFO("Server: %s:%d%s\n", g_config.server_host, g_config.server_port, g_config.server_path);
    LOG_INFO("User: %s, Device: %s\n", g_config.username, g_config.device_id);

    pthread_t mon_tid;
    pthread_create(&mon_tid, NULL, clipboard_monitor, NULL);

    struct lws_context_creation_info ci;
    memset(&ci, 0, sizeof(ci));
    ci.port = CONTEXT_PORT_NO_LISTEN;
    ci.protocols = protocols;
    ci.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    struct lws_context *ctx = lws_create_context(&ci);
    if (!ctx) { g_running = 0; return 1; }
    g_context = ctx;

    while (g_running) {
        if (!g_connected && !g_wsi) {
            /* v2: acquire JWT before attempting WS handshake */
            if (ws_acquire_tokens(&g_config, &g_tokens) != 0) {
                LOG_WARN("could not acquire JWT, retrying in %ds", g_reconnect_delay);
                sleep(g_reconnect_delay);
                if (g_reconnect_delay < 60) g_reconnect_delay *= 2;
                continue;
            }
            ws_build_bearer_protocol(g_tokens.access_token, g_ws_subproto, sizeof(g_ws_subproto));

            struct lws_client_connect_info conn;
            memset(&conn, 0, sizeof(conn));
            conn.context  = ctx;
            conn.address  = g_config.server_host;
            conn.port     = g_config.server_port;
            conn.path     = "/ws/v2/clipboard";
            conn.host     = g_config.server_host;
            conn.origin   = g_config.server_host;
            conn.protocol = g_ws_subproto;
            conn.retry_and_idle_policy = &ws_retry;
            LOG_INFO("connecting to %s:%d/ws/v2/clipboard", g_config.server_host, g_config.server_port);
            g_wsi = lws_client_connect_via_info(&conn);
            if (!g_wsi) {
                sleep(g_reconnect_delay);
                if (g_reconnect_delay < 60) g_reconnect_delay *= 2;
                if (g_reconnect_delay > 60) g_reconnect_delay = 60;
                continue;
            }
        }
        lws_service(ctx, 250);
    }

    g_running = 0;
    pthread_join(mon_tid, NULL);
    lws_context_destroy(ctx);
    if (g_xfer.data) free(g_xfer.data);
    return 0;
}
