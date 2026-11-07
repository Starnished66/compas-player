/* Scan staging: seen set, path hash, journal, staging files, lookup and upsert. */
#include "tagcache_internal.h"
#include "tagcache_scan_intern.h"
#include <strings.h>

bool updating;
_Atomic bool update_failed;
/* A scan starts as a read-only view.  Bits are set by lookups and are only
 * copied into staging if the scan actually needs to publish a generation. */
static unsigned char *scan_seen_bitmap;
static size_t scan_seen_bitmap_bytes;
bool scan_lazy_active;
bool scan_force_publication;
bool scan_preserve_unseen;
static void (*scan_unlock_cb)(void);
static void (*scan_lock_cb)(void);
/* Set while the scan thread has released the query lock to build staging. */
bool scan_initializing;

void scan_seen_release(void) {
    free(scan_seen_bitmap);
    scan_seen_bitmap = NULL;
    scan_seen_bitmap_bytes = 0;
    scan_lazy_active = false;
    scan_force_publication = false;
    scan_unlock_cb = NULL;
    scan_lock_cb = NULL;
}

bool scan_seen_test(int32_t slot) {
    return slot >= 0 && (size_t)slot < scan_seen_bitmap_bytes * 8u &&
           scan_seen_bitmap && (scan_seen_bitmap[(size_t)slot >> 3] & (1u << ((unsigned)slot & 7u)));
}

static void scan_seen_set(int32_t slot) {
    if (slot >= 0 && (size_t)slot < scan_seen_bitmap_bytes * 8u && scan_seen_bitmap)
        scan_seen_bitmap[(size_t)slot >> 3] |= (unsigned char)(1u << ((unsigned)slot & 7u));
}

/* Scan mutations use unlinked staging files in the database directory. */
#define SCAN_HASH_SLOTS (TAGCACHE_MAX_ENTRIES * 4u)
static int scan_hash_fd = -1;
int scan_numeric_fd = -1;
static int32_t *scan_hash_mem;
bool scan_tag_private[TAG_COUNT];

/* The scan hash is deliberately bounded to the same 8 MiB footprint as the
 * old temporary file.  Keep a conservative reserve for the rest of the
 * scan, and retain the file-backed path when memory is tight or allocation
 * fails.  Tests can force the fallback deterministically. */
#define SCAN_HASH_MEM_RESERVE (16ull * 1024ull * 1024ull)
#define SCAN_HASH_MEM_MAX (8ull * 1024ull * 1024ull)

/* mem_available_bytes() intentionally has a generous fallback for unrelated
 * index heuristics.  Hash admission must fail closed when availability is
 * unknown, since allocating the full table is optional. */
size_t scan_hash_mem_available_bytes(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[160];
        size_t avail = 0, memfree = 0, cached = 0, shmem = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned long value = 0;
            if (sscanf(line, "MemAvailable: %lu", &value) == 1)
                avail = (size_t) value * 1024u;
            else if (sscanf(line, "MemFree: %lu", &value) == 1)
                memfree = (size_t) value * 1024u;
            else if (sscanf(line, "Cached: %lu", &value) == 1)
                cached = (size_t) value * 1024u;
            else if (sscanf(line, "Shmem: %lu", &value) == 1)
                shmem = (size_t) value * 1024u;
        }
        fclose(f);
        if (avail) return avail;
        if (cached > shmem) cached -= shmem; else cached = 0;
        if (memfree > SIZE_MAX - cached) return SIZE_MAX;
        if (memfree || cached) return memfree + cached;
    }
#ifdef _SC_AVPHYS_PAGES
    {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0 && (uintmax_t) pages <= SIZE_MAX / (size_t) page_size)
            return (size_t) pages * (size_t) page_size;
    }
#endif
    return 0;
}

void scan_hash_release(void) {
    free(scan_hash_mem);
    scan_hash_mem = NULL;
    if (scan_hash_fd >= 0) close(scan_hash_fd);
    scan_hash_fd = -1;
}

static bool scan_hash_read(uint32_t bucket, int32_t *stored) {
    if (scan_hash_mem) {
        *stored = scan_hash_mem[bucket];
        return true;
    }
    return tc_read_full(scan_hash_fd, stored, sizeof(*stored), (off_t) bucket * sizeof(*stored));
}

static bool scan_hash_write(uint32_t bucket, int32_t stored) {
    if (scan_hash_mem) {
        scan_hash_mem[bucket] = stored;
        return true;
    }
    return write_at(scan_hash_fd, &stored, sizeof(stored), (off_t) bucket * sizeof(stored));
}

static bool copy_fd(int from, int to) {
    char buf[32768];
    off_t off = 0;
    for (;;) {
        ssize_t n = pread(from, buf, sizeof(buf), off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n == 0) return true;
        if (!write_at(to, buf, (size_t) n, off)) return false;
        off += n;
    }
}

bool scan_write_index(int32_t slot, const struct index_entry *idx) {
    bool ok = write_at(reader_master_fd, idx, sizeof(*idx),
                       sizeof(struct master_header) + (off_t) slot * sizeof(*idx));
    if (!ok) update_failed = true;
    return ok;
}

static int32_t scan_find_path(const char *path, bool insert, int32_t new_slot) {
    uint32_t bucket = fnv1a(path, strlen(path)) & (SCAN_HASH_SLOTS - 1);
    for (uint32_t probe = 0; probe < SCAN_HASH_SLOTS; probe++) {
        int32_t stored;
        if (!scan_hash_read(bucket, &stored)) {
            update_failed = true;
            return -1;
        }
        if (!stored) {
            if (insert) {
                stored = new_slot + 1;
                if (!scan_hash_write(bucket, stored)) update_failed = true;
            }
            return -1;
        }
        struct index_entry idx;
        char existing[TAGCACHE_PATH_MAX];
        if (!reader_index(stored - 1, &idx) ||
            !reader_string(tag_filename, idx.tag_seek[tag_filename], stored - 1, existing, sizeof(existing))) {
            update_failed = true;
            return -1;
        }
        if (strcmp(existing, path) == 0) return stored - 1;
        bucket = (bucket + 1) & (SCAN_HASH_SLOTS - 1);
    }
    update_failed = true;
    return -1;
}

/* One record of the scan numeric journal (private unlinked temp file). */
struct scan_numeric_change {
    int32_t slot, rating, playcount, last_played;
};

static void scan_journal_stats(int32_t slot, const struct index_entry *idx) {
    struct scan_numeric_change change = {slot, idx->tag_seek[tag_rating], idx->tag_seek[tag_playcount],
                                         idx->tag_seek[tag_lastplayed]};
    if (!write_fully(scan_numeric_fd, &change, sizeof(change))) update_failed = true;
}

bool replay_scan_numeric(int fd) {
    struct stat journal_stat;
    if (fstat(scan_numeric_fd, &journal_stat) != 0) return false;
    for (off_t at = 0; at < journal_stat.st_size; at += sizeof(struct scan_numeric_change)) {
        struct scan_numeric_change change;
        struct index_entry idx;
        if (!tc_read_full(scan_numeric_fd, &change, sizeof(change), at) ||
            change.slot < 0 || change.slot >= scan_reader.entries) return false;
        int32_t target_slot = change.slot;
        if (commit_slot_map) {
            if (change.slot >= commit_slot_map_count || commit_slot_map[change.slot] < 0) continue;
            target_slot = commit_slot_map[change.slot];
        }
        off_t offset = sizeof(struct master_header) + (off_t)target_slot * sizeof(idx);
        if (!tc_read_full(fd, &idx, sizeof(idx), offset)) return false;
        idx.tag_seek[tag_rating] = change.rating; idx.tag_seek[tag_playcount] = change.playcount;
        idx.tag_seek[tag_lastplayed] = change.last_played;
        if (!write_at(fd, &idx, sizeof(idx), offset)) return false;
    }
    return true;
}

/* Shares the committed string files with the staged reader, giving a tag
 * without one a private empty file. */
static bool staging_open_strings(int strings[]) {
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(reader_persist_tags) / sizeof(reader_persist_tags[0]); i++) {
        int tag = reader_persist_tags[i];
        if (reader_tag_fd[tag] >= 0) {
            strings[tag] = dup(reader_tag_fd[tag]);
            scan_tag_private[tag] = false;
        } else {
            strings[tag] = reader_create_temp();
            struct tagcache_header hdr = {TAGCACHE_MAGIC, 0, 0};
            scan_tag_private[tag] = true;
            ok = strings[tag] >= 0 && write_at(strings[tag], &hdr, sizeof(hdr), 0);
        }
        ok = ok && strings[tag] >= 0;
    }
    return ok;
}

/* Path hash for the scan: RAM when it fits and memory allows, else a temp file. */
static bool staging_hash_init(void) {
    const char *disable_ram_hash = getenv("TAGCACHE_DISABLE_RAM_HASH");
    size_t hash_bytes = (size_t) SCAN_HASH_SLOTS * sizeof(int32_t);
    bool ram_hash_ok = hash_bytes <= SCAN_HASH_MEM_MAX &&
                       (!disable_ram_hash || disable_ram_hash[0] == '\0' || disable_ram_hash[0] == '0') &&
                       scan_hash_mem_available_bytes() >= hash_bytes + SCAN_HASH_MEM_RESERVE;
    if (ram_hash_ok) {
        scan_hash_mem = malloc(hash_bytes);
        if (scan_hash_mem) memset(scan_hash_mem, 0, hash_bytes);
        ram_hash_ok = scan_hash_mem != NULL;
    }
    if (ram_hash_ok) return true;
    scan_hash_fd = reader_create_temp();
    return scan_hash_fd >= 0 && ftruncate(scan_hash_fd, (off_t) hash_bytes) == 0;
}

/* Copies every row into the staged index and the path hash, then carries the
 * lazy scan's seen set over.  Failures set update_failed. */
static void staging_copy_rows(bool had_lazy_scan) {
    for (int32_t slot = 0; slot < ent_count && !update_failed; slot++) {
        struct index_entry idx;
        if (!reader_index(slot, &idx)) { update_failed = true; break; }
        idx.flag &= ~FLAG_SEEN;
        if (!scan_write_index(slot, &idx)) break;
        if (idx.flag & FLAG_DELETED) continue;
        char path[TAGCACHE_PATH_MAX];
        if (!reader_string(tag_filename, idx.tag_seek[tag_filename], slot, path, sizeof(path))) {
            update_failed = true;
            break;
        }
        scan_find_path(path, true, slot);
    }
    /* The lazy session has no staged index to carry the seen state.  Apply it
     * after the copied rows and hash are ready, so subsequent mutations use
     * the normal staged reader. */
    if (had_lazy_scan) {
        for (int32_t slot = 0; slot < ent_count && !update_failed; slot++) {
            if (!scan_seen_test(slot)) continue;
            struct index_entry idx;
            if (!reader_index(slot, &idx)) { update_failed = true; break; }
            idx.flag |= FLAG_SEEN;
            if (!scan_write_index(slot, &idx)) break;
        }
    }
}

/* Releases whatever a failed staging start had opened. */
static void staging_abandon(int master, const int strings[]) {
    if (master >= 0) close(master);
    for (int t = 0; t < TAG_COUNT; t++) if (strings[t] >= 0) close(strings[t]);
    memset(scan_tag_private, 0, sizeof(scan_tag_private));
    scan_hash_release();
}

/* Makes the staged files the selected scan reader. */
static void staging_attach_reader(int master, const int strings[]) {
    reader_context_init(&scan_reader);
    scan_reader.master_fd = master;
    scan_reader.entries = ent_count; scan_reader.live = live_count;
    scan_reader.serial = master_serial; scan_reader.commitid = master_commitid; scan_reader.generation = disk_gen;
    memcpy(scan_reader.tag_fd, strings, sizeof(int) * TAG_COUNT);
    memcpy(scan_reader.tag_size, reader_tag_size, sizeof(scan_reader.tag_size));
    scan_reader.compact_order = reader_compact_order;
    selected_reader = &scan_reader;
}

bool start_staging_with_lock(void (*unlock)(void), void (*lock)(void)) {
    if (updating) return !update_failed;
    bool had_lazy_scan = scan_lazy_active;
    updating = true;
    update_failed = false;
    int master = -1, strings[TAG_COUNT];
    for (int t = 0; t < TAG_COUNT; t++) strings[t] = -1;
    memset(scan_tag_private, 0, sizeof(scan_tag_private));
    bool ok = (mkdir(db_dir, 0755) == 0 || errno == EEXIST);
    if (ok) ok = (master = reader_create_temp()) >= 0;
    if (ok) ok = (scan_numeric_fd = reader_create_temp()) >= 0;
    /* Updates still queued would reach only the old generation's master,
     * which the copy below may predate.  The lock is held, so nothing new can
     * queue before scan_initializing starts journaling them. */
    if (ok) ok = tagcache_flush_numeric();
    scan_initializing = true;
    if (unlock) unlock();
    struct master_header mh = {{TAGCACHE_MAGIC, ent_count * (int32_t) sizeof(struct index_entry), ent_count},
                                master_serial, master_commitid, 0};
    if (ok) ok = reader_master_fd >= 0 ? copy_fd(reader_master_fd, master) : write_at(master, &mh, sizeof(mh), 0);
    if (ok) ok = staging_open_strings(strings);
    if (ok) ok = staging_hash_init();
    if (!ok) {
        staging_abandon(master, strings);
        if (lock) lock();
        scan_initializing = false;
        update_failed = true;
        return false;
    }
    staging_attach_reader(master, strings);
    if (!scan_intern_init()) update_failed = true;
    staging_copy_rows(had_lazy_scan);
    if (lock) lock();
    if (!replay_scan_numeric(reader_master_fd)) update_failed = true;
    scan_initializing = false;
    scan_view_ready = true;
    scan_lazy_active = false;
    scan_unlock_cb = NULL;
    scan_lock_cb = NULL;
    selected_reader = &committed_reader;
    return !update_failed;
}

static bool start_staging(void) {
    void (*unlock)(void) = scan_unlock_cb;
    void (*lock)(void) = scan_lock_cb;
    return start_staging_with_lock(unlock, lock);
}

int32_t find_path(const char *path) {
    if (!path || !db_open) return -1;
    return scan_selected ? scan_find_path(path, false, -1) : reader_find_path(path);
}

static bool append_string(int tag, int32_t slot, const char *value, int32_t *seek) {
    if (!value) value = "";
    size_t len = strlen(value) + 1;
    if (len > TAGCACHE_PATH_MAX) return false;
    if (!scan_tag_private[tag]) {
        int private_fd = reader_create_temp();
        if (private_fd < 0 || !copy_fd(reader_tag_fd[tag], private_fd)) {
            if (private_fd >= 0) close(private_fd);
            return false;
        }
        close(reader_tag_fd[tag]);
        reader_tag_fd[tag] = private_fd;
        scan_tag_private[tag] = true;
    }
    off_t off = lseek(reader_tag_fd[tag], 0, SEEK_END);
    if (off < (off_t) sizeof(struct tagcache_header) || off > INT32_MAX - (off_t) sizeof(struct tagfile_entry) - (off_t) len)
        return false;
    struct tagfile_entry te = {(int32_t) len, slot};
    if (!write_at(reader_tag_fd[tag], &te, sizeof(te), off) ||
        !write_at(reader_tag_fd[tag], value, len, off + sizeof(te))) return false;
    *seek = (int32_t) off;
    reader_tag_size[tag] = (size_t) off + sizeof(te) + len;
    return true;
}

static void tagcache_begin_update_mode_with_lock(void (*unlock)(void), void (*lock)(void),
                                                 bool preserve_unseen) {
    if (!db_open) return;
    if (!tagcache_flush_numeric()) { update_failed = true; return; }
    /* A second begin belongs to the same scan, including a still-lazy scan;
     * restarting here would discard the RAM seen set. */
    if (updating || scan_lazy_active) return;
    scan_seen_release();
    scan_preserve_unseen = preserve_unseen;
    scan_unlock_cb = unlock;
    scan_lock_cb = lock;
    scan_force_publication = !disk_ready || rebuild_preserve_generations ||
                             last_load_outcome == TAGCACHE_LOAD_SUCCESS_RECOVERED ||
                             !tagcache_album_sort_available(1) || !tagcache_album_sort_available(2) ||
                             !tagcache_album_sort_available(3) ||
                             (join_republish_request && tagcache_album_join_active());
    size_t bytes = ((size_t)ent_count + 7u) / 8u;
    if (bytes) scan_seen_bitmap = calloc(1, bytes);
    if (bytes && !scan_seen_bitmap) {
        /* The bitmap is an optimization.  Preserve the old safe behavior if
         * memory is tight rather than making a scan fail. */
        start_staging_with_lock(unlock, lock);
        return;
    }
    scan_seen_bitmap_bytes = bytes;
    scan_lazy_active = true;
}
void tagcache_begin_update_with_lock(void (*unlock)(void), void (*lock)(void)) {
    tagcache_begin_update_mode_with_lock(unlock, lock, false);
}
void tagcache_begin_targeted_update_with_lock(void (*unlock)(void), void (*lock)(void)) {
    tagcache_begin_update_mode_with_lock(unlock, lock, true);
}

void song_set_stats(tagcache_song_t *song, const struct index_entry *idx) {
    song->playcount = idx->tag_seek[tag_playcount];
    song->last_played = idx->tag_seek[tag_lastplayed];
    song->rating = idx->tag_seek[tag_rating];
}

static bool path_is_wav(const char *path) {
    const char *ext = strrchr(path, '.');
    return ext && (!strcasecmp(ext, ".wav") || !strcasecmp(ext, ".rf64"));
}

bool tagcache_lookup(const char *path, int32_t mtime, int32_t size, tagcache_song_t *out) {
    if (!db_open || !path) return false;
    SCAN_SCOPE;
    int32_t slot = find_path(path);
    struct index_entry idx;
    if (slot < 0 || !reader_index(slot, &idx) || (idx.flag & FLAG_DELETED)) return false;
    if (scan_selected) {
        idx.flag |= FLAG_SEEN;
        if (!scan_write_index(slot, &idx)) return false;
    } else if (scan_lazy_active) {
        scan_seen_set(slot);
    }
    if (idx.tag_seek[tag_mtime] != mtime || idx.tag_seek[tag_lastoffset] != size) return false;
    if (path_is_wav(path) && !(idx.flag & FLAG_WAV_ID3_TAGS)) return false;
    if (!(idx.flag & FLAG_ORIGINAL_YEAR_TAGS)) return false;
    tagcache_song_t song;
    if (!reader_song_fields_raw(slot, TAGCACHE_FIELD_ALL, out ? out : &song)) return false;
    if (out) {
        struct index_entry latest;
        if (reader_index(slot, &latest)) {
            numeric_overlay(slot, &latest);
            song_set_stats(out, &latest);
        }
    }
    if (!(out ? out : &song)->album[0] || ((out ? out : &song)->flags & FLAG_TAGS_INCOMPLETE)) return false;
    return true;
}

/* Caller holds SCAN_SCOPE. Resolves the staging slot for path into *slot and
 * *idx (a fresh slot at ent_count, or the existing index entry). Returns false
 * when the upsert must stop; update_failed is already set where it was before. */
static bool upsert_load_slot(const char *path, int32_t *slot, struct index_entry *idx, bool *fresh) {
    *slot = find_path(path);
    *fresh = *slot < 0;
    if (*fresh) {
        if (update_failed) return false;
        /* Keep targeted-transaction slots stable while numeric updates are
         * journaled by slot. Full scans compact tombstones at publication. */
        if (ent_count >= TAGCACHE_MAX_STAGING_ENTRIES) { update_failed = true; return false; }
        *slot = ent_count;
        idx->tag_seek[tag_commitid] = (int32_t) time(NULL);
    } else if (!reader_index(*slot, idx)) { update_failed = true; return false; }
    return true;
}

/* Caller holds SCAN_SCOPE. Appends each string tag that differs from the
 * stored value, setting *tags_changed. Returns false on failure (update_failed set). */
static bool upsert_write_strings(const char *const values[6], bool fresh, int32_t slot,
                                 struct index_entry *idx, bool *tags_changed) {
    const int tags[] = {tag_filename, tag_title, tag_artist, tag_album, tag_albumartist, tag_genre};
    for (int i = 0; i < 6; i++) {
        char canonical[TAGCACHE_PATH_MAX];
        char existing[TAGCACHE_PATH_MAX];
        const char *value = values[i] ? values[i] : "";
        if (tags[i] != tag_filename && tags[i] != tag_title && scan_intern_find(value, canonical, sizeof(canonical))) value = canonical;
        bool unchanged = false;
        if (!fresh) {
            if (!reader_string(tags[i], idx->tag_seek[tags[i]], slot, existing, sizeof(existing))) {
                update_failed = true;
                return false;
            }
            unchanged = strcmp(existing, value) == 0;
        }
        if (unchanged) continue;
        *tags_changed = true;
        if (update_failed || !append_string(tags[i], slot, value, &idx->tag_seek[tags[i]]) ||
            (tags[i] != tag_filename && tags[i] != tag_title && !scan_intern_insert(tags[i], idx->tag_seek[tags[i]]))) {
            update_failed = true;
            return false;
        }
    }
    return true;
}

void tagcache_upsert_year_changed(const char *path, int32_t mtime, int32_t size, const char *title,
                                  const char *artist, const char *album, const char *album_artist,
                                  const char *genre, int32_t track_number, int32_t disc_number,
                                  int32_t release_year, bool *out_tags_changed) {
    /* Anything short of a completed write with identical stored tags counts
     * as changed, so a failed upsert can never pass for a no-op. */
    if (out_tags_changed) *out_tags_changed = true;
    if (!db_open || !path || !path[0] || !start_staging() || update_failed) return;
    SCAN_SCOPE;
    int32_t slot;
    struct index_entry idx = {0};
    bool fresh;
    if (!upsert_load_slot(path, &slot, &idx, &fresh)) return;
    int32_t stored_track = track_number > 0 ? track_number : -1;
    int32_t stored_disc = disc_number > 0 ? disc_number : -1;
    int32_t stored_year = release_year >= 1000 && release_year <= 9999 ? release_year : -1;
    bool wav = path_is_wav(path);
    bool tags_changed = fresh || (wav && !(idx.flag & FLAG_WAV_ID3_TAGS)) ||
                        !(idx.flag & FLAG_ORIGINAL_YEAR_TAGS) ||
                        (idx.flag & (FLAG_DELETED | FLAG_TAGS_INCOMPLETE)) ||
                        idx.tag_seek[tag_tracknumber] != stored_track ||
                        idx.tag_seek[tag_discnumber] != stored_disc ||
                        idx.tag_seek[tag_year] != stored_year;
    const char *values[] = {path, title, artist, album, album_artist, genre};
    if (!upsert_write_strings(values, fresh, slot, &idx, &tags_changed)) return;
    idx.tag_seek[tag_mtime] = mtime;
    idx.tag_seek[tag_lastoffset] = size;
    idx.tag_seek[tag_tracknumber] = stored_track;
    idx.tag_seek[tag_discnumber] = stored_disc;
    idx.tag_seek[tag_year] = stored_year;
    idx.flag = (idx.flag & ~(FLAG_DELETED | FLAG_TAGS_INCOMPLETE)) | FLAG_SEEN;
    if (wav) idx.flag |= FLAG_WAV_ID3_TAGS;
    idx.flag |= FLAG_ORIGINAL_YEAR_TAGS;
    if (!scan_write_index(slot, &idx)) return;
    reader_reset_cache();
    if (fresh) {
        if (slot == ent_count) ent_count++;
        live_count++;
        scan_find_path(path, true, slot);
    }
    if (out_tags_changed && !update_failed) *out_tags_changed = tags_changed;
}

bool tagcache_delete_target_path(const char *path) {
    if (!db_open || !path || !path[0] || !scan_preserve_unseen) return false;
    SCAN_SCOPE;
    int32_t slot = find_path(path);
    if (slot < 0) return !update_failed;
    struct index_entry idx;
    if (!reader_index(slot, &idx)) { update_failed = true; return false; }
    if (idx.flag & FLAG_DELETED) return true;
    if (!start_staging() || update_failed) return false;
    scan_select(true);
    slot = find_path(path);
    if (slot < 0 || !reader_index(slot, &idx)) { update_failed = true; return false; }
    if (idx.flag & FLAG_DELETED) return true;
    idx.flag |= FLAG_DELETED;
    if (!scan_write_index(slot, &idx)) return false;
    if (live_count > 0) live_count--;
    return true;
}

static void update_stats(struct index_entry *idx, int mode, int32_t rating, int32_t count, int32_t last) {
    if (mode == 0 || mode == 2) idx->tag_seek[tag_rating] = rating;
    if (mode == 1) {
        if (idx->tag_seek[tag_playcount] < INT32_MAX) idx->tag_seek[tag_playcount]++;
        idx->tag_seek[tag_lastplayed] = last;
    }
    if (mode == 2) { idx->tag_seek[tag_playcount] = count; idx->tag_seek[tag_lastplayed] = last; }
}

/* waiter: see queue_numeric_update(); NULL for the ordinary asynchronous
 * setters. */
static void set_song_stats(const char *path, int mode, int32_t rating, int32_t count, int32_t last, int *waiter) {
    if (waiter) *waiter = 0;
    if (!db_open || !path) return;
    if (mode == 2 && !start_staging()) return;
    if (mode != 2) {
        int32_t slot = reader_find_path(path);
        struct index_entry idx;
        if (slot >= 0 && reader_index(slot, &idx) && !(idx.flag & FLAG_DELETED)) {
            update_stats(&idx, mode, rating, count, last);
            queue_numeric_update(slot, &idx, waiter);
            if (scan_initializing) scan_journal_stats(slot, &idx);
        }
    }
    if (scan_view_ready) {
        SCAN_SCOPE;
        int32_t slot = scan_find_path(path, false, -1);
        struct index_entry idx;
        if (slot >= 0 && reader_index(slot, &idx) && !(idx.flag & FLAG_DELETED)) {
            update_stats(&idx, mode, rating, count, last);
            if (scan_write_index(slot, &idx)) scan_journal_stats(slot, &idx);
        }
    }
    reader_reset_cache();
}
void tagcache_set_rating(const char *path, int32_t rating) { set_song_stats(path, 0, rating, 0, 0, NULL); }
int tagcache_set_rating_durable_begin(const char *path, int32_t rating) {
    /* A different card at the logical path would discard the write. */
    if (!db_open || !path || !disk_ready || disk_gen < 0 || !db_identity_current() ||
        reader_find_path(path) < 0) return 0;
    int waiter = 0;
    set_song_stats(path, 0, rating, 0, 0, &waiter);
    return waiter;
}
void tagcache_add_play(const char *path, int32_t now) { set_song_stats(path, 1, 0, 0, now, NULL); }
void tagcache_overlay_stats(const char *path, int32_t rating, int32_t count, int32_t last) {
    set_song_stats(path, 2, rating, count, last, NULL);
}
