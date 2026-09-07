#if !defined(_WIN32)
/* Keeps core syntax-checkable with a plain Linux gcc during development. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "linkpulse/log.h"

#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static atomic_int g_level = ATOMIC_VAR_INIT(LP_LOG_INFO);

#if defined(_WIN32)
#define LP_FLOCKFILE(stream) _lock_file(stream)
#define LP_FUNLOCKFILE(stream) _unlock_file(stream)
#else
#define LP_FLOCKFILE(stream) flockfile(stream)
#define LP_FUNLOCKFILE(stream) funlockfile(stream)
#endif

void lp_log_set_level(lp_log_level_t level)
{
    atomic_store_explicit(&g_level, (int)level, memory_order_relaxed);
}

lp_log_level_t lp_log_get_level(void)
{
    return (lp_log_level_t)atomic_load_explicit(&g_level, memory_order_relaxed);
}

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
    LP_FLOCKFILE(stderr);
    fprintf(stderr, "[%s] %s ", stamp, level_tag(level));
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    LP_FUNLOCKFILE(stderr);
    va_end(args);
}
