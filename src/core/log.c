#if !defined(_WIN32)
/* Keeps core syntax-checkable with a plain Linux gcc during development. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "linkpulse/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static lp_log_level_t g_level = LP_LOG_INFO;

void lp_log_set_level(lp_log_level_t level)
{
    g_level = level;
}

lp_log_level_t lp_log_get_level(void)
{
    return g_level;
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
    if (level > g_level) {
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

    fprintf(stderr, "[%s] %s ", stamp, level_tag(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
}
