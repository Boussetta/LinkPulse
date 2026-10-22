#if !defined(_WIN32)
/* Keeps core syntax-checkable with a plain Linux gcc during development. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "linkpulse/log.h"

#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static atomic_int g_level = LP_LOG_INFO;
static atomic_flag g_log_lock = ATOMIC_FLAG_INIT;
static FILE *g_file;

/* Builds a sortable local timestamp without depending on a platform clock API. */
static void format_timestamp(char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return;
    }
    snprintf(out, out_cap, "0000-00-00 00:00:00.000");

    time_t now_seconds;
    long milliseconds = 0;
#if defined(__MINGW32__)
    /* MinGW-w64's bundled <time.h> does not reliably declare C11's
       timespec_get/TIME_UTC (confirmed on this project's mingw-cross CI
       toolchain), so this path falls back to whole-second resolution. */
    now_seconds = time(NULL);
#else
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return;
    }
    now_seconds = now.tv_sec;
    milliseconds = now.tv_nsec / 1000000L;
#endif

    struct tm local_now;
#if defined(_WIN32)
    if (localtime_s(&local_now, &now_seconds) != 0) {
#else
    if (localtime_r(&now_seconds, &local_now) == NULL) {
#endif
        return;
    }

    snprintf(out, out_cap, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             local_now.tm_year + 1900, local_now.tm_mon + 1, local_now.tm_mday,
             local_now.tm_hour, local_now.tm_min, local_now.tm_sec, milliseconds);
}

/* Publishes the threshold atomically because workers can log concurrently. */
void lp_log_set_level(lp_log_level_t level)
{
    if (level < LP_LOG_ERROR || level > LP_LOG_DEBUG) {
        level = LP_LOG_INFO;
    }
    atomic_store_explicit(&g_level, (int)level, memory_order_relaxed);
}

/* Reads the current threshold without taking the output lock. */
lp_log_level_t lp_log_get_level(void)
{
    return (lp_log_level_t)atomic_load_explicit(&g_level, memory_order_relaxed);
}

/* Swaps the optional file sink while serializing with active log writers. */
void lp_log_set_file(FILE *file)
{
    while (atomic_flag_test_and_set_explicit(&g_log_lock, memory_order_acquire)) {
    }
    g_file = file;
    atomic_flag_clear_explicit(&g_log_lock, memory_order_release);
}

/* Keeps the textual severity width fixed for readable console and file logs. */
static const char *level_tag(lp_log_level_t level)
{
    switch (level) {
    case LP_LOG_ERROR:
        return "ERROR";
    case LP_LOG_WARN:
        return "WARN ";
    case LP_LOG_INFO:
        return "INFO ";
    case LP_LOG_DEBUG:
        return "DEBUG";
    }
    return "?????";
}

/* Emits one timestamped message to stderr and, when configured, the file sink. */
void lp_log(lp_log_level_t level, const char *fmt, ...)
{
    if (level > lp_log_get_level()) {
        return;
    }

    char stamp[32];
    format_timestamp(stamp, sizeof(stamp));

    /* Format the whole record before taking the lock, so other threads only
       spin for the duration of the writes. One byte is kept for the newline. */
    char line[1024];
    const size_t body_cap = sizeof(line) - 1;
    size_t used = 0;
    const int prefix = snprintf(line, body_cap, "[%s] %s ", stamp, level_tag(level));
    if (prefix > 0) {
        used = ((size_t)prefix < body_cap) ? (size_t)prefix : body_cap - 1;
    }
    va_list args;
    va_start(args, fmt);
    const int body = vsnprintf(line + used, body_cap - used, fmt, args);
    va_end(args);
    if (body > 0) {
        used += ((size_t)body < body_cap - used) ? (size_t)body : body_cap - used - 1;
    }
    line[used++] = '\n';
    line[used] = '\0';

    while (atomic_flag_test_and_set_explicit(&g_log_lock, memory_order_acquire)) {
    }
    fputs(line, stderr);
    fflush(stderr);
    if (g_file != NULL) {
        fputs(line, g_file);
        fflush(g_file);
    }
    atomic_flag_clear_explicit(&g_log_lock, memory_order_release);
}
