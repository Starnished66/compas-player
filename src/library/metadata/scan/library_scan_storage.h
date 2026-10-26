#pragma once

#include <stdbool.h>

/* Live mount predicate shared by library scans and the UI coordinator. */
bool sd_card_root_is_mounted(void);
