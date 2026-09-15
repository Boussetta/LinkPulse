#ifndef LINKPULSE_SPEEDTEST_H
#define LINKPULSE_SPEEDTEST_H

#include "linkpulse/status.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LP_SPEEDTEST_PHASE_IDLE = 0,
    LP_SPEEDTEST_PHASE_LATENCY,
    LP_SPEEDTEST_PHASE_DOWNLOAD,
    LP_SPEEDTEST_PHASE_UPLOAD,
    LP_SPEEDTEST_PHASE_DONE,
    LP_SPEEDTEST_PHASE_FAILED,
    LP_SPEEDTEST_PHASE_CANCELLED
} lp_speedtest_phase_t;

typedef enum {
    LP_SPEEDTEST_QUALITY_UNKNOWN = 0,
    LP_SPEEDTEST_QUALITY_POOR,
    LP_SPEEDTEST_QUALITY_FAIR,
    LP_SPEEDTEST_QUALITY_GOOD,
    LP_SPEEDTEST_QUALITY_EXCELLENT
} lp_speedtest_quality_t;

typedef struct {
    lp_speedtest_phase_t phase;
    unsigned phase_percent;          /* 0-100 completion of the running phase */
    bool has_latency;
    bool has_download;
    bool has_upload;
    uint64_t latency_us;             /* median HTTP round trip */
    uint64_t jitter_us;              /* mean deviation between consecutive probes */
    uint64_t download_bytes_per_sec;
    uint64_t upload_bytes_per_sec;
    uint64_t live_bytes_per_sec;     /* throughput of the phase currently running */
    lp_status_t status;
} lp_speedtest_progress_t;

/* Called on the worker thread whenever the run advances; must not block. */
typedef void (*lp_speedtest_progress_fn)(const lp_speedtest_progress_t *progress, void *user_data);

typedef struct {
    lp_speedtest_progress_fn on_progress;
    void *user_data;
    /* Polled between transfer chunks; a non-zero value aborts the run. */
    const volatile long *cancel_flag;
} lp_speedtest_request_t;

/* Runs latency, download and upload probes against a public measurement
   endpoint. Blocking, and intended to be called from a worker thread. */
lp_status_t lp_speedtest_run(const lp_speedtest_request_t *request, lp_speedtest_progress_t *out);

/* Returns a short human-readable name for a phase, e.g. "Download". */
const char *lp_speedtest_phase_str(lp_speedtest_phase_t phase);

/* Grades a run; the weakest measured dimension decides the overall result. */
lp_speedtest_quality_t lp_speedtest_quality(const lp_speedtest_progress_t *progress);

/* Returns the display name for a grade, e.g. "Good". */
const char *lp_speedtest_quality_str(lp_speedtest_quality_t quality);

/* Returns a one-line summary of what the connection comfortably supports. */
const char *lp_speedtest_quality_hint(lp_speedtest_quality_t quality);

#endif /* LINKPULSE_SPEEDTEST_H */
