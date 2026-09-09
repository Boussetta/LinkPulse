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

/* Strips the leading "ASxxxxx " announcing-AS prefix from an "org" field,
   copying the removed prefix into asn_out (without the trailing space). */
static void split_org_field(const char *org, char *isp_out, size_t isp_cap, char *asn_out,
                            size_t asn_cap)
{
    asn_out[0] = '\0';
    if (org[0] == 'A' && org[1] == 'S') {
        const char *cursor = org + 2;
        while (*cursor >= '0' && *cursor <= '9') {
            ++cursor;
        }
        if (cursor != org + 2 && *cursor == ' ') {
            const size_t asn_length = (size_t)(cursor - org);
            if (asn_length < asn_cap) {
                memcpy(asn_out, org, asn_length);
                asn_out[asn_length] = '\0';
            }
            snprintf(isp_out, isp_cap, "%s", cursor + 1);
            return;
        }
    }
    snprintf(isp_out, isp_cap, "%s", org);
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

/* Queries every available public IP/ISP/location field over HTTPS; a failed
   or missing field is simply left empty rather than failing the whole lookup. */
lp_status_t lp_isp_lookup(lp_isp_info_t *out)
{
    if (out == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

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

    lp_status_t any_field_status = LP_ERR_NOT_FOUND;
    char org[LP_ISP_NAME_MAX + LP_ISP_ASN_MAX];

    if (extract_field(response, "ip", out->public_ip, sizeof(out->public_ip)) == LP_OK)
        any_field_status = LP_OK;
    if (extract_field(response, "org", org, sizeof(org)) == LP_OK) {
        any_field_status = LP_OK;
        split_org_field(org, out->isp, sizeof(out->isp), out->asn, sizeof(out->asn));
    }
    if (extract_field(response, "hostname", out->hostname, sizeof(out->hostname)) == LP_OK)
        any_field_status = LP_OK;
    if (extract_field(response, "city", out->city, sizeof(out->city)) == LP_OK)
        any_field_status = LP_OK;
    if (extract_field(response, "region", out->region, sizeof(out->region)) == LP_OK)
        any_field_status = LP_OK;
    if (extract_field(response, "country", out->country, sizeof(out->country)) == LP_OK)
        any_field_status = LP_OK;
    if (extract_field(response, "postal", out->postal, sizeof(out->postal)) == LP_OK)
        any_field_status = LP_OK;
    if (extract_field(response, "timezone", out->timezone, sizeof(out->timezone)) == LP_OK)
        any_field_status = LP_OK;
    if (extract_field(response, "loc", out->loc, sizeof(out->loc)) == LP_OK)
        any_field_status = LP_OK;

    return any_field_status;
}

