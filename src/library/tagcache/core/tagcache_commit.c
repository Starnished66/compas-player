/* Generation commit and the end or abort of a scan. */
#include "tagcache_internal.h"

int32_t *commit_slot_map;
int32_t commit_slot_map_count;
typedef struct {
    int32_t seek, slot;
    unsigned char key[TAGCACHE_SORT_TEXT_PREFIX];
} commit_key_t;
static int commit_source_tag;
static bool commit_read_failed;
static int32_t commit_tag_sources[TAGCACHE_REFS_COUNT];

static bool tag_output_matches(int tag, int source, int64_t count, int keys, int32_t gen) {
    int32_t source_gen = gen;
    if (!resolve_tag_generation(gen, tag, &source_gen)) return false;
    char path[640];
    tag_file_path(path, sizeof(path), tag, source_gen);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct tagcache_header old;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && pread(fd, &old, sizeof(old), 0) == (ssize_t)sizeof(old) &&
              old.magic == TAGCACHE_MAGIC;
    char previous[TAGCACHE_PATH_MAX] = "";
    off_t at = sizeof(old);
    int32_t entries = 0;
    for (int64_t i = 0; ok && i < count; i++) {
        commit_key_t key;
        char value[TAGCACHE_PATH_MAX] = "";
        char old_value[TAGCACHE_PATH_MAX] = "";
        ok = tc_read_full(keys, &key, sizeof(key), (off_t)i * sizeof(key));
        if (ok && tag != tag_comment) ok = reader_string(source, key.seek, key.slot, value, sizeof(value));
        if (!ok || (tag != tag_title && tag != tag_filename && i > 0 && ascii_casecmp(previous, value) == 0)) continue;
        struct tagfile_entry te;
        int32_t mapped = key.slot >= 0 && key.slot < commit_slot_map_count ? commit_slot_map[key.slot] : -1;
        int32_t len = (int32_t)strlen(value) + 1;
        ok = mapped >= 0 && pread(fd, &te, sizeof(te), at) == (ssize_t)sizeof(te) &&
             te.tag_length == len && te.idx_id == mapped &&
             pread(fd, old_value, (size_t)len, at + (off_t)sizeof(te)) == (ssize_t)len &&
             memcmp(old_value, value, (size_t)len) == 0;
        if (ok) { at += sizeof(te) + len; entries++; snprintf(previous, sizeof(previous), "%s", value); }
    }
    ok = ok && old.entry_count == entries && old.datasize == at - (off_t)sizeof(old) &&
         st.st_size == at;
    close(fd);
    return ok;
}

static int compare_commit_key(const void *a, const void *b, void *ctx) {
    (void) ctx;
    const commit_key_t *ka = a, *kb = b;
    bool full_compare;
    int c = tagcache_sort_prefix_compare(ka->key, kb->key, sizeof(ka->key), &full_compare);
    if (!c && full_compare) {
        char sa[TAGCACHE_PATH_MAX], sb[TAGCACHE_PATH_MAX];
        if (!reader_string(commit_source_tag, ka->seek, ka->slot, sa, sizeof(sa)) ||
            !reader_string(commit_source_tag, kb->seek, kb->slot, sb, sizeof(sb))) {
            commit_read_failed = true;
            return (ka->slot > kb->slot) - (ka->slot < kb->slot);
        }
        c = ascii_casecmp(sa, sb);
    }
    return c ? c : (ka->slot > kb->slot) - (ka->slot < kb->slot);
}

static bool write_commit_tag(int tag, int32_t gen, int master) {
    int source = tag;
    if (tag == tag_composer || tag == tag_virt_canonicalartist) source = tag_artist;
    if (tag == tag_grouping) source = tag_title;
    bool empty = tag == tag_comment;
    bool unique = tag != tag_title && tag != tag_filename;
    int keys = reader_create_temp(), scratch = reader_create_temp();
    int fd = -1;
    bool ok = keys >= 0 && scratch >= 0;
    size_t key_batch_capacity = (64u * 1024u) / sizeof(commit_key_t);
    unsigned char *key_batch = NULL;
    size_t key_batch_used = 0;
    int64_t count = 0;
    commit_source_tag = source;
    commit_read_failed = false;
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        if (!reader_index(slot, &idx)) { ok = false; break; }
        if (idx.flag & FLAG_DELETED) continue;
        commit_key_t key = {0};
        key.seek = empty ? 0 : idx.tag_seek[source];
        key.slot = slot;
        if (!empty && tag != tag_filename) {
            char value[TAGCACHE_PATH_MAX];
            if (!reader_string(source, key.seek, slot, value, sizeof(value)) ||
                !tagcache_sort_prefix(value, key.key, sizeof(key.key))) {
                commit_read_failed = true;
                ok = false;
                break;
            }
        }
        if (key_batch == NULL) {
            key_batch = (unsigned char *)malloc(64u * 1024u);
            if (key_batch == NULL) { ok = false; break; }
        }
        memcpy(key_batch + key_batch_used * sizeof(key), &key, sizeof(key));
        key_batch_used++;
        count++;
        if (key_batch_used == key_batch_capacity) {
            ok = write_at(keys, key_batch, key_batch_used * sizeof(key),
                                  (off_t)(count - (int64_t)key_batch_used) * (off_t)sizeof(key));
            if (ok) key_batch_used = 0;
        }
    }
    if (ok && key_batch_used != 0)
        ok = write_at(keys, key_batch, key_batch_used * sizeof(commit_key_t),
                              (off_t)(count - (int64_t)key_batch_used) * (off_t)sizeof(commit_key_t));
    free(key_batch);
    if (ok && !empty && tag != tag_filename)
        ok = tc_sort_fd(keys, sizeof(commit_key_t), count, compare_commit_key, NULL, scratch) && !commit_read_failed;
    bool reuse = ok && tag_output_matches(tag, source, count, keys, disk_gen);
    char path[640], tmp_path[680];
    tag_file_path(path, sizeof(path), tag, gen);
    snprintf(tmp_path, sizeof(tmp_path), "%s.new", path);
    if (ok && !reuse) ok = (fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0644)) >= 0;
    struct tagcache_header hdr = {TAGCACHE_MAGIC, 0, 0};
    if (ok && !reuse) ok = write_fully(fd, &hdr, sizeof(hdr));
    char previous[TAGCACHE_PATH_MAX] = "";
    int32_t pos = sizeof(hdr), seek = 0;
    for (int64_t i = 0; ok && i < count; i++) {
        commit_key_t key;
        char value[TAGCACHE_PATH_MAX] = "";
        ok = tc_read_full(keys, &key, sizeof(key), (off_t) i * sizeof(key));
        if (ok && !empty) ok = reader_string(source, key.seek, key.slot, value, sizeof(value));
        if (!ok) break;
        if (!unique || i == 0 || ascii_casecmp(previous, value) != 0) {
            int32_t len = (int32_t) strlen(value) + 1;
            if (!commit_slot_map || key.slot < 0 || key.slot >= commit_slot_map_count || commit_slot_map[key.slot] < 0) {
                ok = false;
                break;
            }
            struct tagfile_entry te = {len, commit_slot_map[key.slot]};
            seek = pos;
            if (!reuse) ok = write_fully(fd, &te, sizeof(te)) && write_fully(fd, value, (size_t) len);
            pos += sizeof(te) + len;
            hdr.entry_count++;
            snprintf(previous, sizeof(previous), "%s", value);
        }
        if (ok) ok = write_at(master, &seek, sizeof(seek), sizeof(struct master_header) +
                               (off_t) commit_slot_map[key.slot] * sizeof(struct index_entry) + (off_t) tag * sizeof(int32_t));
    }
    hdr.datasize = pos - sizeof(hdr);
    if (ok && !reuse) ok = write_at(fd, &hdr, sizeof(hdr), 0);
    if (fd >= 0 && !close_synced(fd)) ok = false;
    fd = -1;
    int index = persisted_tag_index(tag);
    if (ok && !reuse) {
        ok = rename(tmp_path, path) == 0;
        if (ok && index >= 0) commit_tag_sources[index] = gen;
    } else if (ok && reuse) {
        int32_t source_gen = disk_gen;
        ok = resolve_tag_generation(disk_gen, tag, &source_gen);
        if (ok && index >= 0) commit_tag_sources[index] = source_gen;
    }
    if (keys >= 0) close(keys);
    if (scratch >= 0) close(scratch);
    if (!ok) { unlink(path); unlink(tmp_path); }
    return ok;
}

static void commit_slot_map_drop(void) {
    free(commit_slot_map);
    commit_slot_map = NULL;
}

/* Maps old slots to the slots of the new generation (-1 drops a deleted row)
 * and sets live_count.  A targeted pass keeps every slot, so the slot count
 * itself must fit the published limit. */
static bool build_commit_slot_map(void) {
    commit_slot_map_drop();
    commit_slot_map_count = ent_count;
    if (ent_count <= 0) return true;
    if (scan_preserve_unseen && ent_count > TAGCACHE_MAX_ENTRIES) return false;
    commit_slot_map = malloc((size_t)ent_count * sizeof(*commit_slot_map));
    if (!commit_slot_map) return false;
    int32_t dense = 0;
    for (int32_t old = 0; old < ent_count; old++) {
        struct index_entry idx;
        if (!reader_index(old, &idx)) { commit_slot_map_drop(); return false; }
        if (idx.flag & FLAG_DELETED) commit_slot_map[old] = -1;
        else if (scan_preserve_unseen) { commit_slot_map[old] = old; dense++; }
        else if (dense >= TAGCACHE_MAX_ENTRIES) { commit_slot_map_drop(); return false; }
        else commit_slot_map[old] = dense++;
    }
    live_count = dense;
    return true;
}

/* Writes the header and every surviving row of the new master. */
static bool write_master_rows(int master, const struct master_header *mh) {
    bool ok = write_fully(master, mh, sizeof(*mh));
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        if (!scan_preserve_unseen && commit_slot_map && commit_slot_map[slot] < 0) continue;
        ok = reader_index(slot, &idx);
        if (!ok) break;
        idx.flag &= ~FLAG_RAM_ONLY;
        if (idx.flag & FLAG_DELETED) { memset(&idx, 0, sizeof(idx)); idx.flag = FLAG_DELETED; }
        int32_t output_slot = scan_preserve_unseen ? slot : commit_slot_map[slot];
        ok = write_at(master, &idx, sizeof(idx), sizeof(*mh) + (off_t)output_slot * sizeof(idx));
    }
    return ok;
}

/* Writes the tag files and references of generation gen. */
static bool write_generation_files(int master, int32_t gen) {
    bool ok = true;
    for (size_t i = 0; ok && i < TAGCACHE_REFS_COUNT; i++)
        ok = write_commit_tag(persist_tag_ids[i], gen, master);
    if (ok) {
        for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++)
            if (commit_tag_sources[i] < 0) commit_tag_sources[i] = gen;
        ok = write_generation_refs(gen, commit_tag_sources);
    }
    return ok;
}

static bool build_commit_indexes(int32_t gen) {
    selected_reader = &build_reader;
    reader_context_init(&build_reader);
    bool ok = reader_open(gen) && reader_join_build(join_albums_request) && reader_build_indexes() &&
              reader_write_indexes(gen);
    reader_close_indexes();
    reader_close();
    selected_reader = &scan_reader;
    return ok;
}

/* Replays the numeric journal into the finished master and checks that the
 * directory still belongs to this database.  The index build runs with the
 * query lock released; the pinned descriptors keep writes on the original
 * card, but a replacement must still reject publication. */
static bool commit_finalize(const char *path, int32_t gen) {
    /* Updates queued during the build are in the journal; this only keeps the
     * old generation current and its result is not needed. */
    tagcache_flush_numeric();
    if (update_failed) return false;
    int fd = open(path, O_RDWR);
    bool ok = fd >= 0 && replay_scan_numeric(fd);
    if (fd >= 0 && !close_synced(fd)) ok = false;
    if (ok && !db_identity_current()) ok = false;
    if (ok) ok = library_fsync_dir(db_dir);
    if (ok && !rebuild_preserve_generations) ok = write_gen_pointer(gen);
    return ok;
}

static bool write_all(void (*unlock)(void), void (*lock)(void)) {
    int32_t previous = 0, highest = 0, recovered = disk_gen;
    int pointer_result = read_gen_pointer(&previous);
    if (pointer_result <= 0) previous = 0;
    if (!scan_generation_max(&highest)) return false;
    if (highest < previous) highest = previous;
    if (highest < disk_gen) highest = disk_gen;
    if (highest == INT32_MAX) return false;
    int32_t gen = highest + 1;
    for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++) commit_tag_sources[i] = -1;
    if (!build_commit_slot_map()) return false;
    char path[640];
    master_file_path(path, sizeof(path), gen);
    int master = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (master < 0) { commit_slot_map_drop(); return false; }
    int32_t entries = scan_preserve_unseen ? ent_count : live_count;
    struct master_header mh = {{TAGCACHE_INDEXED_MAGIC, entries * (int32_t) sizeof(struct index_entry), entries},
                                master_serial, gen, 0};
    if (unlock) unlock();
    bool ok = write_master_rows(master, &mh) && write_generation_files(master, gen);
    if (close(master) != 0) ok = false;
    if (ok) ok = build_commit_indexes(gen);
    if (lock) lock();
    if (ok) ok = commit_finalize(path, gen);
    else tagcache_flush_numeric();
    if (!ok) {
        unlink_generation(gen);
        commit_slot_map_drop();
        return false;
    }
    disk_gen = gen;
    master_commitid = gen;
    disk_ready = true;
    if (!rebuild_preserve_generations) {
        if (pointer_result > 0) unlink_other_generations(gen, previous);
        else if (recovered > 0 && pointer_result == 0) unlink_other_generations(gen, recovered);
    }
    rebuild_preserve_generations = false;
    commit_slot_map_drop();
    commit_slot_map_count = 0;
    return true;
}

/* stat() of a stored path with the query lock released; true only for ENOENT. */
static bool stored_path_missing(const char *path, void (*unlock)(void), void (*lock)(void)) {
    struct stat st;
    if (unlock) unlock();
    char file[TAGCACHE_PATH_MAX + 256];
    bool missing = path[0] && stat(storage_stored_path(path, file, sizeof(file)), &st) != 0 &&
                   errno == ENOENT;
    if (lock) lock();
    return missing;
}

/* A failed end-of-pass drops the whole session (staging, scan view, seen set,
 * preserve-unseen) and reopens the committed reader, as the header promises.
 * The drop comes first so it holds even when the reopen cannot run (for
 * example no descriptor left for the directory dup). */
static bool end_update_fail(void) {
    scan_session_drop();
    reload_from_disk();
    return false;
}

/* True when a row the lazy scan never saw has vanished from disk; a read
 * failure sets update_failed. */
static bool lazy_unseen_row_missing(void (*unlock)(void), void (*lock)(void)) {
    for (int32_t slot = 0; slot < ent_count; slot++) {
        if (scan_seen_test(slot)) continue;
        struct index_entry idx;
        char path[TAGCACHE_PATH_MAX];
        if (!reader_index(slot, &idx)) { update_failed = true; return false; }
        if (idx.flag & FLAG_DELETED) continue;
        if (!reader_string(tag_filename, idx.tag_seek[tag_filename], slot, path, sizeof(path))) {
            update_failed = true;
            return false;
        }
        if (stored_path_missing(path, unlock, lock)) return true;
    }
    return false;
}

/* Decides whether a lazy pass needs a publication.  1 = go on to publish,
 * 0 = nothing to publish (pass finished), -1 = failed (session dropped). */
static int end_update_lazy_gate(void (*unlock)(void), void (*lock)(void)) {
    if (scan_preserve_unseen) {
        if (updating) return 1;
        if (!scan_force_publication) {
            scan_seen_release();
            scan_preserve_unseen = false;
            return 0;
        }
        return start_staging_with_lock(unlock, lock) ? 1 : (end_update_fail(), -1);
    }
    bool changed = scan_force_publication || lazy_unseen_row_missing(unlock, lock);
    if (update_failed || !db_identity_current()) { end_update_fail(); return -1; }
    if (!changed) {
        scan_seen_release();
        return 0;
    }
    return start_staging_with_lock(unlock, lock) ? 1 : (end_update_fail(), -1);
}

/* Clears FLAG_SEEN on every row of the staged index, tombstoning unseen rows
 * whose file is gone, and counts the live rows. */
static bool end_update_rewrite_index(void (*unlock)(void), void (*lock)(void)) {
    live_count = 0;
    for (int32_t slot = 0; slot < ent_count; slot++) {
        struct index_entry idx;
        if (!reader_index(slot, &idx)) return false;
        if (!scan_preserve_unseen && !(idx.flag & (FLAG_DELETED | FLAG_SEEN))) {
            char path[TAGCACHE_PATH_MAX];
            if (!reader_string(tag_filename, idx.tag_seek[tag_filename], slot, path, sizeof(path))) return false;
            bool missing = stored_path_missing(path, unlock, lock);
            if (!reader_index(slot, &idx)) return false;
            if (missing) idx.flag |= FLAG_DELETED;
        }
        idx.flag &= ~FLAG_SEEN;
        if (!(idx.flag & FLAG_DELETED)) live_count++;
        if (!scan_write_index(slot, &idx)) return false;
        if (unlock && (slot & 63) == 63) { unlock(); sched_yield(); lock(); }
    }
    return true;
}

bool tagcache_end_update_with_lock(void (*unlock)(void), void (*lock)(void)) {
    if (!db_open) return false;
    if (!db_identity_current()) {
        update_failed = true;
        tagcache_abort_update();
        return false;
    }
    if (!tagcache_flush_numeric()) { update_failed = true; return end_update_fail(); }
    if (scan_lazy_active) {
        int gate = end_update_lazy_gate(unlock, lock);
        if (gate <= 0) return gate == 0;
    }
    bool preserve = rebuild_preserve_generations;
    bool ok = start_staging_with_lock(unlock, lock);
    SCAN_SCOPE;
    if (ok) ok = end_update_rewrite_index(unlock, lock);
    if (preserve && !live_count) ok = false;
    if (ok && !update_failed && db_identity_current()) ok = write_all(unlock, lock); else ok = false;
    scan_select(false);
    scan_session_drop();
    bool loaded = reload_from_disk();
    if (ok && loaded && preserve) last_load_outcome = TAGCACHE_LOAD_SUCCESS_RECOVERED;
    return ok && loaded;
}

void tagcache_abort_update(void) {
    if (!db_open) return;
    bool numeric_ok = tagcache_flush_numeric();
    if (numeric_ok && scan_lazy_active && !updating && !scan_view_ready && !update_failed) {
        scan_seen_release();
        scan_preserve_unseen = false;
        return;
    }
    scan_session_drop();
    reload_from_disk();
}
