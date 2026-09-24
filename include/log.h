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
extern int g_log_debug_forced;

/* Function declarations */
void log_error(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_debug(const char *fmt, ...);
void log_hexdump(const char *label, const uint8_t *data, size_t size);

/* Full capture of every packet, both directions, appended to <data_path>packets.log
 * (config: log.packets). Unlike log.debug it is never truncated and goes to a file, so a
 * whole session can be searched afterwards for a value the bot does not decode yet.
 * dir is "<-" (received) or "->" (sent, before encryption). The file holds session
 * data (login packets): never share it as is. */
extern int g_log_packets;
void ensure_directory(const char *path);
void log_packet(const char *data_path, const char *dir, uint16_t id, const char *name,
                const uint8_t *payload, size_t size);

#endif