/* Database directory pinning, file naming, generation refs and pointer, header validation. */
#include "tagcache_internal.h"

/* db_dir is deliberately the proc-fd path for the directory opened at
 * tagcache_open() time.  The logical path is retained only for detecting a
 * card replacement; using it for I/O would reopen whichever card happens to
 * be mounted there now. */
char db_dir[512];
char db_logical_dir[512];
int db_dir_fd = -1;
struct stat db_dir_identity;
bool db_dir_identity_valid;

static void tagcache_log_free_space(const char * dir) {
    if (!db_log_enabled()) return;
    struct statvfs vfs;
    if (statvfs(dir, &vfs) != 0) {
        DB_LOG("DB", "statvfs_failed dir=%s errno=%d(%s)", dir, errno, strerror(errno));
        return;
    }
    uint64_t free_bytes = (uint64_t) vfs.f_bsize * (uint64_t) vfs.f_bavail;
    DB_LOG("DB", "free_space dir=%s free_kb=%" PRIu64 " free_inodes=%" PRIu64, dir, free_bytes / 1024,
           (uint64_t) vfs.f_favail);
}

uint32_t fnv1a(const char * s, size_t n) {
    if (!s) return 0;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t) s[i];
        h *= 16777619u;
    }
    return h;
}

unsigned char ascii_fold(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char) (c + 32);
    return c;
}

int ascii_casecmp(const char * a, const char * b) {
    if (!a) a = "";
    if (!b) b = "";
    for (;;) {
        unsigned char ca = ascii_fold((unsigned char) *a++);
        unsigned char cb = ascii_fold((unsigned char) *b++);
        if (ca != cb) return (int) ca - (int) cb;
        if (ca == 0) return 0;
    }
}

/* Folded byte order matches ascii_casecmp; NUL padding proves equality only when a prefix terminates. */
bool tagcache_sort_prefix(const char *value, unsigned char *prefix, size_t width) {
    if (!value || !prefix || !width) return false;
    const char *end = memchr(value, '\0', TAGCACHE_PATH_MAX);
    if (!end) return false;
    memset(prefix, 0, width);
    size_t length = (size_t)(end - value);
    size_t copy = length < width ? length : width;
    for (size_t i = 0; i < copy; i++) prefix[i] = ascii_fold((unsigned char)value[i]);
    return true;
}

int tagcache_sort_prefix_compare(const unsigned char *a, const unsigned char *b,
                                       size_t width, bool *full_compare) {
    int c = memcmp(a, b, width);
    *full_compare = c == 0 && memchr(a, '\0', width) == NULL;
    return c;
}

int tagcache_cmp_ascii(const char * a, const char * b) {
    return ascii_casecmp(a, b);
}

const char * ascii_casestr(const char * hay, const char * needle) {
    if (!needle[0]) return hay;
    for (const char * h = hay; *h; h++) {
        const char * p = h;
        const char * n = needle;
        while (*n && ascii_fold((unsigned char) *p) == ascii_fold((unsigned char) *n)) {
            p++;
            n++;
        }
        if (!*n) return h;
        if (!*p) return NULL;
    }
    return NULL;
}

static bool db_path(char * out, size_t out_size, const char * name) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!name) return false;
    int n = snprintf(out, out_size, "%s/%s", db_dir, name);
    if (n < 0 || (size_t) n >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* 1 = logical path still names the pinned directory, 0 = a different
 * directory, -1 = the path could not be statted. Callers that publish must
 * not treat -1 as a clean card change. */
int db_logical_directory_status(void) {
    if (!db_dir_identity_valid || !db_logical_dir[0]) return -1;
    struct stat st;
    if (stat(db_logical_dir, &st) != 0) return -1;
    if (st.st_dev == db_dir_identity.st_dev && st.st_ino == db_dir_identity.st_ino) return 1;
    return 0;
}

bool db_identity_current(void) {
    return db_logical_directory_status() == 1;
}

bool db_pin_directory(const char * dir) {
    if (!dir || !dir[0] || strlen(dir) >= sizeof(db_logical_dir)) return false;
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return false;
    }
    if (snprintf(db_logical_dir, sizeof(db_logical_dir), "%s", dir) >= (int)sizeof(db_logical_dir) ||
        !dir_fd_path(fd, db_dir, sizeof(db_dir))) {
        close(fd);
        db_logical_dir[0] = db_dir[0] = '\0';
        return false;
    }
    db_dir_fd = fd;
    db_dir_identity = st;
    db_dir_identity_valid = true;
    return true;
}

bool tagcache_storage_current(void) {
    return db_open && db_identity_current();
}

int tagcache_dup_directory_fd(void) {
    return db_dir_fd >= 0 ? dup(db_dir_fd) : -1;
}

bool tc_read_full(int fd, void *buf, size_t size, off_t offset) {
    unsigned char *p = buf;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, p + done, size - done, offset + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool write_at(int fd, const void *data, size_t size, off_t offset) {
    const unsigned char *p = data;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(fd, p + done, size - done, offset + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool write_fully(int fd, const void * buf, size_t n) {
    const unsigned char * p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        off += (size_t) w;
    }
    return true;
}

bool close_synced(int fd) {
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

const int persist_tag_ids[TAGCACHE_REFS_COUNT] = { tag_artist,       tag_album,    tag_genre,     tag_albumartist, tag_composer,
                                       tag_comment,      tag_grouping, tag_virt_canonicalartist, tag_title, tag_filename };
_Static_assert(sizeof(persist_tag_ids) / sizeof(persist_tag_ids[0]) == TAGCACHE_REFS_COUNT,
               "persisted tag list must match generation refs manifest");

int persisted_tag_index(int tag) {
    for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++)
        if (persist_tag_ids[i] == tag) return (int)i;
    return -1;
}

static void generation_refs_name(char *out, size_t n, int32_t gen) {
    snprintf(out, n, "tagcache.refs.g%d", gen);
}

static bool refs_file_path(char *out, size_t n, int32_t gen) {
    char name[80];
    generation_refs_name(name, sizeof(name), gen);
    return db_path(out, n, name);
}

static uint32_t generation_refs_checksum(const struct tagcache_generation_refs *refs) {
    return fnv1a((const char *)refs, offsetof(struct tagcache_generation_refs, checksum));
}

static bool generation_refs_valid(const struct tagcache_generation_refs *refs, int32_t gen) {
    if (!refs || refs->magic != TAGCACHE_REFS_MAGIC || refs->version != TAGCACHE_REFS_VERSION ||
        refs->generation != gen || refs->checksum != generation_refs_checksum(refs)) return false;
    for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++)
        if (refs->source[i] < 0 || refs->source[i] > gen) return false;
    return true;
}

/* Resolve one physical tag generation. Missing refs are the legacy format. */
bool resolve_tag_generation(int32_t gen, int tag, int32_t *out_source) {
    if (!out_source || gen < 0) return false;
    int index = persisted_tag_index(tag);
    if (index < 0) return false;
    *out_source = gen;
    if (gen == 0) return true;
    char path[640];
    refs_file_path(path, sizeof(path), gen);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT;
    struct tagcache_generation_refs refs;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size == (off_t)sizeof(refs) &&
              tc_read_full(fd, &refs, sizeof(refs), 0) &&
              generation_refs_valid(&refs, gen);
    if (ok) *out_source = refs.source[index];
    close(fd);
    return ok;
}

bool write_generation_refs(int32_t gen, const int32_t source[TAGCACHE_REFS_COUNT]) {
    char path[640], tmp[680];
    refs_file_path(path, sizeof(path), gen);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    struct tagcache_generation_refs refs = {TAGCACHE_REFS_MAGIC, TAGCACHE_REFS_VERSION, gen, {0}, 0};
    for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++) {
        if (source[i] < 0 || source[i] > gen) return false;
        refs.source[i] = source[i];
    }
    refs.checksum = generation_refs_checksum(&refs);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    bool ok = false;
    if (fd >= 0) {
        bool wrote = write_fully(fd, &refs, sizeof(refs));
        bool synced = close_synced(fd);
        fd = -1;
        ok = wrote && synced;
    }
    if (!ok) {
        if (fd >= 0) close(fd);
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

static void tag_file_name(char * out, size_t n, int tag, int32_t gen) {
    if (gen > 0) snprintf(out, n, "database_%d.tcd.g%d", tag, gen);
    else snprintf(out, n, "database_%d.tcd", tag);
}

static void master_file_name(char * out, size_t n, int32_t gen) {
    if (gen > 0) snprintf(out, n, "database_idx.tcd.g%d", gen);
    else snprintf(out, n, "database_idx.tcd");
}

bool tag_file_path(char * out, size_t n, int tag, int32_t gen) {
    char name[80];
    tag_file_name(name, sizeof(name), tag, gen);
    return db_path(out, n, name);
}

bool master_file_path(char * out, size_t n, int32_t gen) {
    char name[80];
    master_file_name(name, sizeof(name), gen);
    return db_path(out, n, name);
}

bool write_gen_pointer(int32_t gen) {
    char path[640], tmp[640];
    db_path(path, sizeof(path), "tagcache.gen");
    db_path(tmp, sizeof(tmp), "tagcache.gen.tmp");
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        DB_LOG("DB", "write_gen_pointer open_failed gen=%d errno=%d(%s) path=%s", gen, errno, strerror(errno), tmp);
        tagcache_log_free_space(db_dir);
        return false;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", gen);
    bool ok = n > 0 && write_fully(fd, buf, (size_t) n);
    bool synced = close_synced(fd);
    if (!synced || !ok) {
        DB_LOG("DB", "write_gen_pointer write_failed gen=%d errno=%d(%s) path=%s", gen, errno, strerror(errno), tmp);
        tagcache_log_free_space(db_dir);
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        DB_LOG("DB", "write_gen_pointer rename_failed gen=%d errno=%d(%s) from=%s to=%s", gen, errno,
               strerror(errno), tmp, path);
        unlink(tmp);
        return false;
    }
    if (!library_fsync_dir(db_dir)) {
        /* Non-fatal: the rename() above already committed the pointer, so
         * the generation is live regardless. A failed directory fsync only
         * risks losing the directory entry to an immediate power cut. */
        DB_LOG("DB", "write_gen_pointer fsync_dir_failed gen=%d errno=%d(%s) dir=%s (non-fatal, rename already committed)",
               gen, errno, strerror(errno), db_dir);
    }
    return true;
}

int read_gen_pointer(int32_t * out) {
    char path[640];
    db_path(path, sizeof(path), "tagcache.gen");
    errno = 0;
    FILE * f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    bool read_error = ferror(f) != 0;
    if (fclose(f) != 0) read_error = true;
    if (read_error) return -1;
    if (n == sizeof(buf) - 1 || memchr(buf, '\0', n)) return -1;
    buf[n] = '\0';
    char * end;
    errno = 0;
    long gen = strtol(buf, &end, 10);
    if (end == buf || errno == ERANGE || gen <= 0 || gen > INT32_MAX) return -1;
    while (isspace((unsigned char) *end)) end++;
    if (*end) return -1;
    *out = (int32_t) gen;
    return 1;
}

void unlink_generation(int32_t gen) {
    reader_unlink_indexes(gen);
    char path[640];
    for (size_t t = 0; t < TAGCACHE_REFS_COUNT; t++) {
        tag_file_path(path, sizeof(path), persist_tag_ids[t], gen);
        unlink(path);
    }
    master_file_path(path, sizeof(path), gen);
    unlink(path);
    refs_file_path(path, sizeof(path), gen);
    unlink(path);
    char tmp[680];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink(tmp);
}

void unlink_other_generations(int32_t keep, int32_t previous) {
    char protected_names[TAGCACHE_REFS_COUNT * 2][80];
    size_t protected_count = 0;
    int32_t generations[2] = {keep, previous};
    for (size_t g = 0; g < 2; g++) {
        if (generations[g] < 0 || (g == 1 && generations[1] == generations[0])) continue;
        for (size_t i = 0; i < TAGCACHE_REFS_COUNT; i++) {
            int32_t source;
            if (!resolve_tag_generation(generations[g], persist_tag_ids[i], &source)) return;
            tag_file_name(protected_names[protected_count++], sizeof(protected_names[0]), persist_tag_ids[i], source);
        }
    }
    DIR *d = opendir(db_dir);
    if (!d) return;
    struct dirent * de;
    while ((de = readdir(d)) != NULL) {
        const char * name = de->d_name;
        char path[800];
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strcmp(name, "tagcache.gen") == 0 || strcmp(name, "tagcache.gen.tmp") == 0) continue;
        bool protected = false;
        for (size_t i = 0; i < protected_count; i++)
            if (strcmp(name, protected_names[i]) == 0) { protected = true; break; }
        if (protected) continue;
        static const char refs_prefix[] = "tagcache.refs.g";
        if (strncmp(name, refs_prefix, sizeof(refs_prefix) - 1) == 0) {
            char *end;
            long parsed = strtol(name + sizeof(refs_prefix) - 1, &end, 10);
            if (*end == '\0' && parsed > 0 && parsed <= INT32_MAX &&
                (parsed == keep || parsed == previous)) continue;
            if (db_path(path, sizeof(path), name)) unlink(path);
            continue;
        }
        const char *gpos = strstr(name, ".tcd.g");
        if (gpos) {
            int gen = atoi(gpos + 6);
            if ((keep > 0 && gen == keep) || (previous > 0 && gen == previous)) continue;
            if (db_path(path, sizeof(path), name)) unlink(path);
            continue;
        }
        size_t n = strlen(name);
        if (n >= 4 && strcmp(name + n - 4, ".new") == 0) {
            if (db_path(path, sizeof(path), name)) unlink(path);
            continue;
        }
        if (keep > 0 && (strcmp(name, "database_idx.tcd") == 0 ||
                         (strncmp(name, "database_", 9) == 0 && n >= 4 && strcmp(name + n - 4, ".tcd") == 0))) {
            if (db_path(path, sizeof(path), name)) unlink(path);
        }
    }
    closedir(d);
}

/* Find the highest generation number in every generation file, including
 * partial tag-only generations left by an interrupted write. */
static bool parse_generation(const char *s, int32_t *out) {
    char *end;
    errno = 0;
    long gen = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || gen <= 0 || gen > INT32_MAX) return false;
    *out = (int32_t)gen;
    return true;
}

bool scan_generation_max(int32_t * out_max) {
    DIR * d = opendir(db_dir);
    if (!d) return false;
    int32_t max_gen = 0;
    bool ok = true;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(d);
        if (!de) {
            if (errno != 0) ok = false;
            break;
        }
        const char * name = de->d_name;
        const char * gpos = strstr(name, ".tcd.g");
        if (!gpos || strncmp(name, "database_", 9) != 0) continue;
        int32_t gen;
        /* Invalid suffixes cannot collide with a generated filename. */
        if (!parse_generation(gpos + 6, &gen)) continue;
        if (gen > max_gen) max_gen = gen;
    }
    if (closedir(d) != 0) ok = false;
    if (!ok) return false;
    *out_max = max_gen;
    return true;
}

bool validate_master_header(const struct master_header * mh, size_t file_size) {
    if (!mh) return false;
    if (mh->tch.magic != TAGCACHE_MAGIC && mh->tch.magic != TAGCACHE_INDEXED_MAGIC) return false;
    if (mh->dirty != 0) return false;
    if (mh->tch.entry_count < 0 || mh->tch.entry_count > TAGCACHE_MAX_ENTRIES) return false;
    if (mh->tch.datasize < 0) return false;
    size_t payload_bytes = 0;
    if (!checked_mul_size((size_t) mh->tch.entry_count, sizeof(struct index_entry), &payload_bytes)) return false;
    size_t min_file_size = 0;
    if (!checked_add_size(sizeof(struct master_header), payload_bytes, &min_file_size)) return false;
    if (file_size < min_file_size) return false;
    if (mh->tch.datasize > 0 && (size_t) mh->tch.datasize != payload_bytes) return false;
    return true;
}

bool validate_tag_header(const struct tagcache_header * hdr, size_t file_size) {
    if (!hdr) return false;
    if (hdr->magic != TAGCACHE_MAGIC) return false;
    if (hdr->entry_count < 0 || hdr->entry_count > TAGCACHE_MAX_ENTRIES) return false;
    if (hdr->datasize < 0) return false;
    if (file_size < sizeof(struct tagcache_header)) return false;
    if (hdr->datasize > 0) {
        size_t min_size = 0;
        if (!checked_add_size(sizeof(struct tagcache_header), (size_t) hdr->datasize, &min_size)) return false;
        if (file_size < min_size) return false;
    }
    return true;
}

static int compare_load_generations(const void * a, const void * b) {
    int32_t ga = *(const int32_t *) a;
    int32_t gb = *(const int32_t *) b;
    return (ga < gb) - (ga > gb);
}

bool collect_load_generations(int32_t ** out, size_t * count, int32_t pointed,
                                     bool * saved_files, bool * pointer_file) {
    DIR * d = opendir(db_dir);
    if (!d) return false;
    int32_t * gens = NULL;
    size_t n = 0, cap = 0;
    bool ok = true;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(d);
        if (!de) {
            if (errno != 0) ok = false;
            break;
        }
        const char * name = de->d_name;
        if (strcmp(name, "tagcache.gen") == 0) *pointer_file = true;
        if (strncmp(name, "tagcache.gen", 12) == 0 ||
            (strncmp(name, "database_", 9) == 0 && strstr(name, ".tcd")))
            *saved_files = true;
        int32_t gen;
        if (strcmp(name, "database_idx.tcd") == 0) {
            gen = 0;
        } else {
            const char * prefix = "database_idx.tcd.g";
            size_t len = strlen(prefix);
            if (strncmp(name, prefix, len) != 0) continue;
            if (!parse_generation(name + len, &gen)) continue;
        }
        if (gen == pointed && pointed > 0) continue;
        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 48;
            if (new_cap < cap || new_cap > SIZE_MAX / sizeof(*gens)) {
                ok = false;
                break;
            }
            int32_t * grown = realloc(gens, new_cap * sizeof(*gens));
            if (!grown) {
                ok = false;
                break;
            }
            gens = grown;
            cap = new_cap;
        }
        gens[n++] = gen;
    }
    if (closedir(d) != 0) ok = false;
    if (!ok) {
        free(gens);
        return false;
    }
    if (n > 1) qsort(gens, n, sizeof(*gens), compare_load_generations);
    *out = gens;
    *count = n;
    return true;
}
