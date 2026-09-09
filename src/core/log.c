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

/* Publishes the threshold atomically because workers can log concurrently. */
void lp_log_set_level(lp_log_level_t level)
{
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

    char stamp[32] = "--:--:--";
    const time_t now = time(NULL);
    struct tm tm_buf;
#if defined(_WIN32)
    if (localtime_s(&tm_buf, &now) == 0) {
#else
    if (localtime_r(&now, &tm_buf) != NULL) {
#endif
        strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_buf);
    }

    va_list args;
    va_start(args, fmt);
    va_list file_args;
    va_copy(file_args, args);
    while (atomic_flag_test_and_set_explicit(&g_log_lock, memory_order_acquire)) {
    }
    fprintf(stderr, "[%s] %s ", stamp, level_tag(level));
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    if (g_file != NULL) {
        fprintf(g_file, "[%s] %s ", stamp, level_tag(level));
        vfprintf(g_file, fmt, file_args);
        fputc('\n', g_file);
        fflush(g_file);
    }
    va_end(file_args);
    atomic_flag_clear_explicit(&g_log_lock, memory_order_release);
    va_end(args);
}
