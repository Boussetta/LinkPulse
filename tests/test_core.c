#include "lp_test.h"

#include "linkpulse/log.h"
#include "linkpulse/net.h"
#include "linkpulse/status.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#define LP_DUP _dup
#define LP_DUP2 _dup2
#define LP_FILENO _fileno
#define LP_CLOSE _close
#else
#include <unistd.h>
#define LP_DUP dup
#define LP_DUP2 dup2
#define LP_FILENO fileno
#define LP_CLOSE close
#endif

static atomic_flag g_stderr_capture_lock = ATOMIC_FLAG_INIT;

/* tmpfile() fails on Windows without admin rights: the MSVCRT implementation
   tries to create the file in the root of the current drive. */
/* Creates a temporary capture file safely on both MSVCRT and POSIX runtimes. */
static FILE *lp_tmpfile(void)
{
#if defined(_WIN32)
    char dir[MAX_PATH];
    char path[MAX_PATH];
    if (GetTempPathA(sizeof(dir), dir) == 0) {
        return NULL;
    }
    if (GetTempFileNameA(dir, "lpt", 0, path) == 0) {
        return NULL;
    }
    FILE *file = fopen(path, "w+bTD"); /* _O_TEMPORARY: deleted on close */
    if (file == NULL) {
        DeleteFileA(path);
    }
    return file;
#else
    return tmpfile();
#endif
}

/* Verifies known and unknown status codes have stable diagnostics. */
static void test_status_strings(void)
{
    LP_CHECK_STR_EQ(lp_status_str(LP_OK), "ok");
    LP_CHECK_STR_EQ(lp_status_str(LP_ERR_NOT_FOUND), "not found");
    LP_CHECK_STR_EQ(lp_status_str((lp_status_t)999), "unknown error");
}

/* Verifies interface lookup handles hits, misses, and invalid arguments. */
static void test_iface_list_find(void)
{
    lp_iface_t items[2] = {0};
    snprintf(items[0].name, sizeof(items[0].name), "Ethernet");
    snprintf(items[1].name, sizeof(items[1].name), "Wi-Fi");
    items[1].rx_bytes = 4242;

    const lp_iface_list_t list = {items, 2};

    const lp_iface_t *found = lp_iface_list_find(&list, "Wi-Fi");
    LP_CHECK(found != NULL);
    LP_CHECK(found != NULL && found->rx_bytes == 4242);
    LP_CHECK(lp_iface_list_find(&list, "nope") == NULL);
    LP_CHECK(lp_iface_list_find(&list, NULL) == NULL);
    LP_CHECK(lp_iface_list_find(NULL, "Wi-Fi") == NULL);
}

/* Verifies snapshot cleanup is safe when repeated or passed NULL. */
static void test_iface_list_free_is_idempotent(void)
{
    lp_iface_list_t list = {NULL, 0};
    lp_iface_list_free(&list);
    lp_iface_list_free(&list);
    lp_iface_list_free(NULL);
    LP_CHECK(list.items == NULL);
    LP_CHECK(list.count == 0);
}

/* Reads a capture stream into a bounded NUL-terminated test string. */
static void read_stream(FILE *stream, char *buffer, size_t buffer_size)
{
    const size_t bytes_read = fread(buffer, 1, buffer_size - 1, stream);
    buffer[bytes_read] = '\0';
}

/* Redirects stderr around one logging scenario while serializing test captures. */
static void with_captured_stderr(FILE *capture, void (*fn)(void))
{
    while (atomic_flag_test_and_set_explicit(&g_stderr_capture_lock, memory_order_acquire)) {
    }

    const int stderr_fd = LP_FILENO(stderr);
    const int saved_stderr = LP_DUP(stderr_fd);
    LP_CHECK(saved_stderr >= 0);
    if (saved_stderr < 0) {
        atomic_flag_clear_explicit(&g_stderr_capture_lock, memory_order_release);
        return;
    }

    fflush(stderr);
    const int redirect_status = LP_DUP2(LP_FILENO(capture), stderr_fd);
    LP_CHECK(redirect_status >= 0);
    if (redirect_status < 0) {
        LP_CLOSE(saved_stderr);
        atomic_flag_clear_explicit(&g_stderr_capture_lock, memory_order_release);
        return;
    }

    fn();
    fflush(stderr);

    const int restore_status = LP_DUP2(saved_stderr, stderr_fd);
    LP_CLOSE(saved_stderr);
    atomic_flag_clear_explicit(&g_stderr_capture_lock, memory_order_release);
    if (restore_status < 0) {
        fputs("FAIL failed to restore stderr\n", stdout);
        exit(EXIT_FAILURE);
    }
}

/* Emits a debug message below the active warning threshold. */
static void log_hidden_debug_message(void)
{
    lp_log_set_level(LP_LOG_WARN);
    LP_CHECK(lp_log_get_level() == LP_LOG_WARN);
    LP_DEBUG("hidden debug message");
}

/* Emits an info message at the active threshold. */
static void log_visible_info_message(void)
{
    lp_log_set_level(LP_LOG_INFO);
    LP_CHECK(lp_log_get_level() == LP_LOG_INFO);
    LP_INFO("visible info message");
}

/* Sends one record to the optional file sink so both output paths are covered. */
static void log_file_message(void)
{
    lp_log_set_level(LP_LOG_INFO);
    LP_INFO("file info message");
}

/* Verifies severity filtering and visible log formatting. */
static void test_log_level_and_filtering(void)
{
    FILE *hidden_capture = lp_tmpfile();
    LP_CHECK(hidden_capture != NULL);
    if (hidden_capture == NULL) {
        return;
    }

    with_captured_stderr(hidden_capture, log_hidden_debug_message);

    rewind(hidden_capture);
    char buffer[256];
    read_stream(hidden_capture, buffer, sizeof(buffer));
    LP_CHECK_STR_EQ(buffer, "");
    fclose(hidden_capture);

    FILE *visible_capture = lp_tmpfile();
    LP_CHECK(visible_capture != NULL);
    if (visible_capture == NULL) {
        return;
    }

    with_captured_stderr(visible_capture, log_visible_info_message);

    rewind(visible_capture);
    read_stream(visible_capture, buffer, sizeof(buffer));
    LP_CHECK(strstr(buffer, "INFO ") != NULL);
    LP_CHECK(strstr(buffer, "visible info message") != NULL);

    fclose(visible_capture);

    FILE *file_capture = lp_tmpfile();
    LP_CHECK(file_capture != NULL);
    if (file_capture == NULL) {
        return;
    }

    lp_log_set_file(file_capture);
    with_captured_stderr(file_capture, log_file_message);
    lp_log_set_file(NULL);
    rewind(file_capture);
    read_stream(file_capture, buffer, sizeof(buffer));
    LP_CHECK(strstr(buffer, "INFO ") != NULL);
    LP_CHECK(strstr(buffer, "file info message") != NULL);
    LP_CHECK(buffer[0] == '[' && buffer[5] == '-');
    fclose(file_capture);
}

/* Runs status, network-container, and logger contract scenarios. */
int main(void)
{
    test_status_strings();
    test_iface_list_find();
    test_iface_list_free_is_idempotent();
    test_log_level_and_filtering();
    LP_TEST_RETURN();
}
