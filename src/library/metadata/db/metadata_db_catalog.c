#include "metadata_db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* All helpers below require metadata_db_mutex to stay held for the whole
 * revision/count/page snapshot. Never call the public metadata_db page helpers
 * from these paths: they acquire their own guard and would split this snapshot
 * into separately locked reads. */
static metadata_db_catalog_result_t catalog_page_begin_locked(
    const char * expected_revision, metadata_db_catalog_page_t * out_page) {
    if (!metadata_db_db_ready || metadata_db_catalog_rebuild_pending || !tagcache_storage_current() || !metadata_db_catalog_identity_prepare())
        return METADATA_DB_CATALOG_UNAVAILABLE;
    int32_t generation = tagcache_generation();
    if (generation < 0) return METADATA_DB_CATALOG_UNAVAILABLE;
    int title_order_version = tagcache_title_order_version();
    if (title_order_version < 1 || title_order_version > 4)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    snprintf(out_page->library_id, sizeof(out_page->library_id), "%s", metadata_db_catalog_library_id);
    /* The trailing format version invalidates clients that cached the older
     * title-rank order while retaining the same library and tagcache generation. */
    snprintf(out_page->revision, sizeof(out_page->revision), "%s:%d:%d", metadata_db_catalog_library_id,
             generation, title_order_version);
    if (expected_revision && strcmp(expected_revision, out_page->revision) != 0)
        return METADATA_DB_CATALOG_STALE;
    return METADATA_DB_CATALOG_OK;
}

bool metadata_db_catalog_get_revision(char *out_revision, size_t out_size) {
    if (out_revision && out_size) out_revision[0] = '\0';
    if (!out_revision || out_size == 0) return false;

    metadata_db_catalog_page_t page = {0};
    METADATA_DB_GUARD;
    if (catalog_page_begin_locked(NULL, &page) != METADATA_DB_CATALOG_OK) return false;
    size_t length = strlen(page.revision);
    if (length >= out_size) return false;
    memcpy(out_revision, page.revision, length + 1);
    return true;
}

bool metadata_db_catalog_revision_matches(const char *expected_revision) {
    if (!expected_revision || !expected_revision[0]) return false;
    metadata_db_catalog_page_t page = {0};
    METADATA_DB_GUARD;
    return catalog_page_begin_locked(expected_revision, &page) == METADATA_DB_CATALOG_OK;
}

metadata_db_catalog_result_t metadata_db_try_catalog_revision_check(const char *expected_revision) {
    if (!expected_revision || !expected_revision[0]) return METADATA_DB_CATALOG_UNAVAILABLE;
    if (!metadata_db_query_try_lock()) return METADATA_DB_CATALOG_BUSY;
    metadata_db_catalog_page_t page = {0};
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, &page);
    metadata_db_query_unlock();
    return result;
}

typedef struct {
    int rank;
    int disc_number;
    int track_number;
} collection_album_rank_t;

static bool collection_cancelled(metadata_db_cancel_fn is_cancelled, void *userdata) {
    return is_cancelled && is_cancelled(userdata);
}

static bool collection_pause_for_lock(metadata_db_cancel_fn is_cancelled, void *userdata) {
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 5000000L };
    while (!metadata_db_query_try_lock()) {
        if (collection_cancelled(is_cancelled, userdata)) return false;
        nanosleep(&pause, NULL);
    }
    return true;
}

/* Appends chunk_count elements of elem bytes to the array *items_slot points
 * at (an int * or collection_album_rank_t * variable), growing it by doubling
 * up to maximum elements. */
static bool append_chunk(void *items_slot, size_t elem, int *count, int *capacity,
                         const void *chunk, int chunk_count, int maximum) {
    if (chunk_count <= 0) return true;
    if (chunk_count > maximum - *count) return false;
    void *items;
    memcpy(&items, items_slot, sizeof(items));
    int needed = *count + chunk_count;
    if (needed > *capacity) {
        int next_capacity = *capacity ? *capacity : 64;
        while (next_capacity < needed) {
            if (next_capacity > maximum / 2) { next_capacity = maximum; break; }
            next_capacity *= 2;
        }
        if (next_capacity < needed || (size_t)next_capacity > SIZE_MAX / elem) return false;
        void *next = realloc(items, (size_t)next_capacity * elem);
        if (!next) return false;
        items = next;
        memcpy(items_slot, &items, sizeof(items));
        *capacity = next_capacity;
    }
    memcpy((char *)items + (size_t)*count * elem, chunk, (size_t)chunk_count * elem);
    *count = needed;
    return true;
}

static bool collection_album_rank_before(const collection_album_rank_t *a,
                                         const collection_album_rank_t *b,
                                         const metadata_db_snapshot_t *snapshot,
                                         bool *ok) {
    int da = a->disc_number > 0 ? a->disc_number : 1;
    int db = b->disc_number > 0 ? b->disc_number : 1;
    if (da != db) return da < db;
    int ta = a->track_number, tb = b->track_number;
    if (ta > 0 || tb > 0) {
        if (ta <= 0) return false;
        if (tb <= 0) return true;
        if (ta != tb) return ta < tb;
    }
    char path_a[600], path_b[600];
    if (!tagcache_snapshot_path_at(snapshot, a->rank, path_a, sizeof(path_a)) ||
        !tagcache_snapshot_path_at(snapshot, b->rank, path_b, sizeof(path_b))) {
        *ok = false;
        return a->rank < b->rank;
    }
    int paths = strcasecmp(path_a, path_b);
    return paths ? paths < 0 : a->rank < b->rank;
}

static bool sort_album_collection_ranks(collection_album_rank_t *ranks, int count,
                                        const metadata_db_snapshot_t *snapshot,
                                        metadata_db_cancel_fn is_cancelled, void *userdata) {
    if (count < 2) return true;
    if ((size_t)count > SIZE_MAX / sizeof(*ranks)) return false;
    collection_album_rank_t *scratch = malloc((size_t)count * sizeof(*scratch));
    if (!scratch) return false;
    bool ok = true;
    for (int width = 1; width < count && ok;) {
        for (int start = 0; start < count && ok; start += width * 2) {
            if (collection_cancelled(is_cancelled, userdata)) { ok = false; break; }
            int mid = start + width < count ? start + width : count;
            int end = mid + width < count ? mid + width : count;
            int left = start, right = mid, out = start;
            while (left < mid && right < end) {
                if (!ok) break;
                if (collection_album_rank_before(&ranks[left], &ranks[right], snapshot, &ok))
                    scratch[out++] = ranks[left++];
                else scratch[out++] = ranks[right++];
            }
            while (left < mid) scratch[out++] = ranks[left++];
            while (right < end) scratch[out++] = ranks[right++];
            memcpy(ranks + start, scratch + start, (size_t)(end - start) * sizeof(*ranks));
        }
        if (width > count / 2) break;
        width *= 2;
    }
    free(scratch);
    return ok && !collection_cancelled(is_cancelled, userdata);
}

/* State of one filtered-collection build, carried across its lock windows. */
typedef struct {
    const char *artist, *album_artist, *album;
    const char *genre; /* NULL when not filtering by genre */
    bool album_order, album_artist_empty_filter, need_song_fields;
    tagcache_query_t query;
    uint64_t candidate_count;
    int32_t catalog_count;
    metadata_db_snapshot_t *snapshot;
    uint8_t *seen_ranks;
    int *ranks;
    collection_album_rank_t *album_ranks;
    int count, capacity, album_capacity;
} collection_build_t;

/* One window's worth of matching ranks. */
typedef struct {
    int ranks[64];
    collection_album_rank_t albums[64];
    int count;
} collection_chunk_t;

static void collection_free(collection_build_t *b) {
    free(b->seen_ranks);
    free(b->album_ranks);
    free(b->ranks);
    if (b->snapshot) tagcache_snapshot_close(b->snapshot);
}

/* First lock window: validates the revision, pins the snapshot and starts the
 * candidate query. The lock is released before the seen-rank bitmap is allocated. */
static metadata_db_catalog_result_t collection_begin(
    collection_build_t *b, const char *expected_revision,
    metadata_db_cancel_fn is_cancelled, void *cancel_userdata) {
    if (!collection_pause_for_lock(is_cancelled, cancel_userdata)) return METADATA_DB_CATALOG_UNAVAILABLE;
    metadata_db_catalog_page_t page = {0};
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, &page);
    if (result == METADATA_DB_CATALOG_OK) {
        b->catalog_count = tagcache_live_count();
        if (b->catalog_count < 0) result = METADATA_DB_CATALOG_UNAVAILABLE;
    }
    if (result == METADATA_DB_CATALOG_OK) {
        b->snapshot = tagcache_snapshot_open(false);
        if (!b->snapshot || tagcache_snapshot_count(b->snapshot) != b->catalog_count)
            result = METADATA_DB_CATALOG_UNAVAILABLE;
    }
    if (result == METADATA_DB_CATALOG_OK) {
        tagcache_query_begin(&b->query, TAGCACHE_QUERY_SONGS, NULL, b->artist, b->album_artist, b->album);
        b->candidate_count = b->query.count;
    }
    metadata_db_query_unlock();
    if (result != METADATA_DB_CATALOG_OK) return result;
    if ((size_t)b->catalog_count > SIZE_MAX - 7u) return METADATA_DB_CATALOG_UNAVAILABLE;
    b->seen_ranks = calloc(((size_t)b->catalog_count + 7u) / 8u, 1);
    if (b->catalog_count > 0 && !b->seen_ranks) return METADATA_DB_CATALOG_UNAVAILABLE;
    return METADATA_DB_CATALOG_OK;
}

/* Caller holds the database lock. Records rank in the seen bitmap. Returns
 * false if it was already seen (duplicate rank). */
static bool collection_mark_seen_locked(collection_build_t *b, int32_t rank) {
    size_t seen_byte = (size_t)rank >> 3;
    uint8_t seen_mask = (uint8_t)(1u << ((unsigned)rank & 7u));
    if (b->seen_ranks[seen_byte] & seen_mask) return false;
    b->seen_ranks[seen_byte] |= seen_mask;
    return true;
}

/* Caller holds the database lock. Validates one candidate rank, applies the
 * album-artist and genre filters, and appends survivors to chunk. Returns
 * false when the build must be abandoned; filtered-out candidates return true. */
static bool collection_take_candidate_locked(collection_build_t *b, collection_chunk_t *chunk, int32_t rank) {
    if (rank < 0 || rank >= b->catalog_count) return false;
    tagcache_song_t song = {0};
    if (b->need_song_fields && !tagcache_song_fields_at_title_rank(
            rank, (b->album_artist_empty_filter ? TAGCACHE_FIELD_ALBUM_ARTIST : 0u) |
                 (b->genre ? TAGCACHE_FIELD_GENRE : 0u), &song))
        return false;
    if (b->album_artist_empty_filter && song.album_artist[0]) return true;
    if (b->genre && strcasecmp(song.genre, b->genre) != 0) return true;
    if (!collection_mark_seen_locked(b, rank)) return false;
    if (b->album_order) {
        chunk->albums[chunk->count++] = (collection_album_rank_t){
            .rank = rank, .disc_number = song.disc_number, .track_number = song.track_number
        };
    } else {
        chunk->ranks[chunk->count++] = rank;
    }
    return true;
}

/* Caller holds the database lock. Scans up to 64 candidates into chunk. The
 * query count is narrowed to this window's end and restored before returning. */
static metadata_db_catalog_result_t collection_scan_chunk_locked(
    collection_build_t *b, collection_chunk_t *chunk,
    metadata_db_cancel_fn is_cancelled, void *cancel_userdata) {
    metadata_db_catalog_result_t result = METADATA_DB_CATALOG_OK;
    tagcache_query_t *query = &b->query;
    uint64_t end_position = query->position + 64;
    if (end_position > b->candidate_count) end_position = b->candidate_count;
    uint64_t full_query_count = query->count;
    query->count = end_position;
    int32_t rank;
    while (query->position < end_position && tagcache_query_next(query, &rank)) {
        if (collection_cancelled(is_cancelled, cancel_userdata) ||
            !collection_take_candidate_locked(b, chunk, rank)) {
            result = METADATA_DB_CATALOG_UNAVAILABLE;
            break;
        }
    }
    query->count = full_query_count;
    if (result == METADATA_DB_CATALOG_OK && query->position < end_position)
        result = METADATA_DB_CATALOG_UNAVAILABLE;
    return result;
}

/* One lock window: re-validates the revision, scans a chunk, then (lock
 * released) appends it to the build and checks for cancellation. */
static metadata_db_catalog_result_t collection_scan_window(
    collection_build_t *b, const char *expected_revision,
    metadata_db_cancel_fn is_cancelled, void *cancel_userdata) {
    if (!collection_pause_for_lock(is_cancelled, cancel_userdata)) return METADATA_DB_CATALOG_UNAVAILABLE;
    metadata_db_catalog_page_t page = {0};
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, &page);
    if (result != METADATA_DB_CATALOG_OK) {
        metadata_db_query_unlock();
        return result;
    }
    collection_chunk_t chunk = { .count = 0 };
    result = collection_scan_chunk_locked(b, &chunk, is_cancelled, cancel_userdata);
    metadata_db_query_unlock();
    if (result != METADATA_DB_CATALOG_OK) return result;

    bool appended = b->album_order
        ? append_chunk(&b->album_ranks, sizeof(*b->album_ranks), &b->count, &b->album_capacity,
                       chunk.albums, chunk.count, b->catalog_count)
        : append_chunk(&b->ranks, sizeof(*b->ranks), &b->count, &b->capacity,
                       chunk.ranks, chunk.count, b->catalog_count);
    if (!appended || collection_cancelled(is_cancelled, cancel_userdata))
        return METADATA_DB_CATALOG_UNAVAILABLE;
    return METADATA_DB_CATALOG_OK;
}

/* No lock held. Album order sorts by disc, track, then path, and drops to plain ranks. */
static metadata_db_catalog_result_t collection_finish(
    collection_build_t *b, metadata_db_cancel_fn is_cancelled, void *cancel_userdata) {
    if (!b->album_order) return METADATA_DB_CATALOG_OK;
    if (!sort_album_collection_ranks(b->album_ranks, b->count, b->snapshot, is_cancelled, cancel_userdata))
        return METADATA_DB_CATALOG_UNAVAILABLE;
    if (b->count > 0) {
        b->ranks = malloc((size_t)b->count * sizeof(*b->ranks));
        if (!b->ranks) return METADATA_DB_CATALOG_UNAVAILABLE;
        for (int i = 0; i < b->count; i++) b->ranks[i] = b->album_ranks[i].rank;
    }
    return METADATA_DB_CATALOG_OK;
}

metadata_db_catalog_result_t metadata_db_filtered_snapshot_open(
    const char *expected_revision, const char *artist, const char *album_artist,
    bool exact_album_artist, const char *album, const char *genre,
    metadata_db_snapshot_t **out_snapshot,
    int **out_title_ranks, int *out_count, metadata_db_cancel_fn is_cancelled,
    void *cancel_userdata) {
    if (out_snapshot) *out_snapshot = NULL;
    if (out_title_ranks) *out_title_ranks = NULL;
    if (out_count) *out_count = 0;
    if (!expected_revision || !expected_revision[0] || !out_snapshot ||
        !out_title_ranks || !out_count ||
        (genre && strlen(genre) >= METADATA_DB_TEXT_MAX))
        return METADATA_DB_CATALOG_UNAVAILABLE;

    collection_build_t b = {
        .artist = artist, .album_artist = album_artist, .album = album,
        .genre = genre && genre[0] ? genre : NULL,
        .album_order = album && album[0],
        .album_artist_empty_filter = exact_album_artist && (!album_artist || !album_artist[0]),
    };
    b.need_song_fields = b.album_artist_empty_filter || b.genre || b.album_order;

    metadata_db_catalog_result_t result = collection_begin(&b, expected_revision, is_cancelled, cancel_userdata);
    while (result == METADATA_DB_CATALOG_OK && b.query.position < b.candidate_count)
        result = collection_scan_window(&b, expected_revision, is_cancelled, cancel_userdata);
    if (result == METADATA_DB_CATALOG_OK) result = collection_finish(&b, is_cancelled, cancel_userdata);
    if (result == METADATA_DB_CATALOG_OK) {
        *out_snapshot = b.snapshot;
        *out_title_ranks = b.ranks;
        *out_count = b.count;
        b.snapshot = NULL;
        b.ranks = NULL;
    }
    collection_free(&b);
    return result;
}

/* Canonical Album group index of a song's album, or -1 when it has no album
 * or no group. The library scanner normalizes absent ALBUMARTIST to ARTIST
 * before upserting. Keep that same identity for older/incomplete cache rows. */
static int catalog_album_group_index(const tagcache_song_t * song) {
    if (!song->album[0]) return -1;
    const char * album_artist = song->album_artist[0] ? song->album_artist : song->artist;
    int index = tagcache_group_index(TAGCACHE_GROUP_ALBUM, song->album, album_artist);
    if (index < 0 && !song->album_artist[0])
        index = tagcache_group_index(TAGCACHE_GROUP_ALBUM, song->album, "");
    return index;
}

static bool catalog_album_representative_locked(const tagcache_song_t * song, int64_t * out_id) {
    *out_id = 0;
    if (!song || !song->album[0]) return true;
    int index = catalog_album_group_index(song);
    tagcache_group_t group;
    if (index < 0 || !tagcache_group_at(TAGCACHE_GROUP_ALBUM, index, &group)) return false;
    *out_id = group.first_song_id;
    return true;
}

metadata_db_catalog_result_t metadata_db_catalog_songs_page(
    const char * expected_revision, int offset, int limit,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_song_t * out_rows) {
    if (!out_page || !out_rows || offset < 0 || limit < 1 || limit > METADATA_DB_CATALOG_PAGE_MAX)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    memset(out_page, 0, sizeof(*out_page));
    METADATA_DB_GUARD;
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, out_page);
    if (result != METADATA_DB_CATALOG_OK) return result;

    out_page->total = tagcache_live_count();
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, NULL, NULL, NULL, NULL);
    tagcache_query_skip(&query, offset);
    int32_t rank;
    while (out_page->count < limit && tagcache_query_next(&query, &rank)) {
        tagcache_song_t song;
        if (!tagcache_song_at_title_rank(rank, &song)) return METADATA_DB_CATALOG_UNAVAILABLE;
        metadata_db_catalog_song_t * dst = &out_rows[out_page->count];
        metadata_db_copy_song(&song, &dst->song);
        if (!catalog_album_representative_locked(&song, &dst->album_representative_id))
            return METADATA_DB_CATALOG_UNAVAILABLE;
        out_page->count++;
    }
    if (out_page->count < limit && (int64_t) offset + out_page->count < out_page->total)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    return METADATA_DB_CATALOG_OK;
}

metadata_db_catalog_result_t metadata_db_catalog_covers_page(
    const char * expected_revision, int offset, int limit,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_cover_t * out_rows) {
    if (!out_page || !out_rows || offset < 0 || limit < 1 || limit > METADATA_DB_CATALOG_PAGE_MAX)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    memset(out_page, 0, sizeof(*out_page));
    METADATA_DB_GUARD;
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, out_page);
    if (result != METADATA_DB_CATALOG_OK) return result;

    int album_count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    out_page->total = album_count;
    for (int rank = offset; rank < album_count && out_page->count < limit; rank++) {
        tagcache_group_t group;
        if (!tagcache_group_at(TAGCACHE_GROUP_ALBUM, rank, &group) || group.first_song_id <= 0)
            return METADATA_DB_CATALOG_UNAVAILABLE;
        tagcache_song_t representative;
        if (!tagcache_song_by_id((int32_t) group.first_song_id, &representative))
            return METADATA_DB_CATALOG_UNAVAILABLE;

        metadata_db_catalog_cover_t * dst = &out_rows[out_page->count];
        memset(dst, 0, sizeof(*dst));
        metadata_db_copy_song(&representative, &dst->representative);
        dst->representative_id = group.first_song_id;
        dst->scanned_mtime = representative.mtime;
        dst->scanned_size = representative.size;
        out_page->count++;
    }
    if (out_page->count < limit && (int64_t) offset + out_page->count < out_page->total)
        return METADATA_DB_CATALOG_UNAVAILABLE;
    return METADATA_DB_CATALOG_OK;
}

metadata_db_catalog_result_t metadata_db_catalog_cover_source(
    const char * expected_revision, int64_t representative_id,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_cover_t * out_source) {
    if (!out_page || !out_source || representative_id <= 0 || representative_id > INT32_MAX)
        return METADATA_DB_CATALOG_STALE;
    memset(out_page, 0, sizeof(*out_page));
    memset(out_source, 0, sizeof(*out_source));
    METADATA_DB_GUARD;
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, out_page);
    if (result != METADATA_DB_CATALOG_OK) return result;

    tagcache_song_t song;
    if (!tagcache_song_by_id((int32_t) representative_id, &song)) return METADATA_DB_CATALOG_STALE;
    int group_index = catalog_album_group_index(&song);
    tagcache_group_t group;
    if (group_index < 0 || !tagcache_group_at(TAGCACHE_GROUP_ALBUM, group_index, &group) ||
        group.first_song_id != representative_id) return METADATA_DB_CATALOG_STALE;

    metadata_db_copy_song(&song, &out_source->representative);
    out_source->representative_id = representative_id;
    out_source->scanned_mtime = song.mtime;
    out_source->scanned_size = song.size;
    out_page->total = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    out_page->count = 1;
    return METADATA_DB_CATALOG_OK;
}

/* Caller holds METADATA_DB_GUARD. Checks the revision, then loads the song by
 * stable ID; STALE when the song is gone. */
static metadata_db_catalog_result_t catalog_song_locked(
    const char * expected_revision, int64_t song_id, tagcache_song_t * out_song) {
    metadata_db_catalog_page_t page;
    memset(&page, 0, sizeof(page));
    metadata_db_catalog_result_t result = catalog_page_begin_locked(expected_revision, &page);
    if (result != METADATA_DB_CATALOG_OK) return result;
    return tagcache_song_by_id((int32_t) song_id, out_song)
        ? METADATA_DB_CATALOG_OK : METADATA_DB_CATALOG_STALE;
}

metadata_db_catalog_result_t metadata_db_catalog_validate_song_revision(
    const char * expected_revision, int64_t song_id) {
    if (!expected_revision || !expected_revision[0] || song_id <= 0 || song_id > INT32_MAX)
        return METADATA_DB_CATALOG_STALE;
    METADATA_DB_GUARD;
    tagcache_song_t song;
    return catalog_song_locked(expected_revision, song_id, &song);
}

metadata_db_catalog_result_t metadata_db_catalog_get_song_revision(
    const char * expected_revision, int64_t song_id, song_row_t * out_song) {
    if (!expected_revision || !expected_revision[0] || !out_song ||
        song_id <= 0 || song_id > INT32_MAX)
        return METADATA_DB_CATALOG_STALE;
    memset(out_song, 0, sizeof(*out_song));
    METADATA_DB_GUARD;
    tagcache_song_t song;
    metadata_db_catalog_result_t result = catalog_song_locked(expected_revision, song_id, &song);
    if (result == METADATA_DB_CATALOG_OK) metadata_db_copy_song(&song, out_song);
    return result;
}
