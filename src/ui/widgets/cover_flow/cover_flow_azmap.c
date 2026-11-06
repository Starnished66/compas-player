#include "cover_flow_azmap.h"
#include "cover_flow_span.h"

int32_t cover_flow_scrub_pos(int32_t x, int32_t left, int32_t width, int count) {
    if (count <= 1 || width <= 0 || x <= left) return 0;
    int64_t max = (int64_t) (count - 1) * COVER_FLOW_ONE;
    if ((int64_t) x >= (int64_t) left + width) return (int32_t) max;
    return (int32_t) (((int64_t) x - left) * max / width);
}

int32_t cover_flow_scrub_x(int32_t pos, int32_t left, int32_t width, int count) {
    if (count <= 1 || width <= 0 || pos <= 0) return left;
    int64_t max = (int64_t) (count - 1) * COVER_FLOW_ONE;
    if (pos >= max) return left + width;
    return left + (int32_t) (((int64_t) pos * width + max / 2) / max);
}
