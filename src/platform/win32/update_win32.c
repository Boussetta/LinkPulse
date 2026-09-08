#include "linkpulse/update.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <winhttp.h>

#define LP_RELEASE_HOST L"api.github.com"
#define LP_RELEASE_PATH L"/repos/Boussetta/LinkPulse/releases/latest"

static bool parse_version(const char *text, unsigned long parts[3])
{
    if (text == NULL) {
        return false;
    }
    if (text[0] == 'v' || text[0] == 'V') {
        ++text;
    }

    for (size_t i = 0; i < 3; ++i) {
        char *end = NULL;
        parts[i] = strtoul(text, &end, 10);
        if (end == text || (i < 2 && *end != '.') || (i == 2 && *end != '\0')) {
            return false;
        }
        text = end + (i < 2 ? 1 : 0);
    }
    return true;
}

static bool version_is_newer(const char *current, const char *candidate)
{
    unsigned long current_parts[3];
    unsigned long candidate_parts[3];
    if (!parse_version(current, current_parts) || !parse_version(candidate, candidate_parts)) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (candidate_parts[i] != current_parts[i]) {
            return candidate_parts[i] > current_parts[i];
        }
    }
    return false;
}

static lp_status_t extract_tag(const char *response, char *out, size_t cap)
{
    const char *key = strstr(response, "\"tag_name\"");
    if (key == NULL) {
        return LP_ERR_NOT_FOUND;
    }
    const char *value = strchr(key, ':');
    if (value == NULL) {
        return LP_ERR_IO;
    }
    value = strchr(value, '"');
    if (value == NULL) {
        return LP_ERR_IO;
    }
    ++value;
    const char *end = strchr(value, '"');
    if (end == NULL || end == value) {
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

lp_status_t lp_update_check_latest(const char *current_version, char *latest_version,
                                   size_t latest_cap)
{
    if (current_version == NULL || latest_version == NULL || latest_cap == 0) {
        return LP_ERR_INVALID_ARG;
    }
    latest_version[0] = '\0';

    HINTERNET session = WinHttpOpen(L"LinkPulse", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        return LP_ERR_IO;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    HINTERNET connection = WinHttpConnect(session, LP_RELEASE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == NULL) {
        WinHttpCloseHandle(session);
        return LP_ERR_IO;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", LP_RELEASE_PATH, NULL,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (request == NULL) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return LP_ERR_IO;
    }

    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
    const BOOL sent = WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0,
                                         0, 0);
    const BOOL received = sent && WinHttpReceiveResponse(request, NULL);
    if (!received) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return LP_ERR_IO;
    }

    char response[8192];
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
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return LP_ERR_IO;
        }
        response_length += read;
        if (read == 0 || response_length == sizeof(response) - 1) {
            break;
        }
    }
    response[response_length] = '\0';

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    lp_status_t status = extract_tag(response, latest_version, latest_cap);
    if (status != LP_OK) {
        return status;
    }
    if (!version_is_newer(current_version, latest_version)) {
        latest_version[0] = '\0';
        return LP_ERR_NOT_FOUND;
    }
    return LP_OK;
}
