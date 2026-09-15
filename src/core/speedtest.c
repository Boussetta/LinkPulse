#include "linkpulse/speedtest.h"

const char *lp_speedtest_phase_str(lp_speedtest_phase_t phase)
{
    switch (phase) {
    case LP_SPEEDTEST_PHASE_IDLE:
        return "Idle";
    case LP_SPEEDTEST_PHASE_LATENCY:
        return "Latency";
    case LP_SPEEDTEST_PHASE_DOWNLOAD:
        return "Download";
    case LP_SPEEDTEST_PHASE_UPLOAD:
        return "Upload";
    case LP_SPEEDTEST_PHASE_DONE:
        return "Done";
    case LP_SPEEDTEST_PHASE_CANCELLED:
        return "Cancelled";
    case LP_SPEEDTEST_PHASE_FAILED:
    default:
        return "Failed";
    }
}

/* Grades a throughput dimension from 1 (poor) to 4 (excellent). */
static int rate_tier(uint64_t bytes_per_sec, double fair, double good, double excellent)
{
    const double mbps = (double)bytes_per_sec * 8.0 / 1000000.0;
    if (mbps >= excellent) {
        return 4;
    }
    if (mbps >= good) {
        return 3;
    }
    if (mbps >= fair) {
        return 2;
    }
    return 1;
}

/* Same scale as rate_tier, but lower is better. */
static int delay_tier(uint64_t microseconds, double fair, double good, double excellent)
{
    const double ms = (double)microseconds / 1000.0;
    if (ms <= excellent) {
        return 4;
    }
    if (ms <= good) {
        return 3;
    }
    if (ms <= fair) {
        return 2;
    }
    return 1;
}

lp_speedtest_quality_t lp_speedtest_quality(const lp_speedtest_progress_t *progress)
{
    if (progress == NULL) {
        return LP_SPEEDTEST_QUALITY_UNKNOWN;
    }

    int tier = 4;
    bool measured = false;

    if (progress->has_download) {
        const int candidate = rate_tier(progress->download_bytes_per_sec, 5.0, 25.0, 100.0);
        tier = candidate < tier ? candidate : tier;
        measured = true;
    }
    if (progress->has_upload) {
        const int candidate = rate_tier(progress->upload_bytes_per_sec, 1.0, 5.0, 20.0);
        tier = candidate < tier ? candidate : tier;
        measured = true;
    }
    if (progress->has_latency) {
        /* Jitter is graded too: a low median hides an unusable call quality. */
        const int latency = delay_tier(progress->latency_us, 150.0, 60.0, 30.0);
        const int jitter = delay_tier(progress->jitter_us, 60.0, 30.0, 10.0);
        const int candidate = latency < jitter ? latency : jitter;
        tier = candidate < tier ? candidate : tier;
        measured = true;
    }

    if (!measured) {
        return LP_SPEEDTEST_QUALITY_UNKNOWN;
    }
    return (lp_speedtest_quality_t)tier;
}

const char *lp_speedtest_quality_str(lp_speedtest_quality_t quality)
{
    switch (quality) {
    case LP_SPEEDTEST_QUALITY_POOR:
        return "Poor";
    case LP_SPEEDTEST_QUALITY_FAIR:
        return "Fair";
    case LP_SPEEDTEST_QUALITY_GOOD:
        return "Good";
    case LP_SPEEDTEST_QUALITY_EXCELLENT:
        return "Excellent";
    case LP_SPEEDTEST_QUALITY_UNKNOWN:
    default:
        return "Unknown";
    }
}

const char *lp_speedtest_quality_hint(lp_speedtest_quality_t quality)
{
    switch (quality) {
    case LP_SPEEDTEST_QUALITY_POOR:
        return "Basic browsing only. Video calls and streaming will struggle.";
    case LP_SPEEDTEST_QUALITY_FAIR:
        return "Fine for browsing and SD video. Calls may stutter under load.";
    case LP_SPEEDTEST_QUALITY_GOOD:
        return "Comfortable for HD streaming, video calls and gaming.";
    case LP_SPEEDTEST_QUALITY_EXCELLENT:
        return "Handles 4K streaming, large downloads and calls at once.";
    case LP_SPEEDTEST_QUALITY_UNKNOWN:
    default:
        return "Run a test to grade this connection.";
    }
}
