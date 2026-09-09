#ifndef LINKPULSE_PLATFORM_LOG_H
#define LINKPULSE_PLATFORM_LOG_H

/* Opens the per-user LinkPulse log and attaches it to the core logger. */
int lp_win32_log_start(void);

/* Flushes and detaches the platform log file. */
void lp_win32_log_stop(void);

#endif /* LINKPULSE_PLATFORM_LOG_H */
