#include "lp_test.h"

#include "linkpulse/log.h"
#include "linkpulse/net.h"
#include "linkpulse/status.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
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

static void test_status_strings(void)
{
    LP_CHECK_STR_EQ(lp_status_str(LP_OK), "ok");
    LP_CHECK_STR_EQ(lp_status_str(LP_ERR_NOT_FOUND), "not found");
    LP_CHECK_STR_EQ(lp_status_str((lp_status_t)999), "unknown error");
}

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

static void test_iface_list_free_is_idempotent(void)
{
    lp_iface_list_t list = {NULL, 0};
    lp_iface_list_free(&list);
    lp_iface_list_free(&list);
    lp_iface_list_free(NULL);
    LP_CHECK(list.items == NULL);
    LP_CHECK(list.count == 0);
}

static void read_stream(FILE *stream, char *buffer, size_t buffer_size)
{
    const size_t bytes_read = fread(buffer, 1, buffer_size - 1, stream);
    buffer[bytes_read] = '\0';
}

static void with_captured_stderr(FILE *capture, void (*fn)(void))
{
    const int stderr_fd = LP_FILENO(stderr);
    const int saved_stderr = LP_DUP(stderr_fd);
    LP_CHECK(saved_stderr >= 0);
    if (saved_stderr < 0) {
        return;
    }

    fflush(stderr);
    const int redirect_status = LP_DUP2(LP_FILENO(capture), stderr_fd);
    LP_CHECK(redirect_status >= 0);
    if (redirect_status < 0) {
        LP_CLOSE(saved_stderr);
        return;
    }

    fn();
    fflush(stderr);

    LP_CHECK(LP_DUP2(saved_stderr, stderr_fd) >= 0);
    LP_CLOSE(saved_stderr);
}

static void log_hidden_debug_message(void)
{
    lp_log_set_level(LP_LOG_WARN);
    LP_CHECK(lp_log_get_level() == LP_LOG_WARN);
    LP_DEBUG("hidden debug message");
}

static void log_visible_info_message(void)
{
    lp_log_set_level(LP_LOG_INFO);
    LP_CHECK(lp_log_get_level() == LP_LOG_INFO);
    LP_INFO("visible info message");
}

static void test_log_level_and_filtering(void)
{
    FILE *hidden_capture = tmpfile();
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

    FILE *visible_capture = tmpfile();
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
}

int main(void)
{
    test_status_strings();
    test_iface_list_find();
    test_iface_list_free_is_idempotent();
    test_log_level_and_filtering();
    LP_TEST_RETURN();
}
