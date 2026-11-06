#pragma once
#include <stdint.h>

/* Transparent padding includes the downward shadow around the 46 x 28 cap. */
#define COVER_FLOW_CAP_WIDTH 54
#define COVER_FLOW_CAP_HEIGHT 40
void cover_flow_cap_render(uint8_t * pixels, int width, int height, int stride);
