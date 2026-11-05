#define _FILE_OFFSET_BITS 64
#include "caf_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAF_DESC_SIZE 32
#define CAF_MAX_COOKIE 4096
#define CAF_MAX_PACKETS (1u << 24)
#define CAF_MAX_PAKT_TABLE (1u << 26)

struct caf_demux {
    FILE * file;
    char fourcc[5];
    uint32_t frames_per_packet;
    uint8_t * cookie;
    uint32_t cookie_size;
    uint64_t data_start;
    uint64_t data_size;
    int64_t valid_frames;
    int32_t priming;
    uint32_t packet_count;
    uint64_t * offsets; /* packet_count + 1 file offsets, last is the end */
};

static uint32_t be32(const uint8_t * p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static int64_t be64(const uint8_t * p) {
    return (int64_t) (((uint64_t) be32(p) << 32) | be32(p + 4));
}

/* Reads one chunk header: 4-byte type and big-endian int64 size. */
static bool read_chunk_header(FILE * f, char type[4], int64_t * size) {
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12) return false;
    memcpy(type, h, 4);
    *size = be64(h + 4);
    return true;
}

static bool read_desc(FILE * f, char fourcc[5], uint32_t * fpp, uint32_t * bpp) {
    uint8_t h[8], d[CAF_DESC_SIZE];
    char type[4];
    int64_t size;
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "caff", 4) != 0) return false;
    if (!read_chunk_header(f, type, &size) || memcmp(type, "desc", 4) != 0 ||
        size != CAF_DESC_SIZE || fread(d, 1, CAF_DESC_SIZE, f) != CAF_DESC_SIZE) return false;
    memcpy(fourcc, d + 8, 4);
    fourcc[4] = '\0';
    *bpp = be32(d + 16);
    *fpp = be32(d + 20);
    return true;
}

bool caf_demux_peek_codec(const char * path, char out_fourcc[5]) {
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    uint32_t fpp, bpp;
    bool ok = read_desc(f, out_fourcc, &fpp, &bpp);
    fclose(f);
    return ok;
}

/* Reads a whole chunk body into memory, bounded by max. */
static uint8_t * read_body(FILE * f, int64_t size, uint32_t max) {
    if (size < 0 || size > max) return NULL;
    uint8_t * buf = malloc(size ? (size_t) size : 1);
    if (buf && fread(buf, 1, (size_t) size, f) != (size_t) size) { free(buf); return NULL; }
    return buf;
}

/* pakt body: counts, then one BER size per packet. Fills sizes into offsets. */
static bool parse_pakt(caf_demux_t * d, const uint8_t * b, size_t len) {
    if (len < 24) return false;
    int64_t packets = be64(b);
    d->valid_frames = be64(b + 8);
    d->priming = (int32_t) be32(b + 16);
    if (packets < 0 || packets > CAF_MAX_PACKETS || (uint64_t) packets > len - 24 ||
        d->valid_frames < 0 || d->priming < 0) return false;
    d->packet_count = (uint32_t) packets;
    d->offsets = malloc(((size_t) packets + 1) * sizeof(*d->offsets));
    if (!d->offsets) return false;
    size_t pos = 24;
    uint64_t sum = 0;
    for (int64_t i = 0; i < packets; i++) {
        uint64_t v = 0;
        int more = 1;
        for (int n = 0; more && n < 5; n++) {
            if (pos >= len) return false;
            v = (v << 7) | (b[pos] & 0x7F);
            more = b[pos++] & 0x80;
        }
        if (more) return false;
        d->offsets[i] = sum;
        sum += v;
    }
    d->offsets[packets] = sum;
    return true;
}

/* Handles one chunk body; data is only located, not read. */
static bool take_chunk(caf_demux_t * d, const char type[4], int64_t size, int64_t file_left) {
    if (memcmp(type, "data", 4) == 0) {
        if (size == -1) size = file_left;
        if (size < 4) return false;
        d->data_start = (uint64_t) ftello(d->file) + 4;
        d->data_size = (uint64_t) size - 4;
        return true;
    }
    bool is_kuki = memcmp(type, "kuki", 4) == 0, is_pakt = memcmp(type, "pakt", 4) == 0;
    if (!is_kuki && !is_pakt) return true;
    uint8_t * body = read_body(d->file, size, is_kuki ? CAF_MAX_COOKIE : CAF_MAX_PAKT_TABLE);
    if (!body) return false;
    bool ok = true;
    if (is_kuki) {
        free(d->cookie);
        d->cookie = body;
        d->cookie_size = (uint32_t) size;
        return true;
    }
    if (!d->offsets) ok = parse_pakt(d, body, (size_t) size);
    free(body);
    return ok;
}

/* Walks all chunks in file order; the data chunk is skipped over. */
static bool scan_chunks(caf_demux_t * d, int64_t file_size) {
    int64_t pos = 20 + CAF_DESC_SIZE;
    while (pos + 12 <= file_size) {
        char type[4];
        int64_t size;
        if (fseeko(d->file, pos, SEEK_SET) != 0 || !read_chunk_header(d->file, type, &size)) return false;
        pos += 12;
        if (size != -1 && (size < 0 || size > file_size - pos)) return false;
        if (!take_chunk(d, type, size, file_size - pos)) return false;
        if (size == -1) break; /* data to end of file */
        pos += size;
    }
    return d->offsets && d->data_start;
}

/* Rebases the size prefix sums to file offsets and checks they fit the data. */
static bool finish_offsets(caf_demux_t * d) {
    if (d->offsets[d->packet_count] > d->data_size) return false;
    for (uint32_t i = 0; i <= d->packet_count; i++) d->offsets[i] += d->data_start;
    return true;
}

caf_demux_t * caf_demux_open(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    caf_demux_t * d = calloc(1, sizeof(*d));
    uint32_t bpp = 1;
    if (d) d->file = f;
    if (!d || !read_desc(f, d->fourcc, &d->frames_per_packet, &bpp) ||
        bpp != 0 || d->frames_per_packet == 0 || fseeko(f, 0, SEEK_END) != 0 ||
        !scan_chunks(d, (int64_t) ftello(f)) || !finish_offsets(d)) {
        if (d) caf_demux_close(d); else fclose(f);
        return NULL;
    }
    return d;
}

void caf_demux_close(caf_demux_t * d) {
    if (!d) return;
    if (d->file) fclose(d->file);
    free(d->cookie);
    free(d->offsets);
    free(d);
}

void caf_demux_get_codec_fourcc(const caf_demux_t * d, char out_fourcc[5]) {
    memcpy(out_fourcc, d->fourcc, 5);
}

const uint8_t * caf_demux_get_codec_config(const caf_demux_t * d, uint32_t * out_size) {
    *out_size = d->cookie_size;
    return d->cookie;
}

uint32_t caf_demux_get_packet_count(const caf_demux_t * d) { return d->packet_count; }
uint32_t caf_demux_get_frames_per_packet(const caf_demux_t * d) { return d->frames_per_packet; }
uint64_t caf_demux_get_valid_frames(const caf_demux_t * d) { return (uint64_t) d->valid_frames; }
uint32_t caf_demux_get_priming_frames(const caf_demux_t * d) { return (uint32_t) d->priming; }

bool caf_demux_read_packet(caf_demux_t * d, uint32_t index, uint8_t * buf,
                           uint32_t buf_size, uint32_t * out_size) {
    if (index >= d->packet_count) return false;
    uint64_t size = d->offsets[index + 1] - d->offsets[index];
    if (size > buf_size || fseeko(d->file, (off_t) d->offsets[index], SEEK_SET) != 0 ||
        fread(buf, 1, (size_t) size, d->file) != (size_t) size) return false;
    *out_size = (uint32_t) size;
    return true;
}
