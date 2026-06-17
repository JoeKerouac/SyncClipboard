#ifndef SYNC_CLIPBOARD_LOG_H
#define SYNC_CLIPBOARD_LOG_H

#include <stdarg.h>

/* Log levels (mirrors ClientConfig.log_level). */
#define SC_LOG_DEBUG 0
#define SC_LOG_INFO  1
#define SC_LOG_WARN  2
#define SC_LOG_ERROR 3

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the global log level threshold. Messages below the threshold
 * are dropped without formatting. */
void sc_log_set_level(int level);

/* Optionally redirect logs to a file. Pass NULL/"" to log to stderr. The
 * file rolls over once it exceeds max_mb megabytes. */
int sc_log_set_file(const char *path, int max_mb);

void sc_log_close(void);

void sc_log_msg(int level, const char *file, int line, const char *fmt, ...);

#define LOG_DEBUG(...) sc_log_msg(SC_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  sc_log_msg(SC_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  sc_log_msg(SC_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) sc_log_msg(SC_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
