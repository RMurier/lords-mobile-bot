#include "log.h"
#include <stdarg.h>

int g_log_debug = 1;          /* on by default: the raw exchanges are what lets a refused login be diagnosed */
int g_log_debug_forced = 0;   /* --debug on the command line: the config cannot turn it off */

static void log_base(const char *prefix, const char *fmt, va_list args)
{
    fprintf(stderr, "%s", prefix);
    vfprintf(stderr, fmt, args);
    // fprintf(stderr, "\n");
}

void log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_base("[ERROR] ", fmt, args);
    va_end(args);
}

void log_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_base("[WARN ] ", fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_base("[INFO ] ", fmt, args);
    va_end(args);
}

void log_debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_base("[DEBUG] ", fmt, args);
    va_end(args);
}

void log_hexdump(const char *label, const uint8_t *data, size_t size)
{
    fprintf(stderr, "[DEBUG] %s (%zu bytes)\n", label, size);

    for (size_t i = 0; i < size; i += 16) {
        fprintf(stderr, "[DEBUG]   %04zx: ", i);

        for (size_t j = 0; j < 16; j++) {
            if (i + j < size)
                fprintf(stderr, "%02x ", data[i + j]);
            else
                fprintf(stderr, "   ");
        }

        for (size_t j = 0; j < 16 && i + j < size; j++) {
            uint8_t ch = data[i + j];
            fputc((ch >= 32 && ch < 127) ? ch : '.', stderr);
        }

        fputc('\n', stderr);
    }
}
