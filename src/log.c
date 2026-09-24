#include "log.h"
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
#endif

#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

int g_log_debug = 1;          /* on by default: the raw exchanges are what lets a refused login be diagnosed */
int g_log_debug_forced = 0;   /* --debug on the command line: the config cannot turn it off */

/* Local time, millisecond precision: without it two log lines a couple of seconds apart
 * (e.g. the human-pacing delay between two marches) look identical and a real gap cannot be
 * told apart from none at all. */
static void print_timestamp(void)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(stderr, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    fprintf(stderr, "%02d:%02d:%02d.%03ld ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000);
#endif
}

static void log_base(const char *prefix, const char *fmt, va_list args)
{
    print_timestamp();
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
    print_timestamp();
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


int g_log_packets = 0;

/* Creates every folder of the path that does not exist yet (mkdir -p). Best effort, no error. */
void ensure_directory(const char *path)
{
    char dir[300];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/' && *p != '\\')
            continue;
        char saved = *p;
        *p = '\0';
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0700);
#endif
        *p = saved;
    }
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0700);
#endif
}

void log_packet(const char *data_path, const char *dir, uint16_t id, const char *name,
                const uint8_t *payload, size_t size)
{
    if (!g_log_packets)
        return;

    char path[320];
    size_t len = strlen(data_path);
    snprintf(path, sizeof(path), "%s%spackets.log", data_path,
             (len > 0 && (data_path[len - 1] == '/' || data_path[len - 1] == '\\')) ? "" : "/");

    FILE *f = fopen(path, "a");
    if (!f) {
        /* the data folder is only created on demand elsewhere: make it, then retry once */
        ensure_directory(data_path);
        f = fopen(path, "a");
        if (!f)
            return;
    }

    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    fprintf(f, "%02d:%02d:%02d %s %s (%u) %zu bytes\n",
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec, dir, name, id, size);

    for (size_t i = 0; i < size; i += 16) {
        fprintf(f, "  %04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size)
                fprintf(f, "%02x ", payload[i + j]);
            else
                fprintf(f, "   ");
        }
        for (size_t j = 0; j < 16 && i + j < size; j++) {
            uint8_t ch = payload[i + j];
            fputc((ch >= 32 && ch < 127) ? ch : '.', f);
        }
        fputc('\n', f);
    }
    fclose(f);
}
