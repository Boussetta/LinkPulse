#include "linkpulse/update.h"
#include "linkpulse/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <shellapi.h>
#include <softpub.h>
#include <wintrust.h>
#include <winhttp.h>

#define LP_RELEASE_PAGE_URL L"https://github.com/Boussetta/LinkPulse/releases/latest"
#define LP_RELEASE_HOST L"api.github.com"
#define LP_RELEASE_PATH L"/repos/Boussetta/LinkPulse/releases/latest"
#define LP_INSTALLER_NAME "LinkPulseSetup.exe"

/* Parses the strict MAJOR.MINOR.PATCH form accepted by update comparisons. */
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

/* Compares semantic release components without treating lexical order as numeric order. */
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

/* Extracts the latest release tag from the bounded GitHub response payload. */
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

/* Closes any partially-created WinHTTP handle set during error unwinding. */
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

/* Queries GitHub over HTTPS and reports only releases newer than current_version. */
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

/* Resolves the latest installer asset, streams it to a temp file, and returns its path. */
lp_status_t lp_update_download_latest(char *installer_path, size_t path_cap)
{
    LP_INFO("starting update installer download");
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
    /* Wide APIs throughout: the ANSI variants mangle profile paths outside the code page. */
    wchar_t temp_directory[MAX_PATH];
    const DWORD temp_length = GetTempPathW(MAX_PATH, temp_directory);
    wchar_t temp_file[MAX_PATH];
    if (temp_length == 0 || temp_length >= MAX_PATH ||
        GetTempFileNameW(temp_directory, L"LP", 0, temp_file) == 0) {
        return LP_ERR_IO;
    }
    DeleteFileW(temp_file);
    wchar_t *dot = wcsrchr(temp_file, L'.');
    if (dot != NULL) {
        *dot = L'\0';
    }
    wchar_t wide_installer[MAX_PATH + 4];
    const int installer_written = swprintf(wide_installer,
                                           sizeof(wide_installer) / sizeof(wide_installer[0]),
                                           L"%ls.exe", temp_file);
    if (installer_written < 0 ||
        WideCharToMultiByte(CP_UTF8, 0, wide_installer, -1, installer_path, (int)path_cap, NULL,
                            NULL) == 0) {
        installer_path[0] = '\0';
        return LP_ERR_IO;
    }
    HANDLE output = CreateFileW(wide_installer, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        DeleteFileW(wide_installer);
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
        LP_ERROR("update installer download failed: HTTP status or file write error");
        DeleteFileW(wide_installer);
        installer_path[0] = '\0';
        return LP_ERR_IO;
    }
    LP_INFO("update installer download complete: %s", installer_path);
    return LP_OK;
}

/* Checks the file's embedded Authenticode signature against the machine's trusted roots,
   including revocation of every certificate in the chain. */
static bool installer_signature_is_trusted(const wchar_t *path)
{
    WINTRUST_FILE_INFO file_info;
    memset(&file_info, 0, sizeof(file_info));
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path;

    WINTRUST_DATA trust_data;
    memset(&trust_data, 0, sizeof(trust_data));
    trust_data.cbStruct = sizeof(trust_data);
    trust_data.dwUIChoice = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust_data.dwUnionChoice = WTD_CHOICE_FILE;
    trust_data.pFile = &file_info;
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &trust_data);
    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &trust_data);

    if (result != ERROR_SUCCESS) {
        LP_ERROR("update installer signature rejected: WinVerifyTrust=0x%08lx",
                 (unsigned long)result);
        return false;
    }
    return true;
}

/* Verifies, then starts, a downloaded installer; unverified files are never executed. */
lp_status_t lp_update_launch_installer(const char *installer_path)
{
    if (installer_path == NULL || installer_path[0] == '\0') {
        return LP_ERR_INVALID_ARG;
    }
    wchar_t path[MAX_PATH + 4];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, installer_path, -1, path,
                            (int)(sizeof(path) / sizeof(path[0]))) == 0) {
        return LP_ERR_INVALID_ARG;
    }

    if (!installer_signature_is_trusted(path)) {
        DeleteFileW(path);
        LP_WARN("opening the release page instead of running an unverified installer");
        ShellExecuteW(NULL, L"open", LP_RELEASE_PAGE_URL, NULL, NULL, SW_SHOWNORMAL);
        return LP_ERR_UNSUPPORTED;
    }
    if ((INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        DeleteFileW(path);
        LP_ERROR("update installer launch failed: %s", installer_path);
        return LP_ERR_IO;
    }
    LP_INFO("update installer launched: %s", installer_path);
    return LP_OK;
}
