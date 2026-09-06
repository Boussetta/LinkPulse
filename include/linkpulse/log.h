#ifndef LINKPULSE_LOG_H
#define LINKPULSE_LOG_H

typedef enum {
    LP_LOG_ERROR = 0,
    LP_LOG_WARN,
    LP_LOG_INFO,
    LP_LOG_DEBUG
} lp_log_level_t;

void lp_log_set_level(lp_log_level_t level);
lp_log_level_t lp_log_get_level(void);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void lp_log(lp_log_level_t level, const char *fmt, ...);

#define LP_ERROR(...) lp_log(LP_LOG_ERROR, __VA_ARGS__)
#define LP_WARN(...) lp_log(LP_LOG_WARN, __VA_ARGS__)
#define LP_INFO(...) lp_log(LP_LOG_INFO, __VA_ARGS__)
#define LP_DEBUG(...) lp_log(LP_LOG_DEBUG, __VA_ARGS__)

#endif /* LINKPULSE_LOG_H */
