/* Seek-based POSIX tagcache using the Rockbox master and string-file format.
 * Copyright (C) 2005 Miika Pekkarinen (original on-disk format)
 * Copyright (C) Open HiBy Player contributors (POSIX implementation)
 * Generation commits preserve the previous library until pointer publication.
 * Loaded state, open/close/load and the read API.
 */
#include "tagcache_internal.h"

/* Loaded database state.  The committed reader serves the query API; the
 * scan reader is the private staging view; the build reader writes the
 * derived indexes of a generation. */
bool db_open;
bool disk_ready;
bool rebuild_preserve_generations;
/* Set by tagcache_open_read_only(): loading must not write (no index migration). */
static bool open_read_only;
tagcache_load_outcome_t last_load_outcome = TAGCACHE_LOAD_FAILED;
/* Set when the last open rewrote an older index layout (see tagcache_indexes_migrated()). */
static bool indexes_migrated;
reader_context_t committed_reader;
reader_context_t scan_reader;
reader_context_t build_reader;
_Thread_local reader_context_t *selected_reader;
bool scan_view_ready;
/* Album joining of the scan in progress; see tagcache_set_album_join(). */
bool join_albums_request;
bool join_republish_request;

void reader_context_init(reader_context_t *context) {
    memset(context, 0, sizeof(*context));
    context->query_fd = context->artist_names_fd = context->master_fd = context->title_fd = context->recency_fd = context->path_fd = -1;
    for (int i = 0; i < TAG_COUNT; i++) context->tag_fd[i] = -1;
    for (int i = 0; i < 3; i++)
        context->group_fd[i] = context->members_fd[i] = context->album_order_fd[i] = context->album_inverse_fd[i] = -1;
    for (int i = 0; i < 2; i++) context->rank_fd[i] = -1;
    context->fds_ready = true;
    context->serial = 1;
    context->album_year_known_count = -1;
    context->query_cache_valid = false;
}

int32_t tagcache_generation(void) { return db_open ? disk_gen : 0; }
int tagcache_title_order_version(void) {
    return db_open ? (int)committed_reader.title_order_version : 0;
}

static bool load_generation(int32_t gen) {
    reader_legacy_active = false;
    reader_close_indexes();
    bool ok = reader_open(gen);
    int indexed = ok ? reader_open_indexes(gen) : -1;
    if (ok && indexed == 1 && reader_loaded_index_version < TC_INDEX_MIGRATE_BELOW && !open_read_only) {
        uint32_t old_index_version = reader_loaded_index_version;
        if (reader_migrate_indexes(gen)) {
            indexes_migrated = true;
        } else {
            /* Keep the previously valid persisted generation usable if rebuilding
             * derived indexes fails (for example, while storage is read-only). */
            reader_close_indexes();
            indexed = reader_open_indexes(gen);
            DB_LOG("DB", "index_migration_failed gen=%d; retaining valid v%u persisted index",
                   gen, old_index_version);
        }
    }
    if (ok && indexed == 0 && reader_requires_indexes) ok = false;
    if (ok && indexed == 0) ok = reader_legacy_init();
    if (!ok || indexed < 0) {
        reader_close_indexes();
        reader_close();
        ent_count = live_count = 0;
        return false;
    }
    disk_ready = true;
    return true;
}

static bool load_all(void) {
    last_load_outcome = TAGCACHE_LOAD_FAILED;
    int32_t pointed = 0;
    int pointer_result = read_gen_pointer(&pointed);
    bool have_pointer = pointer_result > 0;
    bool saved = pointer_result != 0, pointer_file = have_pointer;
    int32_t *generations = NULL;
    size_t count = 0;
    if (!collect_load_generations(&generations, &count, pointed, &saved, &pointer_file)) return false;
    if (!saved) {
        free(generations);
        reader_compact_order = !choose_intern_strings(0);
        reader_title_order_version = 4;
        reader_loaded_index_version = TC_INDEX_VERSION;
        last_load_outcome = TAGCACHE_LOAD_SUCCESS_FRESH;
        return true;
    }
    size_t attempts = count + (have_pointer ? 1 : 0);
    for (size_t i = 0; i < attempts; i++) {
        int32_t gen = have_pointer && !i ? pointed : generations[i - (have_pointer ? 1 : 0)];
        if (load_generation(gen)) {
            bool recovered = pointer_result < 0 || (have_pointer ? i > 0 : gen > 0 || pointer_file || i > 0);
            last_load_outcome = recovered ? TAGCACHE_LOAD_SUCCESS_RECOVERED : TAGCACHE_LOAD_SUCCESS_NORMAL;
            free(generations);
            return true;
        }
        disk_ready = false;
        disk_gen = 0;
    }
    free(generations);
    return false;
}

bool reload_from_disk(void) {
    if (db_dir_fd < 0 || !db_logical_dir[0]) return false;
    bool require_saved = disk_ready;
    int pinned_fd = dup(db_dir_fd);
    char logical[sizeof(db_logical_dir)];
    if (pinned_fd < 0) return false;
    snprintf(logical, sizeof(logical), "%s", db_logical_dir);
    tagcache_close();
    db_dir_fd = pinned_fd;
    snprintf(db_logical_dir, sizeof(db_logical_dir), "%s", logical);
    if (!dir_fd_path(db_dir_fd, db_dir, sizeof(db_dir))) {
        tagcache_close();
        return false;
    }
    db_dir_identity_valid = fstat(db_dir_fd, &db_dir_identity) == 0;
    db_open = true;
    if (!load_all()) {
        tagcache_close();
        return false;
    }
    if (require_saved && last_load_outcome == TAGCACHE_LOAD_SUCCESS_FRESH) {
        tagcache_close();
        last_load_outcome = TAGCACHE_LOAD_FAILED;
        return false;
    }
    numeric_worker_start();
    return true;
}

static bool open_directory(const char *dir, bool read_only) {
    tagcache_close();
    open_read_only = read_only;
    indexes_migrated = false;
    last_load_outcome = TAGCACHE_LOAD_FAILED;
    if (!db_pin_directory(dir)) return false;
    db_open = true;
    if (!load_all()) { tagcache_close(); return false; }
    numeric_worker_start();
    return true;
}

bool tagcache_open(const char *dir) { return open_directory(dir, false); }

bool tagcache_open_read_only(const char *dir) { return open_directory(dir, true); }

bool tagcache_open_for_rebuild(const char *dir) {
    if (tagcache_open(dir)) return true;
    if (!dir || !dir[0] || strlen(dir) >= sizeof(db_logical_dir)) return false;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || access(dir, R_OK | W_OK) != 0) return false;
    tagcache_close();
    if (!db_pin_directory(dir)) return false;
    db_open = true;
    rebuild_preserve_generations = true;
    numeric_worker_start();
    return true;
}

/* Tears down the in-memory update session (staging, scan view, journal, hash,
 * seen set, flags) and returns to the committed reader.  Needs no descriptor,
 * so it works even when reopening the database cannot. */
void scan_session_drop(void) {
    if (scan_view_ready) {
        scan_select(true);
        reader_close();
        scan_select(false);
        scan_view_ready = false;
    }
    selected_reader = &committed_reader;
    if (scan_numeric_fd >= 0) close(scan_numeric_fd);
    scan_numeric_fd = -1;
    scan_intern_close();
    memset(scan_tag_private, 0, sizeof(scan_tag_private));
    scan_hash_release();
    scan_seen_release();
    updating = scan_initializing = update_failed = false;
    scan_preserve_unseen = false;
}

void tagcache_close(void) {
    if (!committed_reader.fds_ready) reader_context_init(&committed_reader);
    numeric_worker_stop();
    reader_legacy_active = false;
    scan_session_drop();
    reader_close_indexes();
    reader_close();
    reader_reset_cache();
    ent_count = live_count = 0;
    disk_gen = master_commitid = 0;
    master_serial = 1;
    reader_loaded_index_version = reader_title_order_version = 0;
    db_open = disk_ready = rebuild_preserve_generations = false;
    if (db_dir_fd >= 0) close(db_dir_fd);
    db_dir_fd = -1;
    db_dir_identity_valid = false;
    db_dir[0] = '\0';
    db_logical_dir[0] = '\0';
}

tagcache_load_outcome_t tagcache_get_load_outcome(void) { return last_load_outcome; }

int32_t tagcache_live_count(void) { return db_open ? live_count : 0; }
int32_t tagcache_slot_count(void) { return db_open ? ent_count : 0; }

/* Compas Tools' compas-sdtool uses these two; the player itself does not. */
bool tagcache_indexes_migrated(void) { return indexes_migrated; }
bool tagcache_song_at_slot(int32_t slot, tagcache_song_t *out) {
    return slot >= 0 && slot < ent_count && tagcache_song_by_id(slot + 1, out);
}

bool tagcache_song_fields_by_id(int32_t id, unsigned fields, tagcache_song_t *out) {
    if (!db_open || id < 1 || id > ent_count || !out) return false;
    return reader_song_fields(id - 1, fields, out);
}

static bool song_fields_at_rank(bool recency, int32_t rank, unsigned fields, tagcache_song_t *out) {
    if (!db_open || rank < 0 || rank >= live_count || !out) return false;
    if (reader_legacy_active) {
        tagcache_song_t song;
        if (!legacy_order_song(recency, rank, &song)) return false;
        return reader_song_fields(song.id - 1, fields, out);
    }
    int32_t slot;
    return tc_read_full(recency ? reader_recency_fd : reader_title_fd, &slot, sizeof(slot), (off_t)rank * sizeof(slot)) &&
           reader_song_fields(slot, fields, out);
}

bool tagcache_song_fields_at_title_rank(int32_t rank, unsigned fields, tagcache_song_t *out) {
    return song_fields_at_rank(false, rank, fields, out);
}

bool tagcache_song_tags_incomplete(const tagcache_song_t *song) {
    return song && (song->flags & FLAG_TAGS_INCOMPLETE);
}

bool tagcache_song_fields_at_recency_rank(int32_t rank, unsigned fields, tagcache_song_t *out) {
    return song_fields_at_rank(true, rank, fields, out);
}

static bool song_by_id(int32_t id, tagcache_song_t *out, bool raw) {
    if (!db_open || id < 1 || id > ent_count) return false;
    struct index_entry idx;
    if (!reader_index(id - 1, &idx) || (idx.flag & FLAG_DELETED)) return false;
    numeric_overlay(id - 1, &idx);
    if (!out) return true;
    if (!(raw ? reader_song_fields_raw(id - 1, TAGCACHE_FIELD_ALL, out) : reader_song(id - 1, out))) return false;
    song_set_stats(out, &idx);
    return true;
}

bool tagcache_song_by_id(int32_t id, tagcache_song_t *out) { return song_by_id(id, out, false); }

bool tagcache_song_by_path(const char *path, tagcache_song_t *out) {
    int32_t slot = find_path(path);
    return slot >= 0 && song_by_id(slot + 1, out, false);
}

bool tagcache_song_by_path_raw(const char *path, tagcache_song_t *out) {
    int32_t slot = find_path(path);
    return slot >= 0 && song_by_id(slot + 1, out, true);
}

void tagcache_set_album_join(bool join, bool republish) {
    join_albums_request = join;
    join_republish_request = republish;
}

bool tagcache_album_join_active(void) { return join_albums_request || committed_reader.join_bits; }

bool tagcache_song_at_title_rank(int32_t rank, tagcache_song_t *out) {
    if (!db_open) return false;
    return reader_order_song(reader_title_fd, false, rank, out);
}

bool tagcache_song_at_recency_rank(int32_t rank, tagcache_song_t *out) {
    if (!db_open) return false;
    return reader_order_song(reader_recency_fd, true, rank, out);
}

int tagcache_group_count(int kind) { return db_open && kind >= 0 && kind < 3 ? (reader_legacy_active ? legacy_group_count(kind) : reader_group_n[kind]) : 0; }
bool tagcache_group_at(int kind, int index, tagcache_group_t *out) { return db_open && reader_group_at(kind, index, out); }
bool tagcache_album_sort_available(int sort) {
    sort &= ~TAGCACHE_ALBUM_SORT_REVERSE;
    if (!db_open) return false;
    if (sort == 0) return true;
    if (sort < 1 || sort > 3 || reader_legacy_active) return false;
    int index = sort - 1;
    return reader_album_order_fd[index] >= 0 && reader_album_inverse_fd[index] >= 0;
}

/* Reads one album order or inverse entry; true when it is a valid group rank. */
static bool album_order_read(int fd, int at, int count, int32_t *out) {
    return tc_read_full(fd, out, sizeof(*out), (off_t)at * sizeof(*out)) && *out >= 0 && *out < count;
}

/* Year indexes end with albums having no known year. Reverse only the known
 * prefix, leaving that tail last. The mapping is its own inverse. */
static int album_reverse_rank(int sort, int rank) {
    if (!(sort & TAGCACHE_ALBUM_SORT_REVERSE)) return rank;
    int count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    int known = count;
    if ((sort & ~TAGCACHE_ALBUM_SORT_REVERSE) == 2 && tagcache_album_sort_available(2)) {
        int lo = READER->album_year_known_count, hi = count;
        if (lo < 0) lo = 0; else hi = lo;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            int32_t canonical;
            if (!album_order_read(reader_album_order_fd[1], mid, count, &canonical)) return -1;
            bool has_year = false;
            int offset = 0, ids[32], got;
            do {
                got = reader_group_ids(TAGCACHE_GROUP_ALBUM, canonical, offset, ids, 32);
                for (int i = 0; i < got; i++) {
                    struct index_entry idx;
                    if (!reader_index(ids[i] - 1, &idx)) return -1;
                    if (idx.tag_seek[tag_year] > 0) { has_year = true; break; }
                }
                offset += got;
            } while (!has_year && got == 32);
            if (has_year) lo = mid + 1; else hi = mid;
        }
        known = READER->album_year_known_count = lo;
    }
    return rank < known ? known - 1 - rank : rank;
}

bool tagcache_album_group_at_sorted(int sort, int rank, tagcache_group_t *out) {
    int count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    if (!db_open || rank < 0 || rank >= count) return false;
    rank = album_reverse_rank(sort, rank);
    sort &= ~TAGCACHE_ALBUM_SORT_REVERSE;
    if (rank < 0) return false;
    if (sort == 0 || !tagcache_album_sort_available(sort))
        return reader_group_at(TAGCACHE_GROUP_ALBUM, rank, out);
    int32_t canonical;
    if (!album_order_read(reader_album_order_fd[sort - 1], rank, count, &canonical)) return false;
    return reader_group_at(TAGCACHE_GROUP_ALBUM, canonical, out);
}

int tagcache_album_group_sorted_offset(int sort, const char *album, const char *album_artist) {
    if (!db_open) return -1;
    int canonical = reader_group_index(TAGCACHE_GROUP_ALBUM, album, album_artist);
    if (canonical < 0) return -1;
    int requested = sort;
    sort &= ~TAGCACHE_ALBUM_SORT_REVERSE;
    if (sort == 0 || !tagcache_album_sort_available(sort)) return album_reverse_rank(requested, canonical);
    int32_t sorted;
    if (!album_order_read(reader_album_inverse_fd[sort - 1], canonical, tagcache_group_count(TAGCACHE_GROUP_ALBUM),
                          &sorted)) return -1;
    return album_reverse_rank(requested, sorted);
}

bool tagcache_album_group_sorted_rank(int sort, int canonical_rank, int *sorted_rank_out) {
    int count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    if (!db_open || !sorted_rank_out || sort < 0 || sort > 7 ||
        canonical_rank < 0 || canonical_rank >= count) return false;
    int base = sort & ~TAGCACHE_ALBUM_SORT_REVERSE;
    int32_t sorted = canonical_rank;
    if (base && tagcache_album_sort_available(base) &&
        !album_order_read(reader_album_inverse_fd[base - 1], canonical_rank, count, &sorted)) return false;
    *sorted_rank_out = album_reverse_rank(sort, sorted);
    return *sorted_rank_out >= 0;
}

int tagcache_group_index(int kind, const char *name, const char *aa) {
    return db_open ? reader_group_index(kind, name, aa) : -1;
}

int tagcache_artist_song_ids(const char *artist, int offset, int32_t *ids, int max) {
    return reader_group_ids(TAGCACHE_GROUP_ARTIST, tagcache_group_index(TAGCACHE_GROUP_ARTIST, artist, ""), offset, ids, max);
}
int tagcache_album_artist_song_ids(const char *artist, int offset, int32_t *ids, int max) {
    return reader_group_ids(TAGCACHE_GROUP_ALBUM_ARTIST, tagcache_group_index(TAGCACHE_GROUP_ALBUM_ARTIST, artist, ""), offset, ids, max);
}
int tagcache_album_song_ids(const char *album, const char *artist, int offset, int32_t *ids, int max) {
    return reader_group_ids(TAGCACHE_GROUP_ALBUM, tagcache_group_index(TAGCACHE_GROUP_ALBUM, album, artist), offset, ids, max);
}

int32_t tagcache_title_rank_of_path(const char *path) { return reader_rank_of(find_path(path), false); }
int32_t tagcache_recency_rank_of_path(const char *path) { return reader_rank_of(find_path(path), true); }
