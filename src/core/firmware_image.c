#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "firmware_image.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mbedtls/md5.h"

#define ISO_SECTOR 2048U
#define IMAGE_MIN (1024U * 1024U)
#define IMAGE_MAX (45U * 1024U * 1024U)
#define CHUNK_SIZE (512U * 1024U)
#define MAX_ENTRIES 160
#define MAX_NAME 256

typedef struct {
    char name[MAX_NAME];
    uint32_t extent;
    uint32_t size;
    unsigned char flags;
    bool directory;
    bool used;
} iso_entry_t;

typedef struct {
    uint64_t offset;
    uint32_t size;
} iso_dir_t;

typedef struct {
    char name[32];
    uint64_t size;
    char md5[33];
} image_spec_t;

typedef enum { ISO_NAMES_PRIMARY, ISO_NAMES_JOLIET } iso_names_t;

static void fail(char *error, size_t n, const char *fmt, ...) {
    if (!error || n == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, n, fmt, ap);
    va_end(ap);
}

static uint16_t le16(const unsigned char *p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static bool read_at(int fd, uint64_t off, void *buffer, size_t n) {
    unsigned char *p = buffer;
    while (n) {
        ssize_t got = pread(fd, p, n, (off_t) off);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p += got;
        off += (uint64_t) got;
        n -= (size_t) got;
    }
    return true;
}

static bool record_extent(const unsigned char *r, size_t n, uint64_t file_size,
                          iso_dir_t *out) {
    if (n < 34 || r[1] != 0 || r[26] != 0 || r[27] != 0 ||
        le32(r + 2) != be32(r + 6) || le32(r + 10) != be32(r + 14)) return false;
    out->offset = (uint64_t) le32(r + 2) * ISO_SECTOR;
    out->size = le32(r + 10);
    return out->offset <= file_size && out->size <= file_size - out->offset;
}

/* Recovery may mount the Rock Ridge view. Inspect all attributes, including
 * those after NM and in CE, so a symlink/device cannot masquerade as a file. */
static bool rr_name_from_sua(int fd, uint64_t file_size, const unsigned char *r,
                             size_t rec_len, char out[MAX_NAME]) {
    size_t id_len = r[32];
    size_t sua = 33U + id_len + ((id_len & 1U) == 0 ? 1U : 0U);
    uint64_t ce_off = 0;
    uint32_t ce_len = 0;
    bool have_name = false, have_px = false;
    bool have_rr = false, have_tf = false;
    unsigned rr_flags = 0;
    unsigned depth = 0;
    if (sua > rec_len) return false;
    while (depth++ < 5) {
        const unsigned char *p = r + sua;
        size_t len = rec_len - sua;
        unsigned char continuation[2048];
        if (ce_len) {
            if (ce_len > sizeof(continuation) || !read_at(fd, ce_off, continuation, ce_len)) return false;
            p = continuation; len = ce_len; ce_len = 0;
        }
        for (size_t at = 0; at + 4 <= len;) {
            unsigned sl = p[at + 2];
            if (sl < 4 || at + sl > len || p[at + 3] != 1) return false;
            if ((p[at] == 'S' && (p[at + 1] == 'L' || p[at + 1] == 'F')) ||
                (p[at] == 'C' && p[at + 1] == 'L') || (p[at] == 'P' && p[at + 1] == 'L') ||
                (p[at] == 'R' && p[at + 1] == 'E') || (p[at] == 'Z' && p[at + 1] == 'F')) return false;
            if (p[at] == 'S' && p[at + 1] == 'P' &&
                (sl != 7 || p[at + 4] != 0xbe || p[at + 5] != 0xef || p[at + 6] != 0)) return false;
            if (p[at] == 'R' && p[at + 1] == 'R') {
                if (sl != 5 || have_rr) return false;
                rr_flags = p[at + 4];
                /* isofs uses RR_NM to decide whether to read NM at all.
                 * Unsupported link/device extensions must not be advertised. */
                if (rr_flags & 0x76U) return false;
                have_rr = true;
            }
            if (p[at] == 'T' && p[at + 1] == 'F') {
                if (have_tf || sl < 5) return false;
                have_tf = true;
            }
            if (p[at] == 'N' && p[at + 1] == 'M') {
                if (sl < 6 || p[at + 4] != 0 || have_name) return false;
                size_t n = sl - 5;
                if (n >= MAX_NAME) return false;
                for (size_t i = 0; i < n; ++i) {
                    unsigned char c = p[at + 5 + i];
                    if (c < 0x20 || c > 0x7e || c == '/') return false;
                    out[i] = (char) c;
                }
                out[n] = 0;
                have_name = true;
            }
            if (p[at] == 'P' && p[at + 1] == 'X') {
                if ((sl != 36 && sl != 44) || have_px || le32(p + at + 4) != be32(p + at + 8)) return false;
                unsigned mode = le32(p + at + 4) & S_IFMT;
                if (mode != ((r[25] & 2U) ? S_IFDIR : S_IFREG)) return false;
                have_px = true;
            }
            if (p[at] == 'C' && p[at + 1] == 'E') {
                if (sl != 28 || ce_len) return false;
                uint32_t block = le32(p + at + 4), offset = le32(p + at + 12);
                ce_len = le32(p + at + 20);
                if (block != be32(p + at + 8) || offset != be32(p + at + 16) ||
                    ce_len != be32(p + at + 24) || ce_len == 0 || ce_len > sizeof(continuation)) return false;
                ce_off = (uint64_t) block * ISO_SECTOR + offset;
                if (ce_off > file_size || ce_len > file_size - ce_off) return false;
            }
            /* The stock kernel keeps scanning after ST. This packer never
             * emits it; reject it rather than validate a different view. */
            if (p[at] == 'S' && p[at + 1] == 'T') return false;
            at += sl;
        }
        if (!ce_len) return have_rr && have_px &&
            ((rr_flags & 0x01U) != 0) &&
            (((rr_flags & 0x08U) != 0) == have_name) &&
            (((rr_flags & 0x80U) != 0) == have_tf);
    }
    return false;
}

static bool parse_directory(int fd, uint64_t file_size, iso_dir_t dir, iso_names_t names,
                            iso_entry_t *entries, size_t *count,
                            char *error, size_t error_size) {
    unsigned char sector[ISO_SECTOR];
    uint64_t pos = 0;
    while (pos < dir.size) {
        uint64_t sec_pos = pos - (pos % ISO_SECTOR);
        if (!read_at(fd, dir.offset + sec_pos, sector, ISO_SECTOR)) {
            fail(error, error_size, "ISO directory is truncated"); return false;
        }
        size_t in_sector = (size_t) (pos % ISO_SECTOR);
        unsigned rec_len = sector[in_sector];
        if (rec_len == 0) {
            pos = sec_pos + ISO_SECTOR;
            continue;
        }
        if (rec_len < 34 || in_sector + rec_len > ISO_SECTOR || pos + rec_len > dir.size) {
            fail(error, error_size, "malformed ISO directory record"); return false;
        }
        const unsigned char *r = sector + in_sector;
        unsigned id_len = r[32];
        if (33U + id_len > rec_len) { fail(error, error_size, "malformed ISO filename"); return false; }
        if (id_len == 1 && (r[33] == 0 || r[33] == 1)) {
            char ignored[MAX_NAME] = "";
            if (names == ISO_NAMES_PRIMARY && !rr_name_from_sua(fd, file_size, r, rec_len, ignored)) {
                fail(error, error_size, "unsupported Rock Ridge directory attributes"); return false;
            }
            pos += rec_len; continue;
        }
        if (*count >= MAX_ENTRIES) { fail(error, error_size, "too many ISO directory entries"); return false; }
        iso_entry_t *e = &entries[(*count)++];
        memset(e, 0, sizeof(*e));
        if (names == ISO_NAMES_JOLIET) {
            if ((id_len & 1U) != 0 || id_len / 2 >= MAX_NAME) {
                fail(error, error_size, "unsupported Joliet filename"); return false;
            }
            for (unsigned i = 0; i < id_len; i += 2) {
                if (r[33 + i] != 0 || r[34 + i] < 0x20 || r[34 + i] > 0x7e) {
                    fail(error, error_size, "non-ASCII Joliet filename"); return false;
                }
                e->name[i / 2] = (char) r[34 + i];
            }
        } else {
            if (id_len >= MAX_NAME) { fail(error, error_size, "unsupported primary filename"); return false; }
            memcpy(e->name, r + 33, id_len); e->name[id_len] = 0;
            char *version = strrchr(e->name, ';');
            if (version && version[1] == '1' && version[2] == 0) *version = 0;
            char rr_name[MAX_NAME] = {0};
            if (!rr_name_from_sua(fd, file_size, r, rec_len, rr_name)) {
                /* A continuation without a usable NM is unsupported for these packages. */
                fail(error, error_size, "unsupported primary Rock Ridge name"); return false;
            }
            if (rr_name[0]) memcpy(e->name, rr_name, strlen(rr_name) + 1);
        }
        if (r[1] != 0 || r[26] != 0 || r[27] != 0) {
            fail(error, error_size, "unsupported ISO XAR or interleaved entry"); return false;
        }
        e->directory = (r[25] & 2U) != 0;
        e->flags = r[25];
        if ((r[25] & 0x80U) != 0 || !record_extent(r, rec_len, file_size,
                &(iso_dir_t){0})) {
            fail(error, error_size, "invalid or multi-extent ISO entry"); return false;
        }
        e->extent = le32(r + 2);
        e->size = le32(r + 10);
        pos += rec_len;
    }
    return true;
}

static iso_entry_t *entry_find(iso_entry_t *entries, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

static bool equivalent_listing(iso_entry_t *a, size_t an, iso_entry_t *b, size_t bn,
                               char *error, size_t error_size) {
    if (an != bn) { fail(error, error_size, "primary and Joliet directory listings differ"); return false; }
    for (size_t i = 0; i < an; ++i) {
        iso_entry_t *other = entry_find(b, bn, a[i].name);
        if (!other || a[i].directory != other->directory || a[i].flags != other->flags ||
            (!a[i].directory && (a[i].size != other->size || a[i].extent != other->extent))) {
            fail(error, error_size, "primary and Joliet entries differ: %s", a[i].name); return false;
        }
        for (size_t j = i + 1; j < an; ++j) {
            if (strcmp(a[i].name, a[j].name) == 0) {
                fail(error, error_size, "duplicate ISO entry: %s", a[i].name); return false;
            }
        }
    }
    return true;
}

static bool get_roots(int fd, uint64_t file_size, iso_dir_t *primary, iso_dir_t *joliet,
                            char *error, size_t error_size) {
    unsigned char d[ISO_SECTOR];
    bool have_primary = false, have_joliet = false, ended = false;
    for (uint32_t s = 16; s < 64; ++s) {
        if (!read_at(fd, (uint64_t) s * ISO_SECTOR, d, sizeof(d))) break;
        if (memcmp(d + 1, "CD001", 5) != 0 || d[6] != 1) break;
        if (d[0] == 255) { ended = true; break; }
        if (d[0] == 1) {
            if (have_primary || le16(d + 128) != ISO_SECTOR ||
                ((uint16_t) d[130] << 8 | d[131]) != ISO_SECTOR ||
                le32(d + 80) != be32(d + 84) ||
                (uint64_t) le32(d + 80) * ISO_SECTOR != file_size ||
                !record_extent(d + 156, sizeof(d) - 156, file_size, primary)) {
                fail(error, error_size, "invalid primary volume descriptor"); return false;
            }
            have_primary = true;
        }
        if (d[0] == 2 && d[88] == '%' && d[89] == '/' &&
            (d[90] == '@' || d[90] == 'C' || d[90] == 'E')) {
            if (have_joliet || le16(d + 128) != ISO_SECTOR ||
                ((uint16_t) d[130] << 8 | d[131]) != ISO_SECTOR ||
                le32(d + 80) != be32(d + 84) ||
                (uint64_t) le32(d + 80) * ISO_SECTOR != file_size ||
                !record_extent(d + 156, sizeof(d) - 156, file_size, joliet)) {
                fail(error, error_size, "invalid Joliet volume descriptor"); return false;
            }
            have_joliet = true;
        }
    }
    if (!have_primary || !have_joliet || !ended) { fail(error, error_size, "ISO9660 primary/Joliet descriptor missing or truncated"); return false; }
    /* Without SP at the primary root, recovery can ignore all Rock Ridge
     * names and see the mangled ISO identifiers instead of those checked. */
    static const unsigned char sp[] = {'S', 'P', 7, 1, 0xbe, 0xef, 0};
    if (primary->size < 41 || !read_at(fd, primary->offset, d, 41) ||
        d[0] < 41 || d[32] != 1 || d[33] != 0 || memcmp(d + 34, sp, sizeof(sp))) {
        fail(error, error_size, "Rock Ridge root registration missing or invalid"); return false;
    }
    return true;
}

static bool read_iso_file(int fd, uint64_t image_size, const iso_entry_t *e,
                          unsigned char *buf, size_t cap, size_t *out_size) {
    uint64_t off = (uint64_t) e->extent * ISO_SECTOR;
    if (e->directory || e->size > cap || off > image_size || e->size > image_size - off) return false;
    if (!read_at(fd, off, buf, e->size)) return false;
    *out_size = e->size;
    return true;
}

static bool expect_line(const unsigned char *data, size_t len, size_t *at,
                        const char *expected) {
    size_t n = strlen(expected);
    if (*at + n + 1 > len || memcmp(data + *at, expected, n) != 0 || data[*at + n] != '\n') return false;
    *at += n + 1;
    return true;
}

static bool expect_prefix(const unsigned char *data, size_t len, size_t *at,
                          const char *prefix) {
    size_t n = strlen(prefix);
    if (*at + n > len || memcmp(data + *at, prefix, n) != 0) return false;
    *at += n;
    return true;
}

static bool parse_decimal(const unsigned char *data, size_t len, size_t *at, uint64_t *value) {
    uint64_t v = 0;
    size_t start = *at;
    while (*at < len && data[*at] >= '0' && data[*at] <= '9') {
        unsigned digit = data[(*at)++] - '0';
        if (v > (UINT64_MAX - digit) / 10) return false;
        v = v * 10 + digit;
    }
    if (*at == start || *at >= len || data[*at] != '\n') return false;
    ++*at;
    *value = v;
    return true;
}

static bool parse_md5_line(const unsigned char *data, size_t len, size_t *at, char out[33]) {
    if (*at + 33 > len) return false;
    for (size_t i = 0; i < 32; ++i) {
        unsigned char c = data[(*at)++];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        out[i] = (char) c;
    }
    out[32] = 0;
    if (data[(*at)++] != '\n') return false;
    return true;
}

static bool parse_manifest(const unsigned char *buf, size_t len, image_spec_t spec[2]) {
    size_t at = 0;
    const char *names[2] = {"xImage", "rootfs.squashfs"};
    if (!expect_line(buf, len, &at, "ota_version=0")) return false;
    for (size_t i = 0; i < 2; ++i) {
        char line[64];
        snprintf(line, sizeof(line), "img_type=%s", i == 0 ? "kernel" : "rootfs");
        if (!expect_line(buf, len, &at, line)) return false;
        snprintf(line, sizeof(line), "img_name=%s", names[i]);
        if (!expect_line(buf, len, &at, line)) return false;
        if (!expect_prefix(buf, len, &at, "img_size=") || !parse_decimal(buf, len, &at, &spec[i].size)) return false;
        if (!expect_prefix(buf, len, &at, "img_md5=") || !parse_md5_line(buf, len, &at, spec[i].md5)) return false;
        strcpy(spec[i].name, names[i]);
        if (spec[i].size == 0 || spec[i].size > IMAGE_MAX) return false;
    }
    return at == len;
}

static void md5_hex(const unsigned char digest[16], char out[33]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[32] = 0;
}

static bool validate_payload(int fd, uint64_t image_size, iso_entry_t *entries,
                             size_t count, const image_spec_t *spec,
                             char *error, size_t error_size) {
    char chain_name[96], chunk_name[128], actual[33], previous[33];
    unsigned char text[8192], buf[4096], digest[16], chunk_digest[16];
    mbedtls_md5_context ctx, chunk_ctx;
    size_t text_len = 0, at = 0;
    uint64_t total = 0;
    uint32_t chunks = (uint32_t) ((spec->size + CHUNK_SIZE - 1) / CHUNK_SIZE);
    snprintf(chain_name, sizeof(chain_name), "ota_md5_%s.%s", spec->name, spec->md5);
    iso_entry_t *chain = entry_find(entries, count, chain_name);
    if (!chain || chain->directory || !read_iso_file(fd, image_size, chain, text, sizeof(text), &text_len)) {
        fail(error, error_size, "missing or invalid chunk hash list for %s", spec->name); return false;
    }
    strcpy(previous, spec->md5);
    mbedtls_md5_init(&ctx);
    if (mbedtls_md5_starts(&ctx) != 0) { mbedtls_md5_free(&ctx); fail(error,error_size,"MD5 initialization failed"); return false; }
    for (uint32_t i = 0; i < chunks; ++i) {
        size_t expected_size = (size_t) ((spec->size - total) < CHUNK_SIZE ? spec->size - total : CHUNK_SIZE);
        snprintf(chunk_name, sizeof(chunk_name), "%s.%04u.%s", spec->name, i, previous);
        iso_entry_t *part = entry_find(entries, count, chunk_name);
        if (!part || part->directory || part->size != expected_size) {
            mbedtls_md5_free(&ctx); fail(error, error_size, "missing, misnamed, or wrong-sized %s chunk %u", spec->name, i); return false;
        }
        mbedtls_md5_init(&chunk_ctx);
        if (mbedtls_md5_starts(&chunk_ctx) != 0) {
            mbedtls_md5_free(&chunk_ctx); mbedtls_md5_free(&ctx);
            fail(error, error_size, "MD5 initialization failed"); return false;
        }
        uint64_t off = (uint64_t) part->extent * ISO_SECTOR;
        uint64_t left = part->size;
        while (left) {
            size_t n = left < sizeof(buf) ? (size_t) left : sizeof(buf);
            if (!read_at(fd, off, buf, n) || mbedtls_md5_update(&chunk_ctx, buf, n) != 0 ||
                mbedtls_md5_update(&ctx, buf, n) != 0) {
                mbedtls_md5_free(&chunk_ctx); mbedtls_md5_free(&ctx);
                fail(error, error_size, "could not read %s chunk %u", spec->name, i); return false;
            }
            off += n; left -= n;
        }
        if (mbedtls_md5_finish(&chunk_ctx, chunk_digest) != 0) {
            mbedtls_md5_free(&chunk_ctx); mbedtls_md5_free(&ctx);
            fail(error, error_size, "MD5 finalization failed"); return false;
        }
        mbedtls_md5_free(&chunk_ctx);
        md5_hex(chunk_digest, actual);
        if (at + 33 > text_len || memcmp(text + at, actual, 32) != 0 || text[at + 32] != '\n') {
            mbedtls_md5_free(&ctx); fail(error, error_size, "%s chunk hash list mismatch at chunk %u", spec->name, i); return false;
        }
        at += 33;
        strcpy(previous, actual);
        total += part->size;
        part->used = true;
    }
    if (at != text_len || total != spec->size || mbedtls_md5_finish(&ctx, digest) != 0) {
        mbedtls_md5_free(&ctx); fail(error, error_size, "%s hash list or total size is invalid", spec->name); return false;
    }
    mbedtls_md5_free(&ctx);
    md5_hex(digest, actual);
    if (strcmp(actual, spec->md5) != 0) { fail(error, error_size, "%s whole-image checksum mismatch", spec->name); return false; }
    chain->used = true;
    return true;
}

bool firmware_image_validate(const char *path, char *error, size_t error_size) {
    int fd = -1;
    struct stat st, after, named;
    iso_dir_t root, primary_root;
    iso_entry_t *top = NULL, *inner = NULL, *primary_top = NULL, *primary_inner = NULL;
    size_t top_count = 0, inner_count = 0, n = 0;
    unsigned char buf[8192];
    uint64_t image_size;
    bool ok = false;
    if (error && error_size) error[0] = 0;
    if (!path || !*path) { fail(error, error_size, "empty firmware image path"); return false; }
    top = calloc(MAX_ENTRIES * 4U, sizeof(*top));
    if (!top) { fail(error, error_size, "cannot allocate bounded ISO directory tables"); goto done; }
    inner = top + MAX_ENTRIES;
    primary_top = inner + MAX_ENTRIES;
    primary_inner = primary_top + MAX_ENTRIES;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { fail(error, error_size, "cannot open firmware image: %s", strerror(errno)); goto done; }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { fail(error, error_size, "firmware image is not a regular file"); goto done; }
    if (st.st_size < IMAGE_MIN || st.st_size > IMAGE_MAX || stat(path, &named) != 0 ||
        named.st_dev != st.st_dev || named.st_ino != st.st_ino) {
        fail(error, error_size, "firmware image size or pathname identity is invalid (expected 1-45 MiB)"); goto done;
    }
    image_size = (uint64_t) st.st_size;
    if (!get_roots(fd, image_size, &primary_root, &root, error, error_size)) goto done;
    if (!parse_directory(fd, image_size, root, ISO_NAMES_JOLIET, top, &top_count, error, error_size) ||
        !parse_directory(fd, image_size, primary_root, ISO_NAMES_PRIMARY, primary_top, &inner_count, error, error_size) ||
        !equivalent_listing(primary_top, inner_count, top, top_count, error, error_size)) goto done;
    size_t primary_top_count = inner_count;
    inner_count = 0;
    iso_entry_t *config = entry_find(top, top_count, "ota_config.in");
    iso_entry_t *ota = entry_find(top, top_count, "ota_v0");
    if (top_count != 2 || !config || config->directory || !ota || !ota->directory) {
        fail(error, error_size, "unexpected top-level ISO records (expected ota_config.in and ota_v0)"); goto done;
    }
    if (!read_iso_file(fd, image_size, config, buf, sizeof(buf), &n) || n != 18 ||
        memcmp(buf, "current_version=0\n", 18) != 0) {
        fail(error, error_size, "invalid ota_config.in"); goto done;
    }
    iso_dir_t ota_dir = {(uint64_t) ota->extent * ISO_SECTOR, ota->size};
    iso_entry_t *primary_ota = entry_find(primary_top, primary_top_count, "ota_v0");
    if (!primary_ota || !primary_ota->directory) { fail(error, error_size, "primary ota_v0 directory missing"); goto done; }
    iso_dir_t primary_ota_dir = {(uint64_t) primary_ota->extent * ISO_SECTOR, primary_ota->size};
    size_t primary_inner_count = 0;
    if (!parse_directory(fd, image_size, ota_dir, ISO_NAMES_JOLIET, inner, &inner_count, error, error_size) ||
        !parse_directory(fd, image_size, primary_ota_dir, ISO_NAMES_PRIMARY, primary_inner, &primary_inner_count, error, error_size) ||
        !equivalent_listing(primary_inner, primary_inner_count, inner, inner_count, error, error_size)) goto done;
    iso_entry_t *manifest = entry_find(inner, inner_count, "ota_update.in");
    iso_entry_t *ok_marker = entry_find(inner, inner_count, "ota_v0.ok");
    if (!manifest || manifest->directory || !ok_marker || ok_marker->directory || ok_marker->size != 0 ||
        !read_iso_file(fd, image_size, manifest, buf, sizeof(buf), &n)) {
        fail(error, error_size, "missing or invalid ota_v0 manifest/marker"); goto done;
    }
    manifest->used = true; ok_marker->used = true;
    image_spec_t specs[2] = {{{0}, 0, {0}}, {{0}, 0, {0}}};
    if (!parse_manifest(buf, n, specs)) { fail(error, error_size, "malformed or unsupported ota_update.in"); goto done; }
    if (!validate_payload(fd, image_size, inner, inner_count, &specs[0], error, error_size) ||
        !validate_payload(fd, image_size, inner, inner_count, &specs[1], error, error_size)) goto done;
    for (size_t i = 0; i < inner_count; ++i) {
        if (!inner[i].used) { fail(error, error_size, "unexpected OTA image record: %s", inner[i].name); goto done; }
    }
    if (fstat(fd, &after) != 0 || stat(path, &named) != 0 ||
        after.st_dev != st.st_dev || after.st_ino != st.st_ino || after.st_size != st.st_size ||
        after.st_mtim.tv_sec != st.st_mtim.tv_sec || after.st_mtim.tv_nsec != st.st_mtim.tv_nsec ||
        after.st_ctim.tv_sec != st.st_ctim.tv_sec || after.st_ctim.tv_nsec != st.st_ctim.tv_nsec ||
        named.st_dev != st.st_dev || named.st_ino != st.st_ino || named.st_size != st.st_size ||
        named.st_mtim.tv_sec != st.st_mtim.tv_sec || named.st_mtim.tv_nsec != st.st_mtim.tv_nsec ||
        named.st_ctim.tv_sec != st.st_ctim.tv_sec || named.st_ctim.tv_nsec != st.st_ctim.tv_nsec) {
        fail(error, error_size, "firmware image changed during validation"); goto done;
    }
    ok = true;
done:
    if (fd >= 0) close(fd);
    free(top);
    return ok;
}
