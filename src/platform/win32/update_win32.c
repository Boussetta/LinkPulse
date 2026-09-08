#include "linkpulse/update.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <winhttp.h>

#define LP_RELEASE_HOST L"api.github.com"
#define LP_RELEASE_PATH L"/repos/Boussetta/LinkPulse/releases/latest"
#define LP_INSTALLER_NAME "LinkPulseSetup.exe"

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

lp_status_t lp_update_download_latest(char *installer_path, size_t path_cap)
{
    if (installer_path == NULL || path_cap == 0) {
        return LP_ERR_INVALID_ARG;
    }
    installer_path[0] = '\0';

    char latest_version[32];
    const lp_status_t check = lp_update_check_latest("0.0.0", latest_version,
                                                      sizeof(latest_version));
    if (check != LP_OK) {
        return check;
    }

    wchar_t release_path[256];
    const int path_length = swprintf(
        release_path, sizeof(release_path) / sizeof(release_path[0]),
        L"/Boussetta/LinkPulse/releases/download/%hs/%hs", latest_version, LP_INSTALLER_NAME);
    if (path_length < 0 || (size_t)path_length >= sizeof(release_path) / sizeof(release_path[0])) {
        return LP_ERR_IO;
    }

    if (path_cap < MAX_PATH) {
        return LP_ERR_INVALID_ARG;
    }
    char temp_directory[MAX_PATH];
    const DWORD temp_length = GetTempPathA(sizeof(temp_directory), temp_directory);
    char temp_file[MAX_PATH];
    if (temp_length == 0 || temp_length >= sizeof(temp_directory) ||
        GetTempFileNameA(temp_directory, "LP", 0, temp_file) == 0) {
        return LP_ERR_IO;
    }
    DeleteFileA(temp_file);
    char *dot = strrchr(temp_file, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    const int installer_written = snprintf(installer_path, path_cap, "%s.exe", temp_file);
    if (installer_written < 0 || (size_t)installer_written >= path_cap) {
        return LP_ERR_IO;
    }
    HANDLE output = CreateFileA(installer_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        DeleteFileA(installer_path);
        installer_path[0] = '\0';
        return LP_ERR_IO;
    }

    HINTERNET session = WinHttpOpen(L"LinkPulse", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    bool success = false;
    if (session != NULL) {
        WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
        connection = WinHttpConnect(session, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (connection != NULL) {
        request = WinHttpOpenRequest(connection, L"GET", release_path, NULL,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    }
    if (request != NULL && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL)) {
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                                WINHTTP_NO_HEADER_INDEX) &&
            status_code == 200) {
            success = true;
            for (;;) {
                BYTE buffer[8192];
                DWORD bytes_read = 0;
                if (!WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read)) {
                    success = false;
                    break;
                }
                if (bytes_read == 0) {
                    break;
                }
                DWORD bytes_written = 0;
                if (!WriteFile(output, buffer, bytes_read, &bytes_written, NULL) ||
                    bytes_written != bytes_read) {
                    success = false;
                    break;
                }
            }
        }
    }

    CloseHandle(output);
    close_http_handles(request, connection, session);
    if (!success) {
        DeleteFileA(installer_path);
        installer_path[0] = '\0';
        return LP_ERR_IO;
    }
    return LP_OK;
}
