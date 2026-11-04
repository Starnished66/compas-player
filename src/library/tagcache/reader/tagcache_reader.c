/* Reader, derived-index and query code. The reader and query headers hold
 * static implementation; this file is their only translation unit. */
#include "../core/tagcache_internal.h"

static void legacy_canonical_text(char *value);
static bool query_build(int output_fd);
static bool query_validate(int fd, uint64_t file_size);
#define TC_SORT_AVAILABLE_BYTES() scan_hash_mem_available_bytes()
#include "tagcache_reader.h"
#include "../query/tagcache_sort.h"

/* Persisted pre-v6 groups retain their byte ordering when storage prevents
 * migration. Equality always remains the original ASCII identity relation. */
static int reader_group_name_compare(const char *a, const char *b) {
    if (reader_legacy_active || reader_loaded_index_version >= 7u)
        return utf8_collate_identity_compare(a, b);
    if (reader_loaded_index_version == 6u)
        return utf8_collate_identity_compare_v6(a, b);
    return ascii_casecmp(a, b);
}


#include "tagcache_reader_index.h"
#include "tagcache_reader_join.h"
#include "../query/tagcache_query_index.h"
#include "../query/tagcache_query.h"
#include "tagcache_reader_legacy.h"

/* TAGCACHE_DATABASE_FORMAT stands for every on-card layout below: the master
 * file, the generation references and each index family. Changing any of them
 * needs a new value, so a tool built for an older one refuses the card. This
 * fails the build until the number has been considered. */
_Static_assert(TAGCACHE_MAGIC == 0x54434810 && TAGCACHE_INDEXED_MAGIC == 0x54434811 &&
               TAGCACHE_REFS_VERSION == 1u && TC_QUERY_VERSION == 2u && TC_ALBUM_ORDER_VERSION == 4u &&
               TC_INDEX_VERSION == 8u,
               "bump TAGCACHE_DATABASE_FORMAT (tagcache.h) when a database layout version changes");

