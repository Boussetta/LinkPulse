#include "linkpulse/speedtest.h"

#include "linkpulse/clock.h"
#include "linkpulse/log.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <winhttp.h>

/* Cloudflare's public measurement endpoint: no API key, generous size limits,
   and anycast PoPs close enough that the link, not the server, is the
   bottleneck. /__down returns `bytes` of filler, /__up discards its body. */
#define LP_SPEEDTEST_HOST L"speed.cloudflare.com"
#define LP_SPEEDTEST_UPLOAD_PATH L"/__up"
#define LP_SPEEDTEST_LATENCY_PROBES 5
#define LP_SPEEDTEST_PHASE_NS (8ULL * 1000ULL * 1000ULL * 1000ULL)
/* Sized well under the endpoint's per-request cap (100 MB is rejected with
   403); the phase loop simply re-requests until the time budget is spent. */
#define LP_SPEEDTEST_DOWNLOAD_BYTES 26214400UL
#define LP_SPEEDTEST_UPLOAD_BYTES 10485760UL
#define LP_SPEEDTEST_CHUNK 65536
#define LP_SPEEDTEST_REPORT_INTERVAL_NS (100ULL * 1000ULL * 1000ULL)
/* A lossy link can drop a request outright; only give up after this many in a row. */
#define LP_SPEEDTEST_MAX_ATTEMPTS 3

/* Reads the caller's abort flag; plain volatile load, no ordering needed. */
static bool is_cancelled(const lp_speedtest_request_t *request)
{
    return request->cancel_flag != NULL && *request->cancel_flag != 0;
}

static void report(const lp_speedtest_request_t *request,
                   const lp_speedtest_progress_t *progress)
{
    if (request->on_progress != NULL) {
        request->on_progress(progress, request->user_data);
    }
}

static uint64_t rate_bytes_per_sec(uint64_t bytes, uint64_t elapsed_ns)
{
    if (elapsed_ns == 0) {
        return 0;
    }
    return bytes * 1000000000ULL / elapsed_ns;
}

static unsigned elapsed_percent(uint64_t elapsed_ns)
{
    if (elapsed_ns >= LP_SPEEDTEST_PHASE_NS) {
        return 100;
    }
    return (unsigned)(elapsed_ns * 100ULL / LP_SPEEDTEST_PHASE_NS);
}

static void close_handles(HINTERNET request, HINTERNET connection, HINTERNET session)
{
    if (request != NULL) {
        WinHttpCloseHandle(request);
    }
    if (connection != NULL) {
        WinHttpCloseHandle(connection);
    }
    if (session != NULL) {
        WinHttpCloseHandle(session);
    }
}

/* Opens one request and blocks until the response headers arrive. */
static HINTERNET open_response(HINTERNET connection, const wchar_t *verb, const wchar_t *path,
                               DWORD body_length)
{
    HINTERNET request = WinHttpOpenRequest(connection, verb, path, NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request == NULL) {
        return NULL;
    }
    const wchar_t headers[] = L"Cache-Control: no-cache, no-store\r\nPragma: no-cache\r\n";
    if (!WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, body_length,
                            0)) {
        WinHttpCloseHandle(request);
        return NULL;
    }
    if (body_length == 0 && !WinHttpReceiveResponse(request, NULL)) {
        WinHttpCloseHandle(request);
        return NULL;
    }
    return request;
}

static DWORD response_status_code(HINTERNET request)
{
    DWORD code = 0;
    DWORD size = sizeof(code);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return code;
}

/* Times request-to-headers round trips and reports their median and jitter. */
static lp_status_t measure_latency(HINTERNET connection, const lp_speedtest_request_t *request,
                                   lp_speedtest_progress_t *progress)
{
    uint64_t samples_us[LP_SPEEDTEST_LATENCY_PROBES];
    size_t sample_count = 0;

    progress->phase = LP_SPEEDTEST_PHASE_LATENCY;
    progress->phase_percent = 0;
    report(request, progress);

    for (size_t i = 0; i < LP_SPEEDTEST_LATENCY_PROBES; ++i) {
        if (is_cancelled(request)) {
            return LP_ERR_IO;
        }
        const uint64_t started_ns = lp_clock_monotonic_ns();
        HINTERNET probe = open_response(connection, L"GET", L"/__down?bytes=0", 0);
        if (probe == NULL) {
            continue;
        }
        /* Drain the (empty) body so the connection stays reusable. */
        for (;;) {
            char sink[256];
            DWORD read = 0;
            if (!WinHttpReadData(probe, sink, sizeof(sink), &read) || read == 0) {
                break;
            }
        }
        const uint64_t elapsed_ns = lp_clock_monotonic_ns() - started_ns;
        WinHttpCloseHandle(probe);
        samples_us[sample_count++] = elapsed_ns / 1000ULL;

        progress->phase_percent = (unsigned)((i + 1) * 100 / LP_SPEEDTEST_LATENCY_PROBES);
        report(request, progress);
    }

    if (sample_count == 0) {
        return LP_ERR_IO;
    }

    uint64_t jitter_sum_us = 0;
    for (size_t i = 1; i < sample_count; ++i) {
        const uint64_t previous = samples_us[i - 1];
        const uint64_t current = samples_us[i];
        jitter_sum_us += current > previous ? current - previous : previous - current;
    }

    for (size_t i = 1; i < sample_count; ++i) {
        const uint64_t key = samples_us[i];
        size_t j = i;
        while (j > 0 && samples_us[j - 1] > key) {
            samples_us[j] = samples_us[j - 1];
            --j;
        }
        samples_us[j] = key;
    }

    progress->latency_us = samples_us[sample_count / 2];
    progress->jitter_us = sample_count > 1 ? jitter_sum_us / (sample_count - 1) : 0;
    progress->has_latency = true;
    report(request, progress);
    return LP_OK;
}

/* Streams filler payloads until the time cap, re-requesting if one runs out. */
static lp_status_t measure_download(HINTERNET connection, const lp_speedtest_request_t *request,
                                    lp_speedtest_progress_t *progress)
{
    wchar_t path[64];
    if (swprintf(path, sizeof(path) / sizeof(path[0]), L"/__down?bytes=%lu",
                 (unsigned long)LP_SPEEDTEST_DOWNLOAD_BYTES) < 0) {
        return LP_ERR_IO;
    }

    progress->phase = LP_SPEEDTEST_PHASE_DOWNLOAD;
    progress->phase_percent = 0;
    progress->live_bytes_per_sec = 0;
    report(request, progress);

    char buffer[LP_SPEEDTEST_CHUNK];
    uint64_t total_bytes = 0;
    uint64_t started_ns = 0;
    uint64_t last_report_ns = 0;
    unsigned failures = 0;
    bool stop = false;

    while (!stop && !is_cancelled(request) && failures < LP_SPEEDTEST_MAX_ATTEMPTS) {
        HINTERNET response = open_response(connection, L"GET", path, 0);
        if (response == NULL) {
            LP_WARN("speed test download request failed: error=%lu", GetLastError());
            ++failures;
            continue;
        }
        const DWORD http_status = response_status_code(response);
        if (http_status != 200) {
            /* A rejection is not transient, so there is nothing to retry. */
            LP_WARN("speed test download rejected: http_status=%lu", http_status);
            WinHttpCloseHandle(response);
            break;
        }

        uint64_t body_bytes = 0;
        bool read_failed = false;
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(response, buffer, (DWORD)sizeof(buffer), &read)) {
                LP_WARN("speed test download read failed: error=%lu", GetLastError());
                read_failed = true;
                break;
            }
            if (read == 0) {
                break; /* body complete; the outer loop asks for another one */
            }
            const uint64_t now_ns = lp_clock_monotonic_ns();
            if (started_ns == 0) {
                started_ns = now_ns;
                last_report_ns = now_ns;
            }
            body_bytes += read;
            total_bytes += read;
            const uint64_t elapsed_ns = now_ns - started_ns;
            if (now_ns - last_report_ns >= LP_SPEEDTEST_REPORT_INTERVAL_NS) {
                last_report_ns = now_ns;
                progress->live_bytes_per_sec = rate_bytes_per_sec(total_bytes, elapsed_ns);
                progress->phase_percent = elapsed_percent(elapsed_ns);
                report(request, progress);
            }
            if (elapsed_ns >= LP_SPEEDTEST_PHASE_NS) {
                stop = true;
                break;
            }
        }
        WinHttpCloseHandle(response);

        if (body_bytes == 0) {
            LP_WARN("speed test download body was empty");
            ++failures;
            continue;
        }
        if (read_failed && total_bytes > 0) {
            break; /* keep what was measured rather than restarting the phase */
        }
        failures = 0;
    }

    const uint64_t elapsed_ns = started_ns == 0 ? 0 : lp_clock_monotonic_ns() - started_ns;
    if (total_bytes == 0 || elapsed_ns == 0) {
        LP_WARN("speed test download produced no samples: bytes=%llu",
                (unsigned long long)total_bytes);
        return LP_ERR_IO;
    }

    progress->download_bytes_per_sec = rate_bytes_per_sec(total_bytes, elapsed_ns);
    progress->has_download = true;
    progress->live_bytes_per_sec = progress->download_bytes_per_sec;
    progress->phase_percent = 100;
    report(request, progress);
    return LP_OK;
}

/* Pushes filler upstream until the time cap, timing accepted bytes only. */
static lp_status_t measure_upload(HINTERNET connection, const lp_speedtest_request_t *request,
                                  lp_speedtest_progress_t *progress)
{
    progress->phase = LP_SPEEDTEST_PHASE_UPLOAD;
    progress->phase_percent = 0;
    progress->live_bytes_per_sec = 0;
    report(request, progress);

    char buffer[LP_SPEEDTEST_CHUNK];
    for (size_t i = 0; i < sizeof(buffer); ++i) {
        buffer[i] = (char)(i & 0x7F);
    }

    uint64_t total_bytes = 0;
    const uint64_t started_ns = lp_clock_monotonic_ns();
    uint64_t last_report_ns = started_ns;
    unsigned failures = 0;
    bool stop = false;

    while (!stop && !is_cancelled(request) && failures < LP_SPEEDTEST_MAX_ATTEMPTS) {
        HINTERNET response =
            open_response(connection, L"POST", LP_SPEEDTEST_UPLOAD_PATH, LP_SPEEDTEST_UPLOAD_BYTES);
        if (response == NULL) {
            LP_WARN("speed test upload request failed: error=%lu", GetLastError());
            ++failures;
            continue;
        }

        uint64_t remaining = LP_SPEEDTEST_UPLOAD_BYTES;
        bool write_failed = false;
        while (remaining > 0) {
            const DWORD chunk = remaining < sizeof(buffer) ? (DWORD)remaining
                                                           : (DWORD)sizeof(buffer);
            DWORD written = 0;
            if (!WinHttpWriteData(response, buffer, chunk, &written) || written == 0) {
                LP_WARN("speed test upload write failed: error=%lu", GetLastError());
                write_failed = true;
                break;
            }
            total_bytes += written;
            remaining -= written;

            const uint64_t now_ns = lp_clock_monotonic_ns();
            const uint64_t elapsed_ns = now_ns - started_ns;
            if (now_ns - last_report_ns >= LP_SPEEDTEST_REPORT_INTERVAL_NS) {
                last_report_ns = now_ns;
                progress->live_bytes_per_sec = rate_bytes_per_sec(total_bytes, elapsed_ns);
                progress->phase_percent = elapsed_percent(elapsed_ns);
                report(request, progress);
            }
            if (elapsed_ns >= LP_SPEEDTEST_PHASE_NS) {
                stop = true;
                break;
            }
        }

        if (remaining == 0 && WinHttpReceiveResponse(response, NULL)) {
            const DWORD http_status = response_status_code(response);
            if (http_status != 200) {
                LP_WARN("speed test upload rejected: http_status=%lu", http_status);
                WinHttpCloseHandle(response);
                break;
            }
        }
        /* A request cut short by the time cap never gets a complete reply, so
           the handle is closed without reading one. */
        WinHttpCloseHandle(response);

        if (write_failed) {
            if (total_bytes > 0) {
                break; /* keep what was measured rather than restarting the phase */
            }
            ++failures;
            continue;
        }
        failures = 0;
    }

    const uint64_t elapsed_ns = lp_clock_monotonic_ns() - started_ns;
    if (total_bytes == 0 || elapsed_ns == 0) {
        LP_WARN("speed test upload produced no samples");
        return LP_ERR_IO;
    }

    progress->upload_bytes_per_sec = rate_bytes_per_sec(total_bytes, elapsed_ns);
    progress->has_upload = true;
    progress->live_bytes_per_sec = progress->upload_bytes_per_sec;
    progress->phase_percent = 100;
    report(request, progress);
    return LP_OK;
}

lp_status_t lp_speedtest_run(const lp_speedtest_request_t *request, lp_speedtest_progress_t *out)
{
    if (request == NULL || out == NULL) {
        return LP_ERR_INVALID_ARG;
    }

    lp_speedtest_progress_t progress;
    memset(&progress, 0, sizeof(progress));
    progress.status = LP_OK;

    HINTERNET session = WinHttpOpen(L"LinkPulse", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        progress.phase = LP_SPEEDTEST_PHASE_FAILED;
        progress.status = LP_ERR_IO;
        report(request, &progress);
        *out = progress;
        return LP_ERR_IO;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 20000);

    HINTERNET connection =
        WinHttpConnect(session, LP_SPEEDTEST_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == NULL) {
        close_handles(NULL, NULL, session);
        progress.phase = LP_SPEEDTEST_PHASE_FAILED;
        progress.status = LP_ERR_IO;
        report(request, &progress);
        *out = progress;
        return LP_ERR_IO;
    }

    /* Each phase runs independently so one failure still reports the others. */
    (void)measure_latency(connection, request, &progress);
    if (!is_cancelled(request)) {
        (void)measure_download(connection, request, &progress);
    }
    if (!is_cancelled(request)) {
        (void)measure_upload(connection, request, &progress);
    }
    close_handles(NULL, connection, session);

    if (is_cancelled(request)) {
        progress.phase = LP_SPEEDTEST_PHASE_CANCELLED;
        progress.status = LP_OK;
    } else if (progress.has_download || progress.has_upload || progress.has_latency) {
        progress.phase = LP_SPEEDTEST_PHASE_DONE;
        progress.status = LP_OK;
        LP_INFO("speed test complete: down=%llu B/s up=%llu B/s latency=%llu us",
                (unsigned long long)progress.download_bytes_per_sec,
                (unsigned long long)progress.upload_bytes_per_sec,
                (unsigned long long)progress.latency_us);
    } else {
        progress.phase = LP_SPEEDTEST_PHASE_FAILED;
        progress.status = LP_ERR_IO;
        LP_WARN("speed test failed: no phase produced a result");
    }
    progress.live_bytes_per_sec = 0;
    report(request, &progress);
    *out = progress;
    return progress.status;
}
