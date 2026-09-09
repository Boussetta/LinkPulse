#include "linkpulse/isp.h"
#include "linkpulse/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <winhttp.h>

#define LP_ISP_HOST L"ipinfo.io"
#define LP_ISP_PATH L"/json"

/* Extracts one bounded string field from the flat JSON lookup response. */
static lp_status_t extract_field(const char *response, const char *key, char *out, size_t cap)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *field = strstr(response, needle);
    if (field == NULL) {
        return LP_ERR_NOT_FOUND;
    }
    const char *value = strchr(field, ':');
    if (value == NULL) {
        return LP_ERR_IO;
    }
    value = strchr(value, '"');
    if (value == NULL) {
        return LP_ERR_IO;
    }
    ++value;
    const char *end = strchr(value, '"');
    if (end == NULL) {
        return LP_ERR_IO;
    }
    const size_t length = (size_t)(end - value);
    if (length >= cap) {
        return LP_ERR_NO_MEMORY;
    }
    memcpy(out, value, length);
    out[length] = '\0';
    return LP_OK;
}

/* Strips the leading "ASxxxxx " announcing-AS prefix from an "org" field. */
static void strip_as_prefix(char *value)
{
    if (value[0] != 'A' || value[1] != 'S') {
        return;
    }
    const char *cursor = value + 2;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
    }
    if (cursor == value + 2 || *cursor != ' ') {
        return;
    }
    memmove(value, cursor + 1, strlen(cursor + 1) + 1);
}

static void close_http_handles(HINTERNET request, HINTERNET connection, HINTERNET session)
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

/* Queries the public IP and organization name over HTTPS; failures are best-effort. */
lp_status_t lp_isp_lookup(char *isp_out, size_t isp_cap, char *ip_out, size_t ip_cap)
{
    if (isp_out == NULL || isp_cap == 0 || ip_out == NULL || ip_cap == 0) {
        return LP_ERR_INVALID_ARG;
    }
    isp_out[0] = '\0';
    ip_out[0] = '\0';

    HINTERNET session = WinHttpOpen(L"LinkPulse", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        return LP_ERR_IO;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    HINTERNET connection = WinHttpConnect(session, LP_ISP_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == NULL) {
        WinHttpCloseHandle(session);
        return LP_ERR_IO;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", LP_ISP_PATH, NULL,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (request == NULL) {
        close_http_handles(NULL, connection, session);
        return LP_ERR_IO;
    }

    const BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    const BOOL received = sent && WinHttpReceiveResponse(request, NULL);
    if (!received) {
        close_http_handles(request, connection, session);
        return LP_ERR_IO;
    }

    char response[4096];
    size_t response_length = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }
        if (available > sizeof(response) - response_length - 1) {
            available = (DWORD)(sizeof(response) - response_length - 1);
        }
        DWORD read = 0;
        if (!WinHttpReadData(request, response + response_length, available, &read)) {
            close_http_handles(request, connection, session);
            return LP_ERR_IO;
        }
        response_length += read;
        if (read == 0 || response_length == sizeof(response) - 1) {
            break;
        }
    }
    response[response_length] = '\0';
    close_http_handles(request, connection, session);

    const lp_status_t ip_status = extract_field(response, "ip", ip_out, ip_cap);
    const lp_status_t org_status = extract_field(response, "org", isp_out, isp_cap);
    if (org_status == LP_OK) {
        strip_as_prefix(isp_out);
    }
    if (ip_status != LP_OK && org_status != LP_OK) {
        return LP_ERR_NOT_FOUND;
    }
    return LP_OK;
}
