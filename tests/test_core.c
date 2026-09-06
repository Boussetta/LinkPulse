#include "lp_test.h"

#include "linkpulse/net.h"
#include "linkpulse/status.h"

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

int main(void)
{
    test_status_strings();
    test_iface_list_find();
    test_iface_list_free_is_idempotent();
    LP_TEST_RETURN();
}
