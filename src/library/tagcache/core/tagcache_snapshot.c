/* Immutable snapshots of the committed title or recency order. */
#include "tagcache_internal.h"

struct tagcache_snapshot {
    atomic_uint references;
    int dir_fd;
    int master_fd;
    int filename_fd;
    int order_fd;
    int32_t count;
    bool legacy;
    int32_t *legacy_slots;
};

tagcache_snapshot_t *tagcache_snapshot_open(bool recency) {
    if (!db_open || !committed_reader.fds_ready || committed_reader.master_fd < 0 ||
        committed_reader.tag_fd[tag_filename] < 0 || committed_reader.live < 0)
        return NULL;
    tagcache_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) return NULL;
    atomic_init(&snapshot->references, 1);
    snapshot->dir_fd = snapshot->master_fd = snapshot->filename_fd = snapshot->order_fd = -1;
    snapshot->dir_fd = db_dir_fd >= 0 ? dup(db_dir_fd) : -1;
    snapshot->count = committed_reader.live;
    snapshot->legacy = committed_reader.legacy_active;
    snapshot->master_fd = dup(committed_reader.master_fd);
    snapshot->filename_fd = dup(committed_reader.tag_fd[tag_filename]);
    if (snapshot->dir_fd < 0 || snapshot->master_fd < 0 || snapshot->filename_fd < 0) goto fail;
    if (snapshot->legacy) {
        if (snapshot->count > 0) {
            size_t bytes;
            if (!checked_mul_size((size_t)snapshot->count, sizeof(*snapshot->legacy_slots), &bytes)) goto fail;
            snapshot->legacy_slots = malloc(bytes);
            if (!snapshot->legacy_slots) goto fail;
            reader_context_t *saved = selected_reader;
            selected_reader = &committed_reader;
            for (int32_t rank = 0; rank < snapshot->count; rank++) {
                tagcache_song_t song;
                if (!legacy_order_song(recency, rank, &song)) {
                    selected_reader = saved;
                    goto fail;
                }
                snapshot->legacy_slots[rank] = song.id - 1;
            }
            selected_reader = saved;
        }
    } else {
        int source = recency ? committed_reader.recency_fd : committed_reader.title_fd;
        snapshot->order_fd = source >= 0 ? dup(source) : -1;
        if (snapshot->order_fd < 0) goto fail;
    }
    return snapshot;
fail:
    tagcache_snapshot_close(snapshot);
    return NULL;
}

int tagcache_snapshot_count(const tagcache_snapshot_t *snapshot) {
    return snapshot ? snapshot->count : 0;
}

bool tagcache_snapshot_path_at(const tagcache_snapshot_t *snapshot, int rank, char *out, size_t out_size) {
    if (!snapshot || !out || out_size == 0 || rank < 0 || rank >= snapshot->count) return false;
    int32_t slot = -1;
    if (snapshot->legacy) {
        slot = snapshot->legacy_slots[rank];
    } else if (!tc_read_full(snapshot->order_fd, &slot, sizeof(slot), (off_t)rank * sizeof(slot))) {
        return false;
    }
    if (slot < 0 || slot >= TAGCACHE_MAX_ENTRIES) return false;
    struct index_entry idx;
    if (!tc_read_full(snapshot->master_fd, &idx, sizeof(idx),
                             (off_t)sizeof(struct master_header) + (off_t)slot * sizeof(idx))) return false;
    int32_t seek = idx.tag_seek[tag_filename];
    struct tagcache_header hdr;
    struct stat filename_st;
    if (seek < (int32_t)sizeof(hdr) || fstat(snapshot->filename_fd, &filename_st) != 0 ||
        !tc_read_full(snapshot->filename_fd, &hdr, sizeof(hdr), 0) ||
        !validate_tag_header(&hdr, (size_t)filename_st.st_size)) return false;
    struct tagfile_entry entry;
    if (!tc_read_full(snapshot->filename_fd, &entry, sizeof(entry), seek) || entry.idx_id != slot ||
        entry.tag_length <= 0 || (size_t)entry.tag_length > out_size ||
        !tc_read_full(snapshot->filename_fd, out, (size_t)entry.tag_length,
                             (off_t)seek + sizeof(entry))) return false;
    return out[entry.tag_length - 1] == '\0' && !memchr(out, '\0', (size_t)entry.tag_length - 1);
}

tagcache_snapshot_t *tagcache_snapshot_retain(tagcache_snapshot_t *snapshot) {
    if (snapshot) atomic_fetch_add_explicit(&snapshot->references, 1, memory_order_relaxed);
    return snapshot;
}

void tagcache_snapshot_close(tagcache_snapshot_t *snapshot) {
    if (!snapshot) return;
    if (atomic_fetch_sub_explicit(&snapshot->references, 1, memory_order_acq_rel) != 1) return;
    if (snapshot->dir_fd >= 0) close(snapshot->dir_fd);
    if (snapshot->master_fd >= 0) close(snapshot->master_fd);
    if (snapshot->filename_fd >= 0) close(snapshot->filename_fd);
    if (snapshot->order_fd >= 0) close(snapshot->order_fd);
    free(snapshot->legacy_slots);
    free(snapshot);
}

int tagcache_snapshot_dup_directory_fd(const tagcache_snapshot_t *snapshot) {
    return snapshot && snapshot->dir_fd >= 0 ? dup(snapshot->dir_fd) : -1;
}
