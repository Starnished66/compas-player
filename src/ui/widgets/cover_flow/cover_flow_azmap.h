#pragma once
/* The A-Z jump table of a cover flow's items. Pure, so it tests on the host.
 * Mirrors metadata_db_get_az_table(): a letter is the first byte of the
 * title, ASCII case-folded, and '#' is anything that is not a-z. */
#include "cover_flow.h"

#define COVER_FLOW_AZ_LETTERS 27 /* A..Z, then '#' */

/* Fills `jump[27]` with the first item of each letter, or of the next letter
 * that has one (A..Z only; '#' never borrows), or -1 when none is left. Fills
 * `has[27]` with whether the letter itself has an item. The items need not be
 * sorted. */
void cover_flow_az_build(const cover_flow_item_t * items, int count, int jump[COVER_FLOW_AZ_LETTERS],
                         bool has[COVER_FLOW_AZ_LETTERS]);

/* Linear track mapping, in the carousel's 16.16 item position. */
int32_t cover_flow_scrub_pos(int32_t x, int32_t left, int32_t width, int count);
int32_t cover_flow_scrub_x(int32_t pos, int32_t left, int32_t width, int count);
/* -1 for an absent letter, otherwise the first album's track coordinate. */
int32_t cover_flow_scrub_notch(int letter, const int * jump, const bool * has,
                              int32_t left, int32_t width, int count);
