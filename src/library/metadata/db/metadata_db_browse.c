#include "metadata_db_internal.h"

#include "../../../core/text/utf8_util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Distinct-genre list, built once per (incarnation, tagcache generation, title
 * order version) and read under the database guard. A new incarnation (open,
 * close, reload, rebuild prep, publish, abort) frees it; the generation and
 * order version are re-checked on every read because first_song_id follows
 * title rank order. */
typedef struct { char * name; int64_t first_song_id; int song_count; uint32_t hash; } genre_entry_t;
static genre_entry_t * genre_cache;
static int genre_cache_count;
static bool genre_cache_valid;
static uint32_t genre_cache_incarnation;
static int32_t genre_cache_generation;
static int genre_cache_order_version;

/* Caller holds the database guard. */
void metadata_db_genre_cache_free(void) {
    for (int i = 0; i < genre_cache_count; i++) free(genre_cache[i].name);
    free(genre_cache);
    genre_cache = NULL;
    genre_cache_count = 0;
    genre_cache_valid = false;
}
/* Tagcache group for a browse kind; GENRE has no tagcache group and maps to ARTIST. */
static int group_kind_to_tc(metadata_db_group_kind_t kind) {
    return kind == METADATA_DB_GROUP_ALBUM_ARTIST ? TAGCACHE_GROUP_ALBUM_ARTIST
         : kind == METADATA_DB_GROUP_ALBUM        ? TAGCACHE_GROUP_ALBUM
                                                  : TAGCACHE_GROUP_ARTIST;
}

static bool is_artist_kind(metadata_db_group_kind_t kind) {
    return kind == METADATA_DB_GROUP_ARTIST || kind == METADATA_DB_GROUP_ALBUM_ARTIST;
}

static bool album_sort_valid(metadata_db_album_sort_t sort) {
    return sort >= METADATA_DB_ALBUM_SORT_NAME && sort <= (METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR | METADATA_DB_ALBUM_SORT_REVERSE);
}

/* FNV-1a over the ASCII-case-folded bytes of s, continuing from hash. */
static uint32_t fnv1a_fold(uint32_t hash, const char * s) {
    for (const unsigned char * p = (const unsigned char *) s; *p; p++) {
        unsigned char c = *p >= 'A' && *p <= 'Z' ? (unsigned char) (*p + ('a' - 'A')) : *p;
        hash = (hash ^ c) * 16777619u;
    }
    return hash;
}

static int az_kind_to_group(metadata_db_az_kind_t kind) {
    switch (kind) {
        case METADATA_DB_AZ_ARTIST: return TAGCACHE_GROUP_ARTIST;
        case METADATA_DB_AZ_ALBUM_ARTIST: return TAGCACHE_GROUP_ALBUM_ARTIST;
        case METADATA_DB_AZ_ALBUM: return TAGCACHE_GROUP_ALBUM;
        default: return -1;
    }
}

int64_t metadata_db_get_song_count(void) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return 0;
    return tagcache_live_count();
}

void metadata_db_get_group_counts(int * out_artist_count, int * out_album_artist_count, int * out_album_count) {
    METADATA_DB_GUARD;
    *out_artist_count = 0;
    *out_album_artist_count = 0;
    *out_album_count = 0;
    if (!metadata_db_db_ready) return;
    *out_artist_count = tagcache_group_count(TAGCACHE_GROUP_ARTIST);
    *out_album_artist_count = tagcache_group_count(TAGCACHE_GROUP_ALBUM_ARTIST);
    *out_album_count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
}

int metadata_db_get_songs_page_by_recency(int offset, int max_rows, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    int32_t live = tagcache_live_count();
    int w = 0;
    for (int32_t i = offset; i < live && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_at_recency_rank(i, &song)) continue;
        metadata_db_copy_song(&song, &out_rows[w++]);
    }
    return w;
}

/* Caller holds the database guard. Builds the distinct-genre list when the
 * cached one is missing or stale; false (and no cache) if allocation fails. */
static bool genre_cache_ensure(void) {
    int32_t generation = tagcache_generation();
    int order_version = tagcache_title_order_version();
    if (genre_cache_valid && genre_cache_incarnation == metadata_db_db_incarnation &&
        genre_cache_generation == generation && genre_cache_order_version == order_version) return true;
    metadata_db_genre_cache_free();

    enum { GENRE_LIMIT = 2000, GENRE_HASH_SIZE = 4096 };
    genre_entry_t * groups = calloc(GENRE_LIMIT, sizeof(*groups));
    int * slots = malloc(sizeof(*slots) * GENRE_HASH_SIZE);
    if (!groups || !slots) { free(groups); free(slots); return false; }
    for (int i = 0; i < GENRE_HASH_SIZE; i++) slots[i] = -1;
    int used = 0;
    bool ok = true;
    /* A song that could not be read leaves the list incomplete: it still answers this
     * call, as the per-call walk did, but is rebuilt on the next one. */
    bool complete = true;
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, NULL, NULL, NULL, NULL);
    int32_t rank;
    while (tagcache_query_next(&query, &rank)) {
        tagcache_song_t song;
        if (!tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_GENRE, &song)) { complete = false; continue; }
        if (tagcache_song_tags_incomplete(&song)) complete = false;
        if (!song.genre[0]) continue;
        uint32_t hash = fnv1a_fold(2166136261u, song.genre);
        unsigned bucket = hash % GENRE_HASH_SIZE;
        while (slots[bucket] >= 0 && (groups[slots[bucket]].hash != hash ||
               strcasecmp(groups[slots[bucket]].name, song.genre) != 0)) bucket = (bucket + 1) % GENRE_HASH_SIZE;
        if (slots[bucket] >= 0) { groups[slots[bucket]].song_count++; continue; }
        if (used >= GENRE_LIMIT) continue;
        char * name = strdup(song.genre);
        if (!name) { ok = false; break; }
        int index = used++;
        slots[bucket] = index;
        groups[index].hash = hash;
        groups[index].name = name;
        groups[index].song_count = 1;
        groups[index].first_song_id = song.id;
    }
    free(slots);
    if (ok) {
        /* Sort the bounded distinct genre list alphabetically for a stable UI. */
        for (int i = 1; i < used; i++) {
            genre_entry_t value = groups[i];
            int j = i;
            while (j > 0 && utf8_collate_identity_compare(groups[j - 1].name, value.name) > 0) {
                groups[j] = groups[j - 1];
                j--;
            }
            groups[j] = value;
        }
        genre_entry_t * compact = used > 0 ? realloc(groups, sizeof(*groups) * (size_t) used) : NULL;
        if (used > 0 && !compact) ok = false;
        else if (used > 0) groups = compact;
    }
    if (!ok) {
        for (int i = 0; i < used; i++) free(groups[i].name);
        free(groups);
        return false;
    }
    if (used == 0) { free(groups); groups = NULL; }
    genre_cache = groups;
    genre_cache_count = used;
    genre_cache_valid = complete;
    genre_cache_incarnation = metadata_db_db_incarnation;
    genre_cache_generation = generation;
    genre_cache_order_version = order_version;
    return true;
}

int metadata_db_get_genre_count(void) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !genre_cache_ensure()) return 0;
    return genre_cache_count;
}

/* Caller holds METADATA_DB_GUARD. Fills up to max_rows groups from index
 * offset. *next (optional) receives the index after the last one examined,
 * or -1 once the list is exhausted. strict makes an unreadable record fail
 * the call (-1) instead of being skipped, so a walk by *next cannot overlap
 * or leave a hole. */
static int groups_page_locked(metadata_db_group_kind_t kind, int offset, int max_rows, group_row_t * out_rows,
                              int * next, bool strict) {
    if (next) *next = -1;
    if (!metadata_db_db_ready || max_rows <= 0) return strict ? -1 : 0;
    if (offset < 0) offset = 0;
    if (kind == METADATA_DB_GROUP_GENRE) {
        if (!genre_cache_ensure()) return strict ? -1 : 0;
        /* An incomplete list is rebuilt on the next call and may differ, so a
         * walk by *next must not page across it. */
        if (strict && !genre_cache_valid) return -1;
        int written = 0;
        for (int i = offset; i < genre_cache_count && written < max_rows; i++) {
            group_row_t * row = &out_rows[written++];
            memset(row, 0, sizeof(*row));
            snprintf(row->genre_name, sizeof(row->genre_name), "%s", genre_cache[i].name);
            row->song_count = genre_cache[i].song_count;
            row->first_song_id = genre_cache[i].first_song_id;
        }
        if (next && offset + written < genre_cache_count) *next = offset + written;
        return written;
    }
    int tc_kind = group_kind_to_tc(kind);
    int n = tagcache_group_count(tc_kind);
    int w = 0, i = offset;
    for (; i < n && w < max_rows; i++) {
        tagcache_group_t g;
        if (!tagcache_group_at(tc_kind, i, &g)) {
            if (strict) return -1;
            continue;
        }
        metadata_db_copy_group(&g, &out_rows[w]);
        if (tc_kind != TAGCACHE_GROUP_ALBUM) out_rows[w].album_artist[0] = '\0';
        w++;
    }
    if (next && i < n) *next = i;
    return w;
}

int metadata_db_get_groups_page(metadata_db_group_kind_t kind, int offset, int max_rows, group_row_t * out_rows) {
    METADATA_DB_GUARD;
    return groups_page_locked(kind, offset, max_rows, out_rows, NULL, false);
}

static int get_group_songs(metadata_db_group_kind_t kind, const char * name, const char * album_artist,
                            int offset, song_row_t * out_rows, int max_rows) {
    if (!metadata_db_db_ready || max_rows <= 0) return 0;
    if (offset < 0) offset = 0;
    int32_t stack_ids[64];
    int32_t * ids = stack_ids;
    if (max_rows > 64) {
        ids = malloc(sizeof(*ids) * (size_t) max_rows);
        if (!ids) return 0;
    }
    int n;
    switch (kind) {
        case METADATA_DB_GROUP_ARTIST:
            n = tagcache_artist_song_ids(name, offset, ids, max_rows);
            break;
        case METADATA_DB_GROUP_ALBUM_ARTIST:
            n = tagcache_album_artist_song_ids(name, offset, ids, max_rows);
            break;
        case METADATA_DB_GROUP_ALBUM:
            n = tagcache_album_song_ids(name, album_artist, offset, ids, max_rows);
            break;
        default:
            n = 0;
            break;
    }
    int w = 0;
    for (int i = 0; i < n && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_id(ids[i], &song)) continue;
        metadata_db_copy_song(&song, &out_rows[w++]);
    }
    if (ids != stack_ids) free(ids);
    return w;
}

int metadata_db_get_artist_songs(const char * artist, int offset, song_row_t * out_rows, int max_rows) {
    METADATA_DB_GUARD;
    return get_group_songs(METADATA_DB_GROUP_ARTIST, artist, NULL, offset, out_rows, max_rows);
}

/* Mirrors metadata_db_get_artist_songs() above, against the ALBUM_ARTIST
 * group instead -- see tagcache_album_artist_song_ids()'s own comment. */
int metadata_db_get_album_artist_songs(const char * album_artist, int offset, song_row_t * out_rows, int max_rows) {
    METADATA_DB_GUARD;
    return get_group_songs(METADATA_DB_GROUP_ALBUM_ARTIST, album_artist, NULL, offset, out_rows, max_rows);
}

int metadata_db_get_album_songs(const char * album, const char * album_artist, int offset, song_row_t * out_rows,
                                 int max_rows) {
    METADATA_DB_GUARD;
    return get_group_songs(METADATA_DB_GROUP_ALBUM, album, album_artist, offset, out_rows, max_rows);
}

bool metadata_db_get_song_by_id(int64_t id, song_row_t * out_row) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return false;
    tagcache_song_t song;
    if (!tagcache_song_by_id((int32_t) id, &song)) return false;
    metadata_db_copy_song(&song, out_row);
    return true;
}

static bool get_song_by_path(const char * path, song_row_t * out_row, bool raw) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return false;
    tagcache_song_t song;
    if (!(raw ? tagcache_song_by_path_raw(path, &song) : tagcache_song_by_path(path, &song))) return false;
    metadata_db_copy_song(&song, out_row);
    return true;
}

bool metadata_db_get_song_by_path(const char * path, song_row_t * out_row) {
    return get_song_by_path(path, out_row, false);
}

bool metadata_db_get_song_by_path_raw(const char * path, song_row_t * out_row) {
    return get_song_by_path(path, out_row, true);
}

void metadata_db_get_songs_by_ids(const int64_t * ids, int count, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    for (int i = 0; i < count; i++) out_rows[i].id = -1;
    if (!metadata_db_db_ready || count <= 0) return;
    for (int i = 0; i < count; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_id((int32_t) ids[i], &song)) continue;
        metadata_db_copy_song(&song, &out_rows[i]);
    }
}

void metadata_db_get_songs_by_paths(const char * const * paths, int count, song_row_t * out_rows) {
    METADATA_DB_GUARD;
    for (int i = 0; i < count; i++) out_rows[i].id = -1;
    if (!metadata_db_db_ready || count <= 0) return;
    for (int i = 0; i < count; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_path(paths[i], &song)) continue;
        metadata_db_copy_song(&song, &out_rows[i]);
    }
}

/* Caller holds METADATA_DB_GUARD. Rank of path in the order rank_fn defines, or -1. */
static int64_t rank_of_path(int32_t (*rank_fn)(const char *), const char * path) {
    if (!metadata_db_db_ready || !path) return -1;
    int32_t rank = rank_fn(path);
    return rank >= 0 ? rank : -1;
}

int64_t metadata_db_get_song_title_offset(const char * path) {
    METADATA_DB_GUARD;
    return rank_of_path(tagcache_title_rank_of_path, path);
}

int64_t metadata_db_get_song_recency_offset(const char * path) {
    METADATA_DB_GUARD;
    return rank_of_path(tagcache_recency_rank_of_path, path);
}

int64_t metadata_db_get_group_offset(metadata_db_group_kind_t kind, const char * name, const char * album_artist) {
    METADATA_DB_GUARD;
    /* GENRE has no tagcache group; it must not match an artist of that name. */
    if (!metadata_db_db_ready || !name || kind == METADATA_DB_GROUP_GENRE) return -1;
    int tc_kind = group_kind_to_tc(kind);
    int idx = tagcache_group_index(tc_kind, name, album_artist);
    return idx >= 0 ? idx : -1;
}

/* First byte of the name (group) or title (song) at index, ASCII-lowered;
 * false when the row cannot be read. group_kind < 0 means title order. */
static bool az_first_char(int group_kind, int index, unsigned char * out) {
    unsigned char first;
    if (group_kind >= 0) {
        tagcache_group_t group;
        if (!tagcache_group_at(group_kind, index, &group)) return false;
        first = (unsigned char)group.name[0];
    } else {
        tagcache_song_t song;
        if (!tagcache_song_fields_at_title_rank(index, TAGCACHE_FIELD_TITLE, &song)) return false;
        first = (unsigned char)song.title[0];
    }
    if (first >= 'A' && first <= 'Z') first += 'a' - 'A';
    *out = first;
    return true;
}

void metadata_db_get_az_table(metadata_db_az_kind_t kind, int out_table[27]) {
    METADATA_DB_GUARD;
    for (int i = 0; i < 27; i++) out_table[i] = -1;
    if (!metadata_db_db_ready) return;

    int group_kind = az_kind_to_group(kind);
    int count = group_kind >= 0 ? tagcache_group_count(group_kind) : tagcache_live_count();
    for (int letter = 0; letter < 26; letter++) {
        int lo = 0, hi = count;
        unsigned char target = (unsigned char)('a' + letter);
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            unsigned char first;
            if (!az_first_char(group_kind, mid, &first)) break;
            if (first < target) lo = mid + 1; else hi = mid;
        }
        if (lo >= count) continue;
        unsigned char first;
        if (!az_first_char(group_kind, lo, &first)) continue;
        if (first == target) out_table[letter] = lo;
    }
    for (int i = 24; i >= 0; i--) {
        if (out_table[i] == -1) out_table[i] = out_table[i + 1];
    }
    /* The first row may itself be a digit or punctuation.  Checking the
     * offset of A misses that case when A is present at offset zero. */
    out_table[26] = -1;
    if (count > 0) {
        unsigned char first;
        if (!az_first_char(group_kind, 0, &first)) first = 0;
        if (!(first >= 'a' && first <= 'z')) out_table[26] = 0;
    }
}

void metadata_db_get_album_az_table_sorted(metadata_db_album_sort_t sort, int out_table[27]) {
    METADATA_DB_GUARD;
    if (!(sort & METADATA_DB_ALBUM_SORT_REVERSE)) {
        metadata_db_get_az_table(METADATA_DB_AZ_ALBUM, out_table);
        return;
    }
    for (int i = 0; i < 27; i++) out_table[i] = -1;
    if (!metadata_db_db_ready) return;
    int count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    for (int letter = 0; letter < 26; letter++) {
        int lo = 0, hi = count;
        unsigned char target = (unsigned char)('a' + letter), first;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (!az_first_char(TAGCACHE_GROUP_ALBUM, mid, &first)) break;
            if (first <= target) lo = mid + 1; else hi = mid;
        }
        out_table[letter] = lo > 0 && az_first_char(TAGCACHE_GROUP_ALBUM, lo - 1, &first) && first == target
            ? count - lo : -1;
    }
    for (int i = 1; i < 26; i++)
        if (out_table[i] < 0) out_table[i] = out_table[i - 1];
    out_table[26] = -1;
    unsigned char first;
    if (count > 0 && az_first_char(TAGCACHE_GROUP_ALBUM, count - 1, &first) &&
        (first < 'a' || first > 'z')) {
        out_table[26] = 0;
    } else if (count > 0 && az_first_char(TAGCACHE_GROUP_ALBUM, 0, &first) && first < 'a') {
        int lo = 0, hi = count;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (!az_first_char(TAGCACHE_GROUP_ALBUM, mid, &first)) return;
            if (first < 'a') lo = mid + 1; else hi = mid;
        }
        out_table[26] = count - lo;
    }
}

int metadata_db_search_names(metadata_db_az_kind_t kind, const char *needle, int max_rows,
                             metadata_db_search_hit_t *out_hits) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || max_rows <= 0 || !needle || !needle[0]) return 0;
    int group_kind = az_kind_to_group(kind), w = 0;
    tagcache_query_t query;
    tagcache_query_begin(&query, group_kind >= 0 ? group_kind : TAGCACHE_QUERY_TITLES,
                         needle, NULL, NULL, NULL);
    int32_t rank;
    while (w < max_rows && tagcache_query_next(&query, &rank)) {
        if (group_kind >= 0) {
            tagcache_group_t group;
            if (!tagcache_group_at(group_kind, rank, &group)) continue;
            snprintf(out_hits[w].label, sizeof(out_hits[w].label), "%s", group.name);
        } else {
            tagcache_song_t song;
            if (!tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_TITLE, &song)) continue;
            /* Display truncation follows the public song-row title size. */
            song_row_t row = {0};
            snprintf(row.tags.title, sizeof(row.tags.title), "%s", song.title);
            if (!row.tags.title[0]) {
                if (!tagcache_song_fields_by_id(song.id, TAGCACHE_FIELD_PATH, &song)) continue;
                snprintf(row.path, sizeof(row.path), "%s", song.path);
            }
            metadata_db_song_display_title(&row, out_hits[w].label, sizeof(out_hits[w].label));
        }
        out_hits[w++].offset = rank;
    }
    return w;
}

void metadata_db_song_display_title(const song_row_t * row, char * out, size_t out_size) {
    if (!row || !out || out_size == 0) return;
    if (row->tags.title[0] != '\0') {
        utf8_truncate_safe(out, row->tags.title, out_size);
        return;
    }
    const char * slash = strrchr(row->path, '/');
    utf8_truncate_safe(out, slash ? slash + 1 : row->path, out_size);
}

int metadata_db_search_songs(const char *query_text, song_row_t *out_rows, int max_rows) {
    if (!query_text || !query_text[0]) return 0;
    return metadata_db_get_songs_filtered_page(query_text, NULL, NULL, NULL, NULL, 0, max_rows, out_rows);
}

/* True when the song at title rank has this genre (case-insensitive). */
static bool title_rank_has_genre(int32_t rank, const char * genre) {
    tagcache_song_t song;
    return tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_GENRE, &song) &&
           strcasecmp(song.genre, genre) == 0;
}

int64_t metadata_db_count_songs_filtered(const char *needle, const char *artist,
                                        const char *album_artist, const char *album, const char *genre) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return 0;
    if (genre && strlen(genre) >= METADATA_DB_TEXT_MAX) return 0;
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, needle, artist, album_artist, album);
    if (genre && genre[0]) {
        int32_t rank, count = 0;
        while (tagcache_query_next(&query, &rank)) {
            if (title_rank_has_genre(rank, genre)) count++;
        }
        return count;
    }
    return tagcache_query_count(&query);
}

/* Every song of a genre, once, in title order. The old way counted the matches
 * with one full scan and then fetched the songs 64 at a time, each page scanning
 * from the first song again. Here one cursor walks the list; a batch of matches
 * is copied under the database lock and visited after it is released, and the
 * lock is let go every METADATA_DB_GENRE_STEP ranks so the UI's own queries
 * never wait on the whole scan. A publication between steps changes the ranks
 * underneath the cursor, and so does the catalog being closed and reopened:
 * the walk then stops and reports failure. */
#define METADATA_DB_GENRE_BATCH 64
#define METADATA_DB_GENRE_STEP 1024
bool metadata_db_walk_genre_songs(const char * genre, int max_songs,
                                  bool (*visit)(const song_row_t * song, void * user), void * user,
                                  const atomic_bool * cancel) {
    if (!genre || !genre[0] || !visit || max_songs <= 0 || strlen(genre) >= METADATA_DB_TEXT_MAX) return false;
    song_row_t * batch = calloc(METADATA_DB_GENRE_BATCH, sizeof(*batch));
    if (!batch) return false;
    tagcache_query_t query;
    bool begun = false, finished = false, ok = true;
    int32_t generation = 0;
    uint32_t incarnation = 0;
    int visited = 0;
    while (ok && !finished && visited < max_songs) {
        int copied = 0, steps = 0;
        if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) { ok = false; break; }
        {
            METADATA_DB_GUARD;
            if (!metadata_db_db_ready || !tagcache_storage_current()) { ok = false; break; }
            if (!begun) {
                generation = tagcache_generation();
                incarnation = metadata_db_db_incarnation;
                tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, NULL, NULL, NULL, NULL);
                begun = true;
            } else if (tagcache_generation() != generation || metadata_db_db_incarnation != incarnation) { ok = false; break; }
            int32_t rank;
            while (copied < METADATA_DB_GENRE_BATCH && visited + copied < max_songs &&
                   steps < METADATA_DB_GENRE_STEP) {
                if (!tagcache_query_next(&query, &rank)) { finished = true; break; }
                steps++;
                if (!title_rank_has_genre(rank, genre)) continue;
                tagcache_song_t song;
                if (tagcache_song_at_title_rank(rank, &song)) metadata_db_copy_song(&song, &batch[copied++]);
            }
        }
        for (int i = 0; i < copied && ok; i++) ok = visit(&batch[i], user);
        visited += copied;
    }
    free(batch);
    return ok;
}

int metadata_db_get_songs_filtered_page(const char *needle, const char *artist,
                                        const char *album_artist, const char *album, const char *genre,
                                        int offset, int max_rows, song_row_t *out_rows) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || max_rows <= 0) return 0;
    if (genre && strlen(genre) >= METADATA_DB_TEXT_MAX) return 0;
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, needle, artist, album_artist, album);
    if (genre && genre[0]) {
        if (offset < 0) offset = 0;
        int w = 0, skipped = 0;
        int32_t rank;
        while (w < max_rows && tagcache_query_next(&query, &rank)) {
            if (!title_rank_has_genre(rank, genre)) continue;
            if (skipped++ < offset) continue;
            tagcache_song_t song;
            if (tagcache_song_at_title_rank(rank, &song)) metadata_db_copy_song(&song, &out_rows[w++]);
        }
        return w;
    }
    tagcache_query_skip(&query, offset);
    int w = 0;
    int32_t rank;
    while (w < max_rows && tagcache_query_next(&query, &rank)) {
        tagcache_song_t song;
        if (tagcache_song_at_title_rank(rank, &song)) metadata_db_copy_song(&song, &out_rows[w++]);
    }
    return w;
}

/* Caller holds METADATA_DB_GUARD; next/strict as for groups_page_locked. */
static int albums_page_filtered_locked(const char * filter, int offset, int max_rows, group_row_t * out_rows,
                                       int * next, bool strict) {
    if (!filter || !filter[0])
        return groups_page_locked(METADATA_DB_GROUP_ALBUM, offset, max_rows, out_rows, next, strict);
    if (next) *next = -1;
    if (!metadata_db_db_ready || max_rows <= 0) return strict ? -1 : 0;
    int a = 0, b = 0, skipped = 0, w = 0;
    int left = tagcache_group_album_at(TAGCACHE_GROUP_ARTIST, filter, a);
    int right = tagcache_group_album_at(TAGCACHE_GROUP_ALBUM_ARTIST, filter, b);
    while ((left >= 0 || right >= 0) && w < max_rows) {
        int rank = left < 0 ? right : right < 0 ? left : left < right ? left : right;
        if (left == rank) left = tagcache_group_album_at(TAGCACHE_GROUP_ARTIST, filter, ++a);
        if (right == rank) right = tagcache_group_album_at(TAGCACHE_GROUP_ALBUM_ARTIST, filter, ++b);
        if (skipped++ < offset) continue;
        tagcache_group_t group;
        if (tagcache_group_at(TAGCACHE_GROUP_ALBUM, rank, &group)) metadata_db_copy_group(&group, &out_rows[w++]);
        else if (strict) return -1;
    }
    if (next && (left >= 0 || right >= 0)) *next = skipped;
    return w;
}

int metadata_db_get_albums_page_filtered(const char *filter, int offset, int max_rows,
                                         group_row_t *out_rows) {
    METADATA_DB_GUARD;
    return albums_page_filtered_locked(filter, offset, max_rows, out_rows, NULL, false);
}

int metadata_db_group_walk_page(metadata_db_group_walk_t * walk, bool by_albums, metadata_db_group_kind_t kind,
                                const char * album_filter, int max_rows, group_row_t * out_rows) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready) return -1;
    int32_t generation = tagcache_generation();
    if (!walk->started) {
        walk->started = true;
        walk->generation = generation;
        walk->incarnation = metadata_db_db_incarnation;
    } else if (generation != walk->generation || metadata_db_db_incarnation != walk->incarnation) {
        return -1;
    }
    int got = by_albums ? albums_page_filtered_locked(album_filter, walk->next, max_rows, out_rows, &walk->next, true)
                        : groups_page_locked(kind, walk->next, max_rows, out_rows, &walk->next, true);
    if (got < 0) return -1;
    walk->done = walk->next < 0;
    return got;
}

bool metadata_db_album_sort_available(metadata_db_album_sort_t sort) {
    METADATA_DB_GUARD;
    return metadata_db_db_ready && album_sort_valid(sort) && tagcache_album_sort_available((int)sort);
}

int metadata_db_album_canonical_to_sorted(metadata_db_album_sort_t sort, int canonical_rank) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !album_sort_valid(sort) ||
        canonical_rank < 0 || canonical_rank >= tagcache_group_count(TAGCACHE_GROUP_ALBUM)) return -1;
    tagcache_group_t group;
    if (!tagcache_group_at(TAGCACHE_GROUP_ALBUM, canonical_rank, &group)) return -1;
    return tagcache_album_group_sorted_offset((int)sort, group.name, group.album_artist);
}

int metadata_db_get_albums_page_sorted(const char *artist_filter, metadata_db_album_sort_t sort,
                                      int offset, int max_rows, group_row_t *out_rows) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !out_rows || max_rows <= 0 ||
        !album_sort_valid(sort)) return 0;
    if (offset < 0) offset = 0;
    if (sort == METADATA_DB_ALBUM_SORT_NAME)
        return metadata_db_get_albums_page_filtered(artist_filter, offset, max_rows, out_rows);
    int count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    int written = 0, matched = 0;
    int first_rank = artist_filter && artist_filter[0] ? 0 : offset;
    int end_rank = count;
    if ((!artist_filter || !artist_filter[0]) && max_rows <= INT_MAX - offset) {
        end_rank = offset + max_rows;
        if (end_rank > count) end_rank = count;
    }
    for (int rank = first_rank; rank < end_rank && written < max_rows; rank++) {
        tagcache_group_t group;
        if (!tagcache_album_group_at_sorted((int)sort, rank, &group)) continue;
        if (artist_filter && artist_filter[0] &&
            tagcache_group_album_offset(TAGCACHE_GROUP_ARTIST, artist_filter, group.name, group.album_artist) < 0 &&
            tagcache_group_album_offset(TAGCACHE_GROUP_ALBUM_ARTIST, artist_filter, group.name, group.album_artist) < 0)
            continue;
        if (artist_filter && artist_filter[0] && matched++ < offset) continue;
        metadata_db_copy_group(&group, &out_rows[written++]);
    }
    return written;
}

static int genre_group_cmp(const void *a, const void *b) {
    const group_row_t *x = a, *y = b;
    int c = utf8_collate_identity_compare(x->name, y->name);
    return c ? c : utf8_collate_identity_compare(x->album_artist, y->album_artist);
}

enum { GENRE_GROUP_LIMIT = 2000, GENRE_GROUP_HASH_SIZE = 4096 };

typedef struct {
    group_row_t * groups;
    uint32_t * hashes;
    int * slots;
    int used;
} genre_groups_t;

/* Group identity of one song, cut to the stored 127 bytes up front so the hash
 * and the comparison see exactly what the row keeps.  False when the song is
 * not part of the requested groups. */
static bool genre_group_key(metadata_db_group_kind_t kind, metadata_db_group_kind_t artist_kind,
                            const char * artist_filter, const tagcache_song_t * song,
                            char * name, char * second) {
    char artist[sizeof(((group_row_t *) 0)->name)];
    tagcache_artist_primary(song->artist, artist, sizeof(artist));
    const char * album_artist = song->album_artist[0] ? song->album_artist : song->artist;
    const char * key = artist;
    const char * key_second = "";
    if (kind == METADATA_DB_GROUP_ALBUM_ARTIST) {
        key = album_artist;
    } else if (kind == METADATA_DB_GROUP_ALBUM) {
        if (artist_filter && artist_filter[0] &&
            tagcache_cmp_ascii(artist_kind == METADATA_DB_GROUP_ALBUM_ARTIST ? album_artist : artist,
                               artist_filter) != 0) return false;
        key = song->album;
        key_second = album_artist;
    }
    if (!key[0]) return false;
    snprintf(name, sizeof(((group_row_t *) 0)->name), "%s", key);
    snprintf(second, sizeof(((group_row_t *) 0)->album_artist), "%s", key_second);
    return true;
}

/* Counts the song into its group, opening a new one while under the limit. */
static void genre_group_add(genre_groups_t * g, const char * name, const char * second, int64_t song_id) {
    uint32_t hash = 2166136261u;
    for (const char * part = name; part; part = part == name ? second : NULL) {
        hash = fnv1a_fold(hash, part);
        hash = (hash ^ 0x1fu) * 16777619u;
    }
    unsigned bucket = hash % GENRE_GROUP_HASH_SIZE;
    while (g->slots[bucket] >= 0 && (g->hashes[g->slots[bucket]] != hash ||
           strcasecmp(g->groups[g->slots[bucket]].name, name) != 0 ||
           strcasecmp(g->groups[g->slots[bucket]].album_artist, second) != 0))
        bucket = (bucket + 1) % GENRE_GROUP_HASH_SIZE;
    if (g->slots[bucket] >= 0) { g->groups[g->slots[bucket]].song_count++; return; }
    if (g->used >= GENRE_GROUP_LIMIT) return;
    int index = g->used++;
    g->slots[bucket] = index;
    g->hashes[index] = hash;
    snprintf(g->groups[index].name, sizeof(g->groups[index].name), "%s", name);
    snprintf(g->groups[index].album_artist, sizeof(g->groups[index].album_artist), "%s", second);
    g->groups[index].song_count = 1;
    g->groups[index].first_song_id = song_id;
}

int metadata_db_get_genre_groups_page(metadata_db_group_kind_t kind, const char *genre,
                                      metadata_db_group_kind_t artist_kind, const char *artist_filter,
                                      int offset, int max_rows, group_row_t *out_rows, int *out_total) {
    METADATA_DB_GUARD;
    if (out_total) *out_total = 0;
    if (!metadata_db_db_ready || !genre || !genre[0] || strlen(genre) >= METADATA_DB_TEXT_MAX) return 0;
    if (kind != METADATA_DB_GROUP_ARTIST && kind != METADATA_DB_GROUP_ALBUM_ARTIST && kind != METADATA_DB_GROUP_ALBUM)
        return 0;
    if (offset < 0) offset = 0;
    genre_groups_t g = { calloc(GENRE_GROUP_LIMIT, sizeof(*g.groups)), calloc(GENRE_GROUP_LIMIT, sizeof(*g.hashes)),
                         malloc(sizeof(*g.slots) * GENRE_GROUP_HASH_SIZE), 0 };
    if (!g.groups || !g.hashes || !g.slots) { free(g.groups); free(g.hashes); free(g.slots); return 0; }
    for (int i = 0; i < GENRE_GROUP_HASH_SIZE; i++) g.slots[i] = -1;
    tagcache_query_t query;
    tagcache_query_begin(&query, TAGCACHE_QUERY_SONGS, NULL, NULL, NULL, NULL);
    int32_t rank;
    while (tagcache_query_next(&query, &rank)) {
        tagcache_song_t song;
        char name[sizeof(g.groups[0].name)], second[sizeof(g.groups[0].album_artist)];
        if (!tagcache_song_fields_at_title_rank(rank, TAGCACHE_FIELD_GENRE | TAGCACHE_FIELD_ARTIST |
                                                    TAGCACHE_FIELD_ALBUM | TAGCACHE_FIELD_ALBUM_ARTIST, &song) ||
            strcasecmp(song.genre, genre) != 0 ||
            !genre_group_key(kind, artist_kind, artist_filter, &song, name, second)) continue;
        genre_group_add(&g, name, second, song.id);
    }
    qsort(g.groups, (size_t) g.used, sizeof(*g.groups), genre_group_cmp);
    int written = 0;
    for (int i = offset; i < g.used && written < max_rows; i++) out_rows[written++] = g.groups[i];
    if (out_total) *out_total = g.used;
    free(g.slots);
    free(g.hashes);
    free(g.groups);
    return written;
}

int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char *name) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !name || !is_artist_kind(kind)) return 0;
    return tagcache_group_album_count(group_kind_to_tc(kind), name);
}

typedef struct {
    group_row_t * rows;
    int written;
    int reverse_count;
} group_album_page_copy_t;

static void copy_group_album_page_row(int32_t rank, const tagcache_group_t * group,
                                      void * user_data) {
    (void) rank;
    group_album_page_copy_t * copy = user_data;
    int row = copy->reverse_count ? copy->reverse_count - 1 - copy->written : copy->written;
    copy->written++;
    metadata_db_copy_group(group, &copy->rows[row]);
}

int metadata_db_get_albums_for_group(metadata_db_group_kind_t kind, const char *name, int offset, int max_rows,
                                    group_row_t *out_rows) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !name || !out_rows || max_rows <= 0 ||
        !is_artist_kind(kind)) return 0;
    if (offset < 0) offset = 0;
    int tc_kind = group_kind_to_tc(kind);
    group_album_page_copy_t copy = { .rows = out_rows, .written = 0 };
    (void) tagcache_group_album_page(tc_kind, name, offset, max_rows,
                                    copy_group_album_page_row, &copy);
    /* Preserve valid rows copied before a later posting read fails. */
    return copy.written;
}

typedef struct {
    int32_t sorted_rank;
    int32_t canonical_rank;
} sorted_group_album_t;

typedef struct {
    sorted_group_album_t *rows;
    int written;
    int capacity;
    int sort;
    bool failed;
} sorted_group_album_copy_t;

static void copy_sorted_group_album(int32_t rank, const tagcache_group_t *group,
                                    void *user_data) {
    (void) group;
    sorted_group_album_copy_t *copy = user_data;
    int sorted_rank;
    if (!tagcache_album_group_sorted_rank(copy->sort, rank, &sorted_rank) ||
        copy->written >= copy->capacity) {
        copy->failed = true;
        return;
    }
    sorted_group_album_t *dst = &copy->rows[copy->written++];
    dst->sorted_rank = sorted_rank;
    dst->canonical_rank = rank;
}

static int compare_sorted_group_albums(const void *a, const void *b) {
    const sorted_group_album_t *left = a;
    const sorted_group_album_t *right = b;
    if (left->sorted_rank == right->sorted_rank) return 0;
    return left->sorted_rank < right->sorted_rank ? -1 : 1;
}

typedef struct {
    int sort;
    int target_rank;
    int before;
    bool failed;
} sorted_group_album_offset_t;

static void count_sorted_group_albums_before(int32_t rank, const tagcache_group_t *group,
                                            void *user_data) {
    (void) group;
    sorted_group_album_offset_t *state = user_data;
    int sorted_rank;
    if (!tagcache_album_group_sorted_rank(state->sort, rank, &sorted_rank)) state->failed = true;
    else if (sorted_rank < state->target_rank) state->before++;
}

/* The saved sort indexes order every distinct (album, album_artist) pair.
 * Group postings provide the exact Artist or Album Artist membership; sort
 * their compact global-rank pairs here, then read only the requested rows. */
int metadata_db_get_albums_for_group_sorted(metadata_db_group_kind_t kind, const char *name,
                                            metadata_db_album_sort_t sort, int offset, int max_rows,
                                            group_row_t *out_rows) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !name || !out_rows || max_rows <= 0 ||
        !is_artist_kind(kind) ||
        !album_sort_valid(sort)) return 0;
    if (offset < 0) offset = 0;
    int64_t count64 = metadata_db_count_albums_for_group(kind, name);
    if ((sort & ~METADATA_DB_ALBUM_SORT_REVERSE) == METADATA_DB_ALBUM_SORT_NAME ||
        !tagcache_album_sort_available((int)sort)) {
        if (!(sort & METADATA_DB_ALBUM_SORT_REVERSE))
            return metadata_db_get_albums_for_group(kind, name, offset, max_rows, out_rows);
        if (count64 <= offset || count64 > INT_MAX) return 0;
        int rows = (int)count64 - offset;
        if (rows > max_rows) rows = max_rows;
        group_album_page_copy_t copy = { .rows = out_rows, .reverse_count = rows };
        int visited = tagcache_group_album_page(group_kind_to_tc(kind), name,
                                                (int)count64 - offset - rows, rows,
                                                copy_group_album_page_row, &copy);
        return visited == rows && copy.written == rows ? rows : 0;
    }

    if (count64 <= 0 || count64 > INT_MAX || (uint64_t)count64 > SIZE_MAX / sizeof(sorted_group_album_t))
        return 0;
    int count = (int)count64;
    sorted_group_album_t *albums = malloc(sizeof(*albums) * (size_t)count);
    if (!albums) return 0;

    int tc_kind = group_kind_to_tc(kind);
    sorted_group_album_copy_t copy = { .rows = albums, .written = 0,
                                       .capacity = count, .sort = (int)sort };
    int visited = tagcache_group_album_page(tc_kind, name, 0, count,
                                            copy_sorted_group_album, &copy);
    if (visited != count || copy.failed || copy.written != count) {
        free(albums);
        return 0;
    }
    if (copy.written > 1)
        qsort(albums, (size_t)copy.written, sizeof(*albums), compare_sorted_group_albums);

    int written = 0;
    if (offset < copy.written) {
        int available = copy.written - offset;
        written = available < max_rows ? available : max_rows;
        for (int i = 0; i < written; i++) {
            tagcache_group_t group;
            if (!tagcache_group_at(TAGCACHE_GROUP_ALBUM, albums[offset + i].canonical_rank, &group)) {
                written = 0;
                break;
            }
            metadata_db_copy_group(&group, &out_rows[i]);
        }
    }
    free(albums);
    return written;
}

int64_t metadata_db_get_album_for_group_offset(metadata_db_group_kind_t kind, const char *name,
                                               const char *album, const char *album_artist) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !name || !is_artist_kind(kind)) return -1;
    return tagcache_group_album_offset(group_kind_to_tc(kind),
                                      name, album, album_artist);
}

int64_t metadata_db_get_album_for_group_sorted_offset(metadata_db_group_kind_t kind, const char *name,
                                                      metadata_db_album_sort_t sort,
                                                      const char *album, const char *album_artist) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !name || !album || !album_artist ||
        !is_artist_kind(kind) ||
        !album_sort_valid(sort)) return -1;
    int tc_kind = group_kind_to_tc(kind);
    int group_offset = tagcache_group_album_offset(tc_kind, name, album, album_artist);
    if (group_offset < 0) return -1;
    if ((sort & ~METADATA_DB_ALBUM_SORT_REVERSE) == METADATA_DB_ALBUM_SORT_NAME ||
        !tagcache_album_sort_available((int)sort)) {
        int count = tagcache_group_album_count(tc_kind, name);
        return sort & METADATA_DB_ALBUM_SORT_REVERSE ? count - 1 - group_offset : group_offset;
    }

    /* Return a position within this exact group's sorted album list, not the
     * album's global sorted rank. That keeps the synthetic All Songs prefix
     * and now-playing row aligned with the filtered page provider. */
    int canonical_rank = tagcache_group_index(TAGCACHE_GROUP_ALBUM, album, album_artist);
    int target_rank;
    int64_t count64 = metadata_db_count_albums_for_group(kind, name);
    if (canonical_rank < 0 || count64 <= 0 || count64 > INT_MAX ||
        !tagcache_album_group_sorted_rank((int)sort, canonical_rank, &target_rank)) return -1;
    sorted_group_album_offset_t ctx = { .sort = (int)sort, .target_rank = target_rank };
    int visited = tagcache_group_album_page(tc_kind, name, 0, (int)count64,
                                            count_sorted_group_albums_before, &ctx);
    if (visited != count64 || ctx.failed) return -1;
    return ctx.before;
}

#define METADATA_DB_WALK_BATCH 64
/* Caller holds the database guard.  Copies the fields the visitor aggregates
 * for one page of the group's song ids; *got is the page size read. */
static int walk_group_copy_batch(metadata_db_group_kind_t kind, const char * name, int offset, song_row_t * batch,
                                 int32_t * mtimes, int32_t * sizes, int * got) {
    int32_t ids[METADATA_DB_WALK_BATCH];
    *got = kind == METADATA_DB_GROUP_ARTIST
        ? tagcache_artist_song_ids(name, offset, ids, METADATA_DB_WALK_BATCH)
        : tagcache_album_artist_song_ids(name, offset, ids, METADATA_DB_WALK_BATCH);
    int copied = 0;
    for (int i = 0; i < *got; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_id(ids[i], &song)) continue;
        song_row_t * row = &batch[copied];
        row->id = song.id;
        snprintf(row->path, sizeof(row->path), "%s", song.path);
        snprintf(row->tags.album, sizeof(row->tags.album), "%s", song.album);
        snprintf(row->tags.album_artist, sizeof(row->tags.album_artist), "%s", song.album_artist);
        snprintf(row->tags.genre, sizeof(row->tags.genre), "%s", song.genre);
        row->tags.release_year = song.release_year;
        mtimes[copied] = song.mtime;
        sizes[copied] = song.size;
        copied++;
    }
    return copied;
}

bool metadata_db_walk_group_songs(metadata_db_group_kind_t kind, const char * name,
                                  bool (*visit)(const song_row_t * song, int32_t mtime, int32_t size, void * user),
                                  void * user) {
    if (!name || !visit || !is_artist_kind(kind)) return false;
    /* Each batch is copied (just the fields the visitor aggregates) under the
     * database lock and visited after it is released, so the visitor's string
     * work and allocations never hold up the UI's own queries. */
    song_row_t * batch = calloc(METADATA_DB_WALK_BATCH, sizeof(*batch));
    int32_t * mtimes = batch ? calloc(METADATA_DB_WALK_BATCH, sizeof(*mtimes)) : NULL;
    int32_t * sizes = mtimes ? calloc(METADATA_DB_WALK_BATCH, sizeof(*sizes)) : NULL;
    if (!sizes) { free(mtimes); free(batch); return false; }
    bool ok = true, have_generation = false;
    int32_t generation = 0;
    uint32_t incarnation = 0;
    for (int offset = 0; ok;) {
        int got = 0, copied = 0;
        {
            METADATA_DB_GUARD;
            /* A publication or a close and reopen between batches changes the
             * paging underneath the offset: abort rather than mix two states. */
            if (!metadata_db_db_ready || !tagcache_storage_current()) { ok = false; break; }
            if (!have_generation) {
                generation = tagcache_generation();
                incarnation = metadata_db_db_incarnation;
                have_generation = true;
            } else if (tagcache_generation() != generation || metadata_db_db_incarnation != incarnation) {
                ok = false;
                break;
            }
            copied = walk_group_copy_batch(kind, name, offset, batch, mtimes, sizes, &got);
        }
        for (int i = 0; i < copied && ok; i++) ok = visit(&batch[i], mtimes[i], sizes[i], user);
        if (got < METADATA_DB_WALK_BATCH) break;
        offset += got;
    }
    free(sizes);
    free(mtimes);
    free(batch);
    return ok;
}
