#pragma once
/* The scrub track's linear mapping. Pure, so it tests on the host. */
#include "cover_flow.h"

/* Linear track mapping, in the carousel's 16.16 item position. */
int32_t cover_flow_scrub_pos(int32_t x, int32_t left, int32_t width, int count);
int32_t cover_flow_scrub_x(int32_t pos, int32_t left, int32_t width, int count);
