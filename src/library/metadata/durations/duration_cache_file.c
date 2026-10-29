#include "duration_cache.h"
#include "duration_cache_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DURATION_FILE_VERSION 1u
#define DURATION_HEADER_BYTES 16u
#define DURATION_RECORD_BYTES 24u

static uint32_t fnv32(const uint8_t * p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static void put_u32(uint8_t * p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t) (v >> (8 * i)); }
static void put_u64(uint8_t * p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t) (v >> (8 * i)); }
static uint32_t get_u32(const uint8_t * p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t get_u64(const uint8_t * p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

static void pack(uint8_t * p, const dc_entry_t * e) {
    put_u64(p, e->key);
    put_u32(p + 8, e->duration_ms);
    put_u32(p + 12, e->sample_rate);
    p[16] = e->bit_depth; p[17] = e->codec; p[18] = e->flags; p[19] = 0;
    put_u32(p + 20, fnv32(p, 20));
}

static bool unpack(const uint8_t * p, dc_entry_t * e) {
    if (get_u32(p + 20) != fnv32(p, 20)) return false;
    e->key = get_u64(p);
    e->duration_ms = get_u32(p + 8);
    e->sample_rate = get_u32(p + 12);
    e->bit_depth = p[16]; e->codec = p[17]; e->flags = p[18];
    if (e->flags & DURATION_CACHE_FLAG_FAILED) return e->key != 0 && e->duration_ms == 0;
    return e->key != 0 && e->duration_ms > 0;
}

int dc_cmp_entry(const void * a, const void * b) {
    uint64_t x = ((const dc_entry_t *) a)->key, y = ((const dc_entry_t *) b)->key;
    return x < y ? -1 : x > y;
}

dc_table_t * dc_table_alloc(size_t count) {
    dc_table_t * t = malloc(sizeof(*t) + count * sizeof(dc_entry_t));
    if (t) { t->count = 0; t->refs = 1; }
    return t;
}

static dc_table_t * parse_file(const uint8_t * data, size_t size) {
    if (size < DURATION_HEADER_BYTES || memcmp(data, "CDUR", 4) != 0 ||
        get_u32(data + 4) != DURATION_FILE_VERSION || get_u32(data + 12) != fnv32(data, 12)) return NULL;
    uint32_t count = get_u32(data + 8);
    if (count > DURATION_CACHE_MAX_ENTRIES ||
        size != DURATION_HEADER_BYTES + (size_t) count * DURATION_RECORD_BYTES) return NULL;
    dc_table_t * t = dc_table_alloc((size_t) count + 1);
    if (!t) return NULL;
    size_t n = 0;
    for (uint32_t i = 0; i < count; i++)
        if (unpack(data + DURATION_HEADER_BYTES + (size_t) i * DURATION_RECORD_BYTES, &t->e[n])) n++;
    qsort(t->e, n, sizeof(dc_entry_t), dc_cmp_entry);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) if (w == 0 || t->e[i].key != t->e[w - 1].key) t->e[w++] = t->e[i];
    t->count = w;
    return t;
}

void dc_store_close(dc_store_t * store) {
    if (store->dirfd >= 0) close(store->dirfd);
    store->dirfd = -1;
}

dc_load_status_t dc_file_load(const char * path, dc_store_t * store, dc_table_t ** table) {
    *table = NULL;
    store->dirfd = -1;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char * slash = strrchr(dir, '/');
    const char * name = slash ? slash + 1 : path;
    if (strlen(name) >= sizeof(store->name)) return DC_LOAD_FAILED;
    snprintf(store->name, sizeof(store->name), "%s", name);
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");
    if (slash && slash == dir) snprintf(dir, sizeof(dir), "/");
    store->dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (store->dirfd < 0 && errno == ENOENT && mkdir(dir, 0755) == 0)
        store->dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    /* No directory handle: the table can still be read, but never written. */
    int fd = store->dirfd >= 0 ? openat(store->dirfd, store->name, O_RDONLY | O_CLOEXEC)
                               : open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? DC_LOAD_EMPTY : DC_LOAD_FAILED;
    dc_load_status_t status = DC_LOAD_FAILED;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        if (st.st_size == 0 ||
            (uint64_t) st.st_size > DURATION_HEADER_BYTES + (uint64_t) DURATION_CACHE_MAX_ENTRIES * DURATION_RECORD_BYTES) {
            status = DC_LOAD_EMPTY; /* not a file we wrote */
        } else {
            uint8_t * data = malloc((size_t) st.st_size);
            size_t got = 0;
            while (data && got < (size_t) st.st_size) {
                ssize_t r = read(fd, data + got, (size_t) st.st_size - got);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) break;
                got += (size_t) r;
            }
            if (data && got == (size_t) st.st_size) {
                *table = parse_file(data, got);
                status = *table ? DC_LOAD_OK : DC_LOAD_EMPTY;
            }
            free(data);
        }
    }
    close(fd);
    return status;
}

/* Packs the merge of `base` and `extra` straight into `out`; returns the count. */
static size_t pack_merged(uint8_t * out, const dc_table_t * base, const dc_entry_t * extra, size_t extra_count) {
    size_t i = 0, j = 0, n = 0, base_count = base ? base->count : 0;
    while (i < base_count || j < extra_count) {
        const dc_entry_t * pick;
        if (j >= extra_count || (i < base_count && base->e[i].key < extra[j].key)) pick = &base->e[i++];
        else if (i >= base_count || extra[j].key < base->e[i].key) pick = &extra[j++];
        else { pick = &extra[j++]; i++; }
        pack(out + DURATION_HEADER_BYTES + n * DURATION_RECORD_BYTES, pick);
        n++;
    }
    return n;
}

bool dc_file_write(int dirfd, const char * name, const dc_table_t * base, const dc_entry_t * extra,
                   size_t extra_count, bool (*still_current)(void * ctx), void * ctx) {
    if (dirfd < 0 || !name) return false;
    size_t upper = (base ? base->count : 0) + extra_count;
    uint8_t * buffer = malloc(DURATION_HEADER_BYTES + upper * DURATION_RECORD_BYTES + 1);
    if (!buffer) return false;
    size_t count = pack_merged(buffer, base, extra, extra_count);
    memcpy(buffer, "CDUR", 4);
    put_u32(buffer + 4, DURATION_FILE_VERSION);
    put_u32(buffer + 8, (uint32_t) count);
    put_u32(buffer + 12, fnv32(buffer, 12));
    size_t size = DURATION_HEADER_BYTES + count * DURATION_RECORD_BYTES;

    bool ok = false;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", name);
    if (still_current && !still_current(ctx)) { free(buffer); return false; }
    int fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        size_t done = 0;
        ok = true;
        while (done < size) {
            ssize_t w = write(fd, buffer + done, size - done);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = false; break; }
            done += (size_t) w;
        }
        if (ok && fsync(fd) != 0) ok = false;
        if (close(fd) != 0) ok = false;
        if (ok && still_current && !still_current(ctx)) ok = false; /* the card changed while writing */
        if (ok && renameat(dirfd, tmp, dirfd, name) != 0) ok = false;
        if (!ok) unlinkat(dirfd, tmp, 0);
        if (ok) (void) fsync(dirfd);
    }
    free(buffer);
    return ok;
}
