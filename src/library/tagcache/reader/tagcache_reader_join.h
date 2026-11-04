#ifndef HIBY_TAGCACHE_READER_JOIN_H
#define HIBY_TAGCACHE_READER_JOIN_H

/* Join projection: the tracks of an album whose folder holds more than one
 * album artist are shown as READER_JOIN_LABEL.  Stored tags stay raw;
 * reader_song_fields() relabels marked slots.  The marks persist in the album
 * artist groups (representative.ordinal 1) and are restored from their
 * members at load.  Included after reader_index.h. */

typedef struct {
    int32_t slot;
    uint32_t folder, folder_length; /* fnv1a and length of the parent folder */
    unsigned char album[TAGCACHE_SORT_TEXT_PREFIX], aa[TAGCACHE_SORT_TEXT_PREFIX];
} reader_join_key_t;

/* Length of the folder part of a path; 0 when it has none. */
static size_t path_parent_length(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? (size_t)(slash - path) : 0;
}

/* Marks the leading slot (minus bias) of `count` records of `stride` bytes at
 * record `first`; false on a short read or a slot outside [0, slots). */
static bool reader_join_mark_range(int fd, int64_t first, int64_t count, size_t stride, int32_t bias,
                                   unsigned char *bits, int32_t slots) {
    unsigned char buf[8192];
    size_t per_read = sizeof(buf) / stride;
    while (count > 0) {
        size_t n = count < (int64_t)per_read ? (size_t)count : per_read;
        if (first < 0 || !tc_read_full(fd, buf, n * stride, (off_t)first * (off_t)stride)) return false;
        for (size_t i = 0; i < n; i++) {
            int32_t slot;
            memcpy(&slot, buf + i * stride, sizeof(slot));
            if (slot < bias || slot - bias >= slots) return false;
            slot -= bias;
            bits[slot >> 3] |= (unsigned char)(1u << (slot & 7));
        }
        first += (int64_t)n;
        count -= (int64_t)n;
    }
    return true;
}

/* Identity order of the album or album artist of two keys; the full strings
 * are read only when the prefixes tie. */
static int reader_join_compare_text(const reader_join_key_t *a, const reader_join_key_t *b, bool album) {
    bool full;
    int c = tagcache_sort_prefix_compare(album ? a->album : a->aa, album ? b->album : b->aa,
                                         TAGCACHE_SORT_TEXT_PREFIX, &full);
    if (c || !full) return c;
    unsigned field = album ? TAGCACHE_FIELD_ALBUM : TAGCACHE_FIELD_ALBUM_ARTIST;
    tagcache_song_t sa, sb;
    if (!reader_sort_song_fields(a->slot, field, &sa) || !reader_sort_song_fields(b->slot, field, &sb)) return 0;
    return album ? utf8_collate_identity_compare(sa.album, sb.album)
                 : utf8_collate_identity_compare(sa.album_artist, sb.album_artist);
}

static int reader_join_compare_folder(const reader_join_key_t *a, const reader_join_key_t *b) {
    if (a->folder != b->folder) return a->folder < b->folder ? -1 : 1;
    return (a->folder_length > b->folder_length) - (a->folder_length < b->folder_length);
}

/* Orders by folder, then album. */
static int reader_join_compare(const void *a, const void *b, void *context) {
    (void)context;
    const reader_join_key_t *ka = a, *kb = b;
    int c = reader_join_compare_folder(ka, kb);
    if (!c) c = reader_join_compare_text(ka, kb, true);
    return c ? c : (ka->slot > kb->slot) - (ka->slot < kb->slot);
}

/* Reads a slot's folder into dir. */
static bool reader_join_folder(int32_t slot, char *dir) {
    tagcache_song_t song;
    if (!reader_sort_song_fields(slot, TAGCACHE_FIELD_PATH, &song)) return false;
    size_t length = path_parent_length(song.path);
    memcpy(dir, song.path, length);
    dir[length] = '\0';
    return true;
}

/* Marks the mixed run of `count` keys at `start`, unless the folder bytes of
 * a member differ from the first one's (the hash can collide). */
static bool reader_join_mark_run(int fd, int64_t start, int64_t count, int32_t first_slot,
                                 unsigned char *bits, int32_t slots) {
    char first_dir[TAGCACHE_PATH_MAX], dir[TAGCACHE_PATH_MAX];
    reader_join_key_t keys[16];
    if (!reader_join_folder(first_slot, first_dir)) return false;
    for (int64_t at = 0; at < count;) {
        size_t n = (size_t)(count - at < 16 ? count - at : 16);
        if (!tc_read_full(fd, keys, n * sizeof(*keys), (off_t)(start + at) * (off_t)sizeof(*keys))) return false;
        for (size_t i = 0; i < n; i++) {
            if (!reader_join_folder(keys[i].slot, dir)) return false;
            if (strcmp(dir, first_dir)) return true;
        }
        at += (int64_t)n;
    }
    return reader_join_mark_range(fd, start, count, sizeof(*keys), 0, bits, slots);
}

/* Walks the keys sorted by folder and album, run by run, and marks every slot
 * of a run holding two album artists. */
static bool reader_join_walk(int fd, int64_t count, unsigned char *bits, int32_t slots) {
    size_t capacity = READER_TEMP_BATCH_BYTES / sizeof(reader_join_key_t);
    reader_join_key_t *batch = malloc(capacity * sizeof(*batch)), first = {0};
    int64_t start = 0;
    bool ok = batch != NULL, mixed = false;
    for (int64_t at = 0; ok && at < count;) {
        size_t n = (size_t)(count - at < (int64_t)capacity ? count - at : (int64_t)capacity);
        ok = tc_read_full(fd, batch, n * sizeof(*batch), (off_t)at * (off_t)sizeof(*batch));
        for (size_t j = 0; ok && j < n; j++) {
            int64_t k = at + (int64_t)j;
            const reader_join_key_t *key = &batch[j];
            if (k == start || reader_join_compare_folder(&first, key) || reader_join_compare_text(&first, key, true)) {
                if (mixed) ok = reader_join_mark_run(fd, start, k - start, first.slot, bits, slots);
                start = k;
                first = *key;
                mixed = false;
                continue;
            }
            if (reader_join_compare_text(&first, key, false)) mixed = true;
        }
        at += (int64_t)n;
    }
    if (ok && mixed) ok = reader_join_mark_run(fd, start, count - start, first.slot, bits, slots);
    free(batch);
    return ok && !reader_sort_failed;
}

/* Computes the projection of the open generation into reader_join_bits. */
bool reader_join_build(bool enabled) {
    if (!enabled || ent_count <= 0) return true;
    size_t bytes = ((size_t)ent_count + 7u) / 8u;
    unsigned char *bits = calloc(bytes, 1);
    int keys = reader_create_temp(), scratch = reader_create_temp();
    size_t capacity = READER_TEMP_BATCH_BYTES / sizeof(reader_join_key_t);
    reader_join_key_t *batch = malloc(capacity * sizeof(*batch));
    bool ok = bits && keys >= 0 && scratch >= 0 && batch;
    int64_t count = 0;
    size_t used = 0;
    reader_sort_failed = false;
    reader_reset_cache();
    /* Albumless songs form no album, so they are never joined. */
    for (int32_t slot = 0; ok && slot < ent_count; slot++) {
        struct index_entry idx;
        tagcache_song_t song;
        ok = reader_index(slot, &idx);
        if (!ok || (idx.flag & FLAG_DELETED)) continue;
        ok = reader_song_fields_raw(slot, TAGCACHE_FIELD_ALBUM | TAGCACHE_FIELD_ALBUM_ARTIST |
                                          TAGCACHE_FIELD_PATH, &song);
        if (!ok || !song.album[0]) continue;
        size_t folder = path_parent_length(song.path);
        reader_join_key_t *key = &batch[used++];
        memset(key, 0, sizeof(*key));
        key->slot = slot;
        key->folder = fnv1a(song.path, folder);
        key->folder_length = (uint32_t)folder;
        utf8_collate_identity_prefix(song.album, key->album, sizeof(key->album));
        utf8_collate_identity_prefix(song.album_artist, key->aa, sizeof(key->aa));
        if (used == capacity) {
            ok = write_at(keys, batch, used * sizeof(*batch), (off_t)count * (off_t)sizeof(*batch));
            count += (int64_t)used;
            used = 0;
        }
    }
    if (ok && used) ok = write_at(keys, batch, used * sizeof(*batch), (off_t)count * (off_t)sizeof(*batch));
    count += (int64_t)used;
    bool marked = false;
    ok = ok && tc_sort_fd(keys, sizeof(*batch), count, reader_join_compare, NULL, scratch) &&
         !reader_sort_failed && reader_join_walk(keys, count, bits, ent_count);
    for (size_t i = 0; ok && i < bytes && !marked; i++) marked = bits[i] != 0;
    if (keys >= 0) close(keys);
    if (scratch >= 0) close(scratch);
    free(batch);
    reader_reset_cache();
    if (ok && marked) reader_join_bits = bits;
    else free(bits);
    return ok;
}

/* Rebuilds reader_join_bits from the marked album artist groups; false when
 * the groups or their members are malformed. */
static bool reader_join_restore(void) {
    int kind = TAGCACHE_GROUP_ALBUM_ARTIST;
    unsigned char *bits = NULL;
    reader_group_record_t groups[64];
    bool ok = true;
    for (int at = 0; ok && at < reader_group_n[kind];) {
        int n = reader_group_n[kind] - at < 64 ? reader_group_n[kind] - at : 64;
        ok = tc_read_full(reader_group_fd[kind], groups, (size_t)n * sizeof(*groups),
                           (off_t)at * (off_t)sizeof(*groups));
        for (int i = 0; ok && i < n; i++) {
            const reader_group_record_t *group = &groups[i];
            ok = group->representative.ordinal == 0 || group->representative.ordinal == 1;
            if (!ok || !group->representative.ordinal) continue;
            if (!bits) bits = calloc(((size_t)ent_count + 7u) / 8u, 1);
            ok = bits && group->count > 0 &&
                 reader_join_mark_range(reader_members_fd[kind], group->start, group->count,
                                        sizeof(int32_t), 1, bits, ent_count);
        }
        at += n;
    }
    free(reader_join_bits);
    reader_join_bits = ok ? bits : NULL;
    if (!ok) free(bits);
    reader_reset_cache();
    return ok;
}

#endif
