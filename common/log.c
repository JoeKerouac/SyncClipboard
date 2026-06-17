#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_lock;
static int g_lock_init = 0;
static void log_lock_init(void) {
    if (!g_lock_init) { InitializeCriticalSection(&g_log_lock); g_lock_init = 1; }
}
static void log_lock(void)   { log_lock_init(); EnterCriticalSection(&g_log_lock); }
static void log_unlock(void) { LeaveCriticalSection(&g_log_lock); }
#else
#include <pthread.h>
#include <sys/stat.h>
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static void log_lock(void)   { pthread_mutex_lock(&g_log_lock); }
static void log_unlock(void) { pthread_mutex_unlock(&g_log_lock); }
#endif

static int g_level = SC_LOG_INFO;
static FILE *g_file = NULL;
static char g_path[512];
static long long g_max_bytes = 10LL * 1024 * 1024;

static const char *level_name(int level) {
    switch (level) {
        case SC_LOG_DEBUG: return "DEBUG";
        case SC_LOG_INFO:  return "INFO ";
        case SC_LOG_WARN:  return "WARN ";
        case SC_LOG_ERROR: return "ERROR";
        default: return "?    ";
    }
}

void sc_log_set_level(int level) { g_level = level; }

int sc_log_set_file(const char *path, int max_mb) {
    log_lock();
    if (g_file) { fclose(g_file); g_file = NULL; }
    if (!path || !*path) { g_path[0] = '\0'; log_unlock(); return 0; }
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    if (max_mb > 0) g_max_bytes = (long long)max_mb * 1024 * 1024;
    g_file = fopen(g_path, "a");
    int ok = g_file != NULL;
    log_unlock();
    return ok ? 0 : -1;
}

void sc_log_close(void) {
    log_lock();
    if (g_file) { fclose(g_file); g_file = NULL; }
    log_unlock();
}

static void rotate_if_needed_locked(void) {
    if (!g_file || g_path[0] == '\0') return;
    long pos = ftell(g_file);
    if (pos < 0) return;
    if ((long long)pos < g_max_bytes) return;
    fclose(g_file);
    char rotated[600];
    snprintf(rotated, sizeof(rotated), "%s.1", g_path);
    remove(rotated);
    rename(g_path, rotated);
    g_file = fopen(g_path, "a");
}

void sc_log_msg(int level, const char *file, int line, const char *fmt, ...) {
    (void)file; (void)line;
    if (level < g_level) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    time_t t = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

    log_lock();
    if (g_file) {
        fprintf(g_file, "%s %s %s\n", ts, level_name(level), buf);
        fflush(g_file);
        rotate_if_needed_locked();
    } else {
        fprintf(stderr, "%s %s %s\n", ts, level_name(level), buf);
    }
    log_unlock();
}
