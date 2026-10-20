/* Protocol-only checks: compile this file with remote_control_discovery.c. */
#include "remote_control_discovery.h"

#include <assert.h>
#include <string.h>

static void test_request_shape(void) {
    uint8_t request[REMOTE_CONTROL_DISCOVERY_REQUEST_SIZE] = {0};
    static const char prefix[] = "COMPAS_REMOTE_DISCOVER_V1";
    memcpy(request, prefix, sizeof(prefix) - 1);
    assert(remote_control_discovery_request_is_valid(request, sizeof(request)));
    assert(!remote_control_discovery_request_is_valid(request, sizeof(request) - 1));
    request[sizeof(prefix)] = 1;
    assert(!remote_control_discovery_request_is_valid(request, sizeof(request)));
    assert(!remote_control_discovery_request_is_valid(NULL, sizeof(request)));
}

static void test_reply_shape(void) {
    uint8_t reply[32] = {0};
    size_t size = remote_control_discovery_make_reply(reply, sizeof(reply));
    assert(size == 22);
    assert(memcmp(reply, "COMPAS_REMOTE_V1 8899\n", size) == 0);
    assert(remote_control_discovery_make_reply(reply, size - 1) == 0);
}

int main(void) {
    test_request_shape();
    test_reply_shape();
    return 0;
}
