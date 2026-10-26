#ifndef LIBRARY_SCAN_TAGS_H
#define LIBRARY_SCAN_TAGS_H

#include <stdbool.h>
#include "../parsing/metadata.h"
#include "../db/metadata_db.h"

/* The tags the database stores for a parsed file: absent tags become the
 * "Unknown ..." placeholders and absent numbers -1. Shared by the player's
 * scanner and compas-sdtool so both produce identical rows. */
void library_scan_tags_from_track(const track_metadata_t * meta, cached_tags_t * out);

bool library_scan_tags_equal(const cached_tags_t * a, const cached_tags_t * b);

#endif /* LIBRARY_SCAN_TAGS_H */
