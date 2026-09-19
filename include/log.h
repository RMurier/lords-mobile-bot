#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* Simple logging macros */
#define LOGE(fmt, ...) log_error(fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) log_info(fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) log_warn(fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) do { if (g_log_debug) log_debug(fmt, ##__VA_ARGS__); } while (0)

/* Set to 1 to print raw server responses (config: log.debug, CLI: --debug) */
extern int g_log_debug;

/* Function declarations */
void log_error(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_debug(const char *fmt, ...);
void log_hexdump(const char *label, const uint8_t *data, size_t size);

#endif