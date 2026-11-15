/* SACD packet framing follows Rockbox f4c7e8b595 (Martin Cai) and the
 * Scarlet Book structures in sacd-ripper. Reuses Compas' DSD filter/DoP.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "sacd_reader.h"
#include "../../engine/audio_helpers.h"
#include "../../decoders/dst/dst_decoder.h"
#include "../../../library/sacd/sacd_iso.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SECTOR_BYTES 2048u
#define FRAME_BYTES DST_MAX_FRAME_BYTES
#define FRAME_ROWS DST_CHANNEL_BYTES
#define PACKED_MAX 65536u
/* Longest DFF the frame index accepts: 4 hours at 75 frames/s, 8.6 MB of offsets. */
#define DFF_MAX_FRAMES (4u * 3600u * 75u)

struct sacd_reader {
    FILE *f;
    bool dff;
    uint64_t dff_end;
    uint64_t *dff_frames;
    uint32_t first, last, next_sector, file_sector, total_frames, frame_time;
    unsigned packet, packets, frame_info, frame_infos, offset, pos;
    bool dst, failed, eof;
    unsigned channels;
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
    if (r->dff) {
        if (r->frame_time >= r->total_frames) { r->eof = true; return false; }
        uint64_t offset = r->dff_frames[r->frame_time];
        if (offset > r->dff_end || r->dff_end - offset < 12 ||
            fseeko(r->f, (off_t)offset, SEEK_SET) != 0) return false;
        uint8_t header[12];
        if (fread(header, 1, sizeof(header), r->f) != sizeof(header) ||
            memcmp(header, "DSTF", 4) != 0) return false;
        uint64_t size = audio_read_u64be(header + 4);
        if (size > r->dff_end - offset - 12 || size > PACKED_MAX ||
            fread(r->packed, 1, (size_t)size, r->f) != size) return false;
        if (r->frame_time >= decode_from &&
            dst_decoder_decode(r->decoder, r->packed, (size_t)size, r->output) !=
                (int)(r->channels * DST_CHANNEL_BYTES)) return false;
        r->frame_time++;
        r->pos = 0;
        return true;
    }
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
    if (r->dst && r->frame_time >= decode_from &&
        dst_decoder_decode(r->decoder, r->packed, size, r->output) !=
            (int)(r->channels * DST_CHANNEL_BYTES)) return false;
    r->pos = 0;
    return r->dst || size == r->channels * DST_CHANNEL_BYTES;
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
    r->pos = r->channels * DST_CHANNEL_BYTES;
    if (r->dff) {
        uint32_t target = (uint32_t)(byte / FRAME_ROWS);
        if (target == r->total_frames) { r->frame_time = target; r->eof = true; return true; }
        r->frame_time = target;
        if (!next_frame(r, target)) { r->failed = true; return false; }
        r->pos = (unsigned)(byte % FRAME_ROWS) * r->channels;
        return true;
    }
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
    r->pos = (unsigned)(byte % FRAME_ROWS) * r->channels;
    return true;
}

bool sacd_reader_bytes(sacd_reader_t *r, uint8_t *channels[], unsigned count) {
    if (!r || !channels || r->failed || !count || count > 16) return false;
    for (unsigned ch = 0; ch < r->channels; ++ch) if (!channels[ch]) return false;
    if (r->pos == r->channels * DST_CHANNEL_BYTES) {
        uint32_t expected = r->frame_time + 1;
        if (!next_frame(r, 0) || r->frame_time != expected) { r->failed = true; return false; }
    }
    if (count * r->channels > r->channels * DST_CHANNEL_BYTES - r->pos) return false;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned ch = 0; ch < r->channels; ++ch)
            channels[ch][i] = r->output[r->pos++];
    return true;
}

bool sacd_reader_eof(const sacd_reader_t *r) { return r->eof; }

void sacd_reader_close(sacd_reader_t *r) {
    if (!r) return;
    dst_decoder_destroy(r->decoder);
    free(r->packed);
    free(r->dff_frames);
    free(r);
}

unsigned sacd_reader_channels(const sacd_reader_t *r) { return r ? r->channels : 0; }
bool sacd_reader_is_dst(const sacd_reader_t *r) { return r && r->dst; }

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
    r->channels = area.channels;
    if (r->dst) {
        r->packed = malloc(PACKED_MAX);
        r->decoder = dst_decoder_create(r->channels);
        if (!r->packed || !r->decoder) goto fail;
    }
    if (!sacd_reader_seek(r, 0)) goto fail;
    *samples = (uint64_t)r->total_frames * FRAME_ROWS * 8;
    return r;
fail:
    sacd_reader_close(r);
    return NULL;
}

/* Index offsets point to DSTF sound bytes, not chunk headers. Verify every
 * entry against its header before allowing it to drive seeks. */
static bool dff_load_index(FILE *f, uint64_t start, uint64_t end, uint64_t index,
                            uint64_t size, uint32_t count, uint64_t *frames) {
    if (!index || size != (uint64_t)count * 12) return false;
    uint64_t previous_end = start;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t entry[12], header[12];
        if (fseeko(f, (off_t)(index + (uint64_t)i * 12), SEEK_SET) != 0 ||
            fread(entry, 1, 12, f) != 12) return false;
        uint64_t sound = audio_read_u64be(entry);
        uint32_t length = audio_read_u32be(entry + 8);
        if (sound < 12 || sound - 12 < previous_end || sound > end ||
            !length || length > PACKED_MAX || length > end - sound ||
            (length & 1u) > end - sound - length ||
            fseeko(f, (off_t)(sound - 12), SEEK_SET) != 0 ||
            fread(header, 1, 12, f) != 12 || memcmp(header, "DSTF", 4) ||
            audio_read_u64be(header + 4) != length) return false;
        frames[i] = sound - 12;
        previous_end = sound + length + (length & 1u);
    }
    return true;
}

sacd_reader_t *sacd_reader_open_dff(FILE *f, uint64_t start, uint64_t size,
                                    unsigned channels, uint64_t index_start, uint64_t index_size,
                                    uint64_t *samples) {
    struct stat st;
    if (!f || !samples || channels < 2 || channels > DST_MAX_CHANNELS ||
        fstat(fileno(f), &st) != 0 || st.st_size < 0 ||
        start > (uint64_t)st.st_size || size > (uint64_t)st.st_size - start ||
        index_start > (uint64_t)st.st_size || index_size > (uint64_t)st.st_size - index_start)
        return NULL;
    uint64_t end = start + size, offset = start;
    uint32_t frame_count = 0;
    unsigned frame_rate = 0;
    uint64_t *frames = NULL;
    size_t found = 0;
    while (offset < end) {
        uint8_t header[12];
        if (end - offset < sizeof(header) || fseeko(f, (off_t)offset, SEEK_SET) != 0 ||
            fread(header, 1, sizeof(header), f) != sizeof(header)) goto fail;
        uint64_t chunk_size = audio_read_u64be(header + 4);
        uint64_t payload = offset + sizeof(header);
        if (chunk_size > end - payload || chunk_size > UINT64_MAX - payload) goto fail;
        if (memcmp(header, "FRTE", 4) == 0) {
            uint8_t frte[6];
            if (chunk_size < sizeof(frte) || frame_count ||
                fseeko(f, (off_t)payload, SEEK_SET) != 0 ||
                fread(frte, 1, sizeof(frte), f) != sizeof(frte)) goto fail;
            frame_count = audio_read_u32be(frte);
            frame_rate = (unsigned)((frte[4] << 8) | frte[5]);
            if (!frame_count || frame_rate != 75 || frame_count > DFF_MAX_FRAMES ||
                frame_count > size / (sizeof(header) + 1)) goto fail;
            frames = calloc(frame_count, sizeof(*frames));
            if (!frames) goto fail;
            if (dff_load_index(f, payload + chunk_size + (chunk_size & 1), end,
                               index_start, index_size, frame_count, frames)) {
                found = frame_count;
                break;
            }
        } else if (memcmp(header, "DSTF", 4) == 0) {
            if (!frames || found >= frame_count || !chunk_size || chunk_size > PACKED_MAX)
                goto fail;
            frames[found++] = offset;
        }
        uint64_t padded = chunk_size + (chunk_size & 1);
        if (padded > end - payload || payload + padded <= offset) goto fail;
        offset = payload + padded;
    }
    if (!frames || found != frame_count) goto fail;
    sacd_reader_t *r = calloc(1, sizeof(*r));
    if (!r) goto fail;
    r->f = f;
    r->dff = true;
    r->dff_end = end;
    r->dff_frames = frames;
    r->channels = channels;
    r->total_frames = frame_count;
    r->dst = true;
    r->pos = channels * DST_CHANNEL_BYTES;
    r->packed = malloc(PACKED_MAX);
    r->decoder = dst_decoder_create(channels);
    if (!r->packed || !r->decoder) { sacd_reader_close(r); return NULL; }
    *samples = (uint64_t)frame_count * DST_CHANNEL_BYTES * 8;
    return r;
fail:
    free(frames);
    return NULL;
}
