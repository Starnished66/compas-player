#include "../cover_flow_azmap.h"
#include "../cover_flow_span.h"
#include <assert.h>
#include <stdio.h>

static void check_mapping(void) {
    assert(cover_flow_scrub_pos(-10, 20, 400, 5) == 0);
    assert(cover_flow_scrub_pos(500, 20, 400, 5) == 4 * COVER_FLOW_ONE);
    assert(cover_flow_scrub_pos(170, 20, 400, 5) == 3 * COVER_FLOW_ONE / 2);
    assert(cover_flow_scrub_x(3 * COVER_FLOW_ONE / 2, 20, 400, 5) == 170);
    assert(cover_flow_scrub_x(-1, 20, 400, 5) == 20);
    assert(cover_flow_scrub_x(9 * COVER_FLOW_ONE, 20, 400, 5) == 420);
    for (int n = 0; n <= 1; n++) {
        assert(cover_flow_scrub_pos(300, 20, 400, n) == 0);
        assert(cover_flow_scrub_x(100, 20, 400, n) == 20);
    }
    assert(cover_flow_scrub_pos(30, 20, 0, 5) == 0);
    assert(cover_flow_scrub_x(100, 20, 0, 5) == 20);
    for (int x = 20; x <= 420; x++) {
        int32_t pos = cover_flow_scrub_pos(x, 20, 400, COVER_FLOW_MAX_ITEMS);
        assert(cover_flow_scrub_x(pos, 20, 400, COVER_FLOW_MAX_ITEMS) == x);
    }
}

int main(void) {
    check_mapping();
    puts("cover_flow_azmap: PASS");
    return 0;
}
