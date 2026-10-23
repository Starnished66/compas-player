/* SACD packet framing follows Rockbox f4c7e8b595 (Martin Cai) and the
 * Scarlet Book structures in sacd-ripper. Reuses Compas' DSD filter/DoP.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "sacd_reader.h"
#include "dst_decoder.h"
#include "../library/sacd_iso.h"
#include <stdlib.h>
#include <string.h>

#define SECTOR_BYTES 2048u
#define FRAME_BYTES DST_STEREO_FRAME_BYTES
#define FRAME_ROWS DST_STEREO_CHANNEL_BYTES
#define PACKED_MAX 65536u

struct sacd_reader {
    FILE *f;
    uint32_t first, last, next_sector, file_sector, total_frames, frame_time;
    unsigned packet, packets, frame_info, frame_infos, offset, pos;
    bool dst, failed, eof;
    uint8_t sector[SECTOR_BYTES], output[FRAME_BYTES];
    uint8_t *packed; /* Only compressed discs allocate compressed-frame storage. */
    dst_decoder_t *decoder;
};

static bool read_sector(sacd_reader_t *r, uint32_t sector) {
    if (sector > r->last ||
        (sector != r->file_sector && fseeko(r->f, (off_t)sector * SECTOR_BYTES, SEEK_SET)) ||
        fread(r->sector, 1, SECTOR_BYTES, r->f) != SECTOR_BYTES) {
        r->file_sector = UINT32_MAX;
        return false;
    }
    r->file_sector = sector + 1;
    unsigned header = r->sector[0];
    r->packets = header >> 5;
    r->frame_infos = (header >> 2) & 7;
    r->packet = r->frame_info = 0;
    r->offset = 1 + 2 * r->packets + (r->dst ? 4 : 3) * r->frame_infos;
    r->next_sector = sector + 1;
    return (bool)(header & 1) == r->dst;
}

/* Leave a new frame's first packet pending when finishing its predecessor. */
static bool next_frame(sacd_reader_t *r, uint32_t decode_from) {
    size_t size = 0;
    bool started = false;
    unsigned sectors = 0;
    while (r->packet < r->packets || r->next_sector <= r->last) {
        if (r->packet == r->packets && (++sectors > 32 || !read_sector(r, r->next_sector)))
            return false;
        if (r->packet == r->packets) continue;
        const uint8_t *p = r->sector + 1 + 2 * r->packet;
        unsigned length = ((p[0] & 7) << 8) | p[1];
        bool audio = ((p[0] >> 3) & 7) == 2, start = audio && (p[0] & 0x80);
        if (length > SECTOR_BYTES - r->offset) return false;
        if (start && started) break;
        if (start) {
            if (r->frame_info >= r->frame_infos) return false;
            const uint8_t *info = r->sector + 1 + 2 * r->packets +
                                  (r->dst ? 4 : 3) * r->frame_info++;
            if (info[1] >= 60 || info[2] >= 75) return false;
            r->frame_time = ((uint32_t)info[0] * 60 + info[1]) * 75 + info[2];
            started = true;
        }
        if (audio && started) {
            if (length > (r->dst ? PACKED_MAX : FRAME_BYTES) - size) return false;
            memcpy((r->dst ? r->packed : r->output) + size, r->sector + r->offset, length);
            size += length;
        }
        r->offset += length;
        r->packet++;
    }
    if (!started) { r->eof = true; return false; }
    if (r->dst && r->frame_time >= decode_from && dst_decoder_decode(r->decoder, r->packed, size, r->output) != FRAME_BYTES)
        return false;
    r->pos = 0;
    return r->dst || size == FRAME_BYTES;
}

/* Binary search only headers: no whole-disc index or decoding on seek. Each
 * audio frame spans at most 31 sectors; allow the probe to cross one frame. */
static bool probe_time(sacd_reader_t *r, uint32_t sector, uint32_t *found, uint32_t *time) {
    for (unsigned n = 0; n < 32 && sector <= r->last; n++, sector++) {
        if (!read_sector(r, sector)) return false;
        if (!r->frame_infos) continue;
        const uint8_t *info = r->sector + 1 + 2 * r->packets;
        if (info[1] >= 60 || info[2] >= 75) return false;
        *found = sector;
        *time = ((uint32_t)info[0] * 60 + info[1]) * 75 + info[2];
        return true;
    }
    return false;
}

bool sacd_reader_seek(sacd_reader_t *r, uint64_t byte) {
    if (!r || byte > (uint64_t)r->total_frames * FRAME_ROWS) return false;
    uint32_t target = byte / FRAME_ROWS, best = r->first;
    clearerr(r->f);
    r->failed = r->eof = false;
    r->pos = FRAME_BYTES;
    if (target == r->total_frames) {
        r->packets = r->packet = 0;
        r->next_sector = r->last + 1;
        r->eof = true;
        return true;
    }
    uint32_t lo = r->first, hi = r->last;
    while (target && lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2, found, time;
        if (probe_time(r, mid, &found, &time) && time <= target) {
            best = found;
            lo = found + 1;
        } else {
            if (!mid) break;
            hi = mid - 1;
        }
    }
    r->packets = r->packet = 0;
    r->next_sector = best;
    unsigned skipped = 0;
    do {
        if (++skipped > 64 || !next_frame(r, target)) { r->failed = true; return false; }
    } while (r->frame_time < target);
    if (r->frame_time != target) { r->failed = true; return false; }
    r->pos = (unsigned)(byte % FRAME_ROWS) * 2;
    return true;
}

bool sacd_reader_bytes(sacd_reader_t *r, uint8_t *left, uint8_t *right, unsigned count) {
    if (r->failed || !count || count > 16) return false;
    if (r->pos == FRAME_BYTES) {
        uint32_t expected = r->frame_time + 1;
        if (!next_frame(r, 0) || r->frame_time != expected) { r->failed = true; return false; }
    }
    if (count * 2 > FRAME_BYTES - r->pos) return false;
    for (unsigned i = 0; i < count; i++) {
        left[i] = r->output[r->pos++];
        right[i] = r->output[r->pos++];
    }
    return true;
}

bool sacd_reader_eof(const sacd_reader_t *r) { return r->eof; }

void sacd_reader_close(sacd_reader_t *r) {
    if (!r) return;
    dst_decoder_destroy(r->decoder);
    free(r->packed);
    free(r);
}

sacd_reader_t *sacd_reader_open(FILE *f, uint64_t *samples) {
    sacd_area_t area;
    if (!sacd_read_area(f, &area)) return NULL;
    sacd_reader_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->f = f;
    r->file_sector = UINT32_MAX;
    r->first = area.start_sector;
    r->last = area.end_sector;
    r->total_frames = area.total_frames;
    r->dst = area.dst;
    if (r->dst) {
        r->packed = malloc(PACKED_MAX);
        r->decoder = dst_decoder_create();
        if (!r->packed || !r->decoder) goto fail;
    }
    if (!sacd_reader_seek(r, 0)) goto fail;
    *samples = (uint64_t)r->total_frames * FRAME_ROWS * 8;
    return r;
fail:
    sacd_reader_close(r);
    return NULL;
}
