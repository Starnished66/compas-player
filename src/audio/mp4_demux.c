#include "mp4_demux.h"
#include "audio_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
    char type[5];
    uint64_t size;   /* total box size, including header */
    long header_size; /* 8 (normal) or 16 (64-bit extended size) */
    long data_start;  /* file offset where the box's payload begins */
} box_header_t;

typedef struct {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
} stsc_entry_t;

typedef struct { uint32_t count, delta; uint64_t first_sample; } stts_entry_t;

/* Raw big-endian entries [first, first + count) of an on-disk table. */
typedef struct {
    uint8_t * bytes;
    uint32_t first;
    uint32_t count;
} table_window_t;

/* Expanded per-sample tables are ~12 bytes each. A 25h AAC audiobook is
 * millions of access units -- tens of MB, enough to OOM this 56 MiB
 * device on open (Linux overcommit makes malloc succeed, then the first
 * write is SIGKILL). Keep ISO-BMFF stsz/stco/stsc compact past this. */
#ifndef MP4_EXPAND_MAX_SAMPLES
#define MP4_EXPAND_MAX_SAMPLES 65536
#endif
/* Compact mode reads stsz/stco through one bounded window each instead of
 * one entry per sample. Over HTTP every jump between the tables and mdat is
 * a new connection, so per-entry reads never got a long audiobook started.
 * 64Ki sizes cover about 25 min of 44.1 kHz AAC per refill (256 KiB);
 * 32Ki chunk offsets are 128-256 KiB. */
#ifndef MP4_STSZ_WINDOW
#define MP4_STSZ_WINDOW 65536
#endif
#ifndef MP4_STCO_WINDOW
#define MP4_STCO_WINDOW 32768
#endif
/* Entries kept before a refill's target, so a seek's small step back
 * (decoder preroll) does not refetch the window it just loaded. */
#define MP4_WINDOW_BACK_MARGIN 64
#define MP4_MAX_STSC_ENTRIES 4096
#define MP4_MAX_STTS_ENTRIES 4096
#define MP4_MAX_STSD_ENTRY_BYTES (1024U * 1024U)

struct mp4_demux {
    FILE * f;
    uint64_t file_size;

    char codec_fourcc[5];
    uint8_t * codec_config;
    uint32_t codec_config_size;

    uint32_t sample_count;
    uint32_t uniform_size; /* stsz default; 0 = per-sample table on disk */
    long stsz_table_offset;
    uint32_t * sample_sizes; /* NULL in compact mode */

    uint32_t chunk_count;
    bool stco_is64;
    long stco_table_offset;
    uint32_t stsc_count;
    stsc_entry_t * stsc;
    uint64_t * sample_offsets; /* NULL in compact mode */
    table_window_t size_window;
    table_window_t chunk_window;

    /* Compact-mode position: sample cursor_index starts at cursor_offset in
     * 0-based chunk cursor_chunk (stsc entry cursor_stsc), followed by
     * cursor_chunk_left more samples of that chunk. Chunks need not be
     * adjacent (another track can be interleaved), so stepping past a
     * chunk's last sample reads the next chunk's offset. */
    uint32_t cursor_index;
    uint64_t cursor_offset;
    uint32_t cursor_chunk;
    uint32_t cursor_stsc;
    uint32_t cursor_chunk_left;

    uint32_t frames_per_sample;
    uint32_t first_sample_delta;
    uint64_t total_pcm_frames;
    uint64_t raw_total_pcm_frames;
    uint32_t media_timescale;
    uint32_t sample_rate; /* decoded PCM rate; updated from codec config when needed */
    uint32_t sample_entry_rate;
    uint32_t movie_timescale;
    uint64_t edit_priming_ticks;
    uint64_t edit_playable_ticks;
    stts_entry_t * stts;
    uint32_t stts_count;
    uint64_t gapless_priming;
    uint64_t gapless_playable;
    bool has_gapless;
    bool has_itunsmpb;
    uint64_t itunsmpb_priming;
    uint64_t itunsmpb_padding;
    uint64_t itunsmpb_playable;
};

static uint64_t ticks_to_frames(uint64_t ticks, uint32_t rate, uint32_t scale) {
    if (!scale) return 0;
    /* Split the multiplication to avoid overflow for long timelines. */
    return (ticks / scale) * rate + (((ticks % scale) * rate) + scale / 2) / scale;
}

static bool read_box_header(FILE * f, box_header_t * out) {
    long start = ftell(f);
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;

    uint32_t size32 = audio_read_u32be(hdr);
    memcpy(out->type, hdr + 4, 4);
    out->type[4] = '\0';

    if (size32 == 1) {
        uint8_t ext[8];
        if (fread(ext, 1, sizeof(ext), f) != sizeof(ext)) return false;
        out->size = audio_read_u64be(ext);
        out->header_size = 16;
    } else if (size32 == 0) {
        long cur = ftell(f);
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        fseek(f, cur, SEEK_SET);
        out->size = (uint64_t) (end - start);
        out->header_size = 8;
    } else {
        out->size = size32;
        out->header_size = 8;
    }

    if (start < 0 || out->size < (uint64_t) out->header_size ||
        out->size > (uint64_t) (LONG_MAX - start)) return false;
    out->data_start = start + out->header_size;
    return true;
}

static bool box_payload_has(box_header_t box, uint64_t offset, uint64_t bytes) {
    uint64_t payload = box.size - (uint64_t) box.header_size;
    return offset <= payload && bytes <= payload - offset;
}

/* Scans children of a plain container box (moov/trak/mdia/minf/stbl/udta/...
 * -- no extra fields before the first child) for one with the given type,
 * within [container_start, container_start + container_size). */
static bool find_child_box(FILE * f, long container_start, uint64_t container_size, const char * type, box_header_t * out) {
    if (container_start < 0 || container_size > (uint64_t) (LONG_MAX - container_start)) return false;
    long end = container_start + (long) container_size;
    if (fseek(f, container_start, SEEK_SET) != 0) return false;

    while (ftell(f) < end) {
        box_header_t box;
        if (!read_box_header(f, &box)) return false;
        long box_start = box.data_start - box.header_size;
        if (box_start < container_start || box.size > (uint64_t) (end - box_start)) return false;
        if (strcmp(box.type, type) == 0) {
            *out = box;
            return true;
        }
        long next = box_start + (long) box.size;
        if (next <= box_start) return false;
        if (fseek(f, next, SEEK_SET) != 0) return false;
    }
    return false;
}


/* Finds the first "trak" box whose mdia/hdlr declares handler_type "soun". */
static bool find_audio_trak(FILE * f, box_header_t moov, box_header_t * out_trak) {
    long end = moov.data_start + (long) moov.size - moov.header_size;
    fseek(f, moov.data_start, SEEK_SET);

    while (ftell(f) < end) {
        box_header_t trak;
        if (!read_box_header(f, &trak)) return false;

        if (strcmp(trak.type, "trak") == 0) {
            box_header_t mdia, hdlr;
            if (find_child_box(f, trak.data_start, trak.size - (uint64_t) trak.header_size, "mdia", &mdia) &&
                find_child_box(f, mdia.data_start, mdia.size - (uint64_t) mdia.header_size, "hdlr", &hdlr)) {
                uint8_t buf[12];
                fseek(f, hdlr.data_start + 4, SEEK_SET); /* skip version/flags(4), then predefined(4) */
                if (fread(buf, 1, 8, f) == 8 && memcmp(buf + 4, "soun", 4) == 0) {
                    *out_trak = trak;
                    return true;
                }
            }
        }

        long next = (long) (trak.data_start - trak.header_size) + (long) trak.size;
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    return false;
}

/* Parses the single audio sample entry inside stsd: the fixed 36-byte
 * AudioSampleEntry fields, followed by the codec-specific config (the ALAC
 * magic cookie box, or an "esds" box for AAC) -- see
 * ALACMagicCookieDescription.txt for the authoritative byte layout this
 * matches. */
static bool parse_stsd(mp4_demux_t * d, box_header_t stsd) {
    /* stsd is a FullBox: version/flags(4) + entry_count(4), then entries */
    if (!box_payload_has(stsd, 0, 8 + 36) || fseek(d->f, stsd.data_start + 8, SEEK_SET) != 0) return false;

    uint8_t entry_header[36];
    if (fread(entry_header, 1, sizeof(entry_header), d->f) != sizeof(entry_header)) return false;

    uint32_t entry_size = audio_read_u32be(entry_header);
    memcpy(d->codec_fourcc, entry_header + 4, 4);
    d->codec_fourcc[4] = '\0';
    d->sample_entry_rate = audio_read_u32be(entry_header + 32) >> 16;
    d->sample_rate = d->sample_entry_rate;

    if (entry_size <= 36 || entry_size > MP4_MAX_STSD_ENTRY_BYTES || !box_payload_has(stsd, 8, entry_size)) return false;
    uint32_t config_region_size = entry_size - 36;

    uint8_t * config_region = malloc(config_region_size);
    if (!config_region) return false;
    if (fread(config_region, 1, config_region_size, d->f) != config_region_size) {
        free(config_region);
        return false;
    }

    if (strcmp(d->codec_fourcc, "alac") == 0) {
        /* The whole region is the nested 'alac' box containing the magic
         * cookie; ALACDecoder::Init() parses this directly (it knows how
         * to skip the box header itself). */
        d->codec_config = config_region;
        d->codec_config_size = config_region_size;
        return true;
    }

    if (strcmp(d->codec_fourcc, "mp4a") == 0) {
        /* Find the nested "esds" box and extract the DecoderSpecificInfo
         * payload from its MPEG-4 descriptor tags (a small tag+length+value
         * structure, not a plain sub-box). */
        for (uint32_t pos = 0; pos + 8 <= config_region_size;) {
            uint32_t box_size = audio_read_u32be(config_region + pos);
            if (box_size < 8 || pos + box_size > config_region_size) break;

            if (memcmp(config_region + pos + 4, "esds", 4) == 0) {
                uint32_t p = pos + 8 + 4; /* box header(8) + FullBox version/flags(4) */
                /* Walk descriptor tags: tag(1) + size (variable-length, top
                 * bit continuation encoding) + value. We want tag 0x05
                 * (DecoderSpecificInfo), nested inside tag 0x03 (ESDescriptor)
                 * -> tag 0x04 (DecoderConfigDescriptor). */
                while (p + 2 <= pos + box_size) {
                    uint8_t tag = config_region[p++];
                    uint32_t desc_len = 0;
                    for (int i = 0; i < 4 && p < pos + box_size; i++) {
                        uint8_t b = config_region[p++];
                        desc_len = (desc_len << 7) | (b & 0x7F);
                        if (!(b & 0x80)) break;
                    }
                    if (tag == 0x05) { /* DecoderSpecificInfo: this is the raw ASC */
                        if (p + desc_len > config_region_size) break;
                        d->codec_config = malloc(desc_len);
                        if (!d->codec_config) { free(config_region); return false; }
                        memcpy(d->codec_config, config_region + p, desc_len);
                        d->codec_config_size = desc_len;
                        free(config_region);
                        return true;
                    }
                    if (tag == 0x03) { p += 3; continue; } /* ES_ID(2) + flags(1), then nested descriptors */
                    if (tag == 0x04) { p += 13; continue; } /* objectTypeIndication..avgBitrate, then nested descriptors */
                    p += desc_len; /* skip anything else */
                }
            }

            pos += box_size;
        }
    }

    free(config_region);
    return false;
}

static bool read_u32be_file(FILE * f, uint32_t * out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = audio_read_u32be(b);
    return true;
}

/* Entry i of an on-disk table, refilling the window with up to cap entries
 * from just before i when it is outside. One seek and one read per refill. */
static const uint8_t * table_entry(mp4_demux_t * d, table_window_t * w, long table_offset,
                                   uint32_t stride, uint32_t total, uint32_t cap, uint32_t i) {
    if (w->bytes && i >= w->first && i - w->first < w->count)
        return w->bytes + (size_t) (i - w->first) * stride;
    if (cap > total) cap = total;
    if (!w->bytes && !(w->bytes = malloc((size_t) cap * stride))) return NULL;
    uint32_t back = cap / 4 < MP4_WINDOW_BACK_MARGIN ? cap / 4 : MP4_WINDOW_BACK_MARGIN;
    uint32_t first = i > back ? i - back : 0;
    uint32_t count = total - first < cap ? total - first : cap;
    w->count = 0;
    if (fseek(d->f, table_offset + (long) first * (long) stride, SEEK_SET) != 0 ||
        fread(w->bytes, stride, count, d->f) != count) return NULL;
    w->first = first;
    w->count = count;
    return w->bytes + (size_t) (i - first) * stride;
}

static bool sample_size_at(mp4_demux_t * d, uint32_t i, uint32_t * out) {
    if (i >= d->sample_count) return false;
    if (d->sample_sizes) {
        *out = d->sample_sizes[i];
        return true;
    }
    if (d->uniform_size != 0) {
        *out = d->uniform_size;
        return true;
    }
    const uint8_t * entry = table_entry(d, &d->size_window, d->stsz_table_offset, 4,
                                        d->sample_count, MP4_STSZ_WINDOW, i);
    if (!entry) return false;
    *out = audio_read_u32be(entry);
    return true;
}

static bool chunk_offset_at(mp4_demux_t * d, uint32_t chunk_index0, uint64_t * out) {
    if (chunk_index0 >= d->chunk_count) return false;
    uint32_t stride = d->stco_is64 ? 8 : 4;
    const uint8_t * entry = table_entry(d, &d->chunk_window, d->stco_table_offset, stride,
                                        d->chunk_count, MP4_STCO_WINDOW, chunk_index0);
    if (!entry) return false;
    *out = d->stco_is64 ? audio_read_u64be(entry) : (uint64_t) audio_read_u32be(entry);
    return true;
}

/* Last 0-based chunk of stsc entry e. */
static uint32_t stsc_last_chunk(const mp4_demux_t * d, uint32_t e) {
    return (e + 1 < d->stsc_count ? d->stsc[e + 1].first_chunk : d->chunk_count + 1) - 2;
}

static bool sample_offset_at(mp4_demux_t * d, uint32_t i, uint64_t * out) {
    if (i >= d->sample_count) return false;
    if (d->sample_offsets) {
        *out = d->sample_offsets[i];
        return true;
    }
    if (i == d->cursor_index) {
        *out = d->cursor_offset;
        return true;
    }
    if (i == d->cursor_index + 1) {
        uint64_t off;
        if (d->cursor_chunk_left) {
            uint32_t sz;
            if (!sample_size_at(d, d->cursor_index, &sz)) return false;
            off = d->cursor_offset + sz;
            d->cursor_chunk_left--;
        } else {
            uint32_t chunk = d->cursor_chunk + 1;
            uint32_t e = d->cursor_stsc;
            if (chunk > stsc_last_chunk(d, e)) e++;
            if (e >= d->stsc_count || !chunk_offset_at(d, chunk, &off)) return false;
            d->cursor_chunk = chunk;
            d->cursor_stsc = e;
            d->cursor_chunk_left = d->stsc[e].samples_per_chunk - 1;
        }
        d->cursor_index = i;
        d->cursor_offset = off;
        *out = off;
        return true;
    }

    uint32_t remaining = i;
    uint32_t chunk_1based = 1;
    uint32_t index_in_chunk = 0;
    uint32_t entry = 0;
    bool found = false;
    for (uint32_t e = 0; e < d->stsc_count; e++) {
        uint32_t first = d->stsc[e].first_chunk;
        uint32_t spc = d->stsc[e].samples_per_chunk;
        if (spc == 0) return false;
        uint32_t next_first = (e + 1 < d->stsc_count) ? d->stsc[e + 1].first_chunk : d->chunk_count + 1;
        if (next_first <= first) return false;
        uint64_t nsamples = (uint64_t) (next_first - first) * spc;
        if ((uint64_t) remaining < nsamples) {
            chunk_1based = first + remaining / spc;
            index_in_chunk = remaining % spc;
            entry = e;
            found = true;
            break;
        }
        remaining -= (uint32_t) nsamples;
    }
    if (!found || chunk_1based == 0) return false;

    uint64_t off;
    if (!chunk_offset_at(d, chunk_1based - 1, &off)) return false;
    uint32_t first_in_chunk = i - index_in_chunk;
    for (uint32_t s = 0; s < index_in_chunk; s++) {
        uint32_t sz;
        if (!sample_size_at(d, first_in_chunk + s, &sz)) return false;
        off += sz;
    }
    d->cursor_index = i;
    d->cursor_offset = off;
    d->cursor_chunk = chunk_1based - 1;
    d->cursor_stsc = entry;
    d->cursor_chunk_left = d->stsc[entry].samples_per_chunk - 1 - index_in_chunk;
    *out = off;
    return true;
}

static bool parse_stsz(mp4_demux_t * d, box_header_t stsz) {
    uint8_t hdr[12];
    if (!box_payload_has(stsz, 0, sizeof(hdr)) || fseek(d->f, stsz.data_start, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), d->f) != sizeof(hdr)) return false;

    d->uniform_size = audio_read_u32be(hdr + 4);
    uint32_t count = audio_read_u32be(hdr + 8);
    if (count == 0) return false;
    d->sample_count = count;
    d->stsz_table_offset = stsz.data_start + 12;
    if (d->uniform_size == 0 && !box_payload_has(stsz, 12, (uint64_t) count * 4U)) return false;

    if (count > MP4_EXPAND_MAX_SAMPLES) return true;

    d->sample_sizes = malloc(sizeof(uint32_t) * (size_t) count);
    if (!d->sample_sizes) return false;
    if (d->uniform_size != 0) {
        for (uint32_t i = 0; i < count; i++) d->sample_sizes[i] = d->uniform_size;
        return true;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!read_u32be_file(d->f, &d->sample_sizes[i])) return false;
    }
    return true;
}

/* Combines stco/co64 (chunk offsets) with stsc (samples-per-chunk ranges).
 * Short files still expand every sample offset into RAM. Long audiobooks
 * keep the compact boxes and resolve offsets on demand -- sequential
 * playback is a running cursor, seeks walk stsc then one chunk. */
static bool parse_sample_offsets(mp4_demux_t * d, box_header_t stbl) {
    box_header_t stco_box;
    d->stco_is64 = false;
    if (!find_child_box(d->f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stco", &stco_box)) {
        if (!find_child_box(d->f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "co64", &stco_box)) return false;
        d->stco_is64 = true;
    }

    box_header_t stsc_box;
    if (!find_child_box(d->f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stsc", &stsc_box)) return false;

    uint8_t hdr[8];
    if (!box_payload_has(stco_box, 0, 8) || !box_payload_has(stsc_box, 0, 8)) return false;
    fseek(d->f, stco_box.data_start, SEEK_SET);
    if (fread(hdr, 1, 8, d->f) != 8) return false;
    d->chunk_count = audio_read_u32be(hdr + 4);
    if (d->chunk_count == 0 || d->chunk_count > d->sample_count) return false;
    if (!box_payload_has(stco_box, 8, (uint64_t) d->chunk_count * (d->stco_is64 ? 8U : 4U))) return false;
    d->stco_table_offset = stco_box.data_start + 8;

    fseek(d->f, stsc_box.data_start, SEEK_SET);
    if (fread(hdr, 1, 8, d->f) != 8) return false;
    d->stsc_count = audio_read_u32be(hdr + 4);
    if (d->stsc_count == 0 || d->stsc_count > MP4_MAX_STSC_ENTRIES) return false;
    if (!box_payload_has(stsc_box, 8, (uint64_t) d->stsc_count * 12U)) return false;

    d->stsc = malloc(sizeof(stsc_entry_t) * (size_t) d->stsc_count);
    if (!d->stsc) return false;
    for (uint32_t i = 0; i < d->stsc_count; i++) {
        uint8_t buf[12];
        if (fread(buf, 1, 12, d->f) != 12) return false;
        d->stsc[i].first_chunk = audio_read_u32be(buf);
        d->stsc[i].samples_per_chunk = audio_read_u32be(buf + 4);
        if (d->stsc[i].first_chunk == 0 || d->stsc[i].samples_per_chunk == 0) return false;
        if (i > 0 && d->stsc[i].first_chunk <= d->stsc[i - 1].first_chunk) return false;
    }

    if (d->sample_count > MP4_EXPAND_MAX_SAMPLES) {
        if (d->stsc[0].first_chunk != 1) return false;
        if (!chunk_offset_at(d, 0, &d->cursor_offset)) return false;
        d->cursor_index = 0;
        d->cursor_chunk = 0;
        d->cursor_stsc = 0;
        d->cursor_chunk_left = d->stsc[0].samples_per_chunk - 1;
        return true;
    }

    uint64_t * chunk_offsets = malloc(sizeof(uint64_t) * (size_t) d->chunk_count);
    if (!chunk_offsets) return false;
    fseek(d->f, d->stco_table_offset, SEEK_SET);
    for (uint32_t i = 0; i < d->chunk_count; i++) {
        uint8_t buf[8];
        size_t n = d->stco_is64 ? 8 : 4;
        if (fread(buf, 1, n, d->f) != n) {
            free(chunk_offsets);
            return false;
        }
        chunk_offsets[i] = d->stco_is64 ? audio_read_u64be(buf) : (uint64_t) audio_read_u32be(buf);
        if (chunk_offsets[i] >= d->file_size) { free(chunk_offsets); return false; }
    }

    d->sample_offsets = malloc(sizeof(uint64_t) * (size_t) d->sample_count);
    if (!d->sample_offsets) {
        free(chunk_offsets);
        return false;
    }

    uint32_t sample_index = 0;
    for (uint32_t chunk = 1; chunk <= d->chunk_count && sample_index < d->sample_count; chunk++) {
        uint32_t samples_per_chunk = d->stsc[d->stsc_count - 1].samples_per_chunk;
        for (uint32_t e = 0; e < d->stsc_count; e++) {
            uint32_t range_end = (e + 1 < d->stsc_count) ? d->stsc[e + 1].first_chunk : 0xFFFFFFFFu;
            if (chunk >= d->stsc[e].first_chunk && chunk < range_end) {
                samples_per_chunk = d->stsc[e].samples_per_chunk;
                break;
            }
        }

        uint64_t offset = chunk_offsets[chunk - 1];
        for (uint32_t s = 0; s < samples_per_chunk && sample_index < d->sample_count; s++, sample_index++) {
            uint32_t sz;
            if (!sample_size_at(d, sample_index, &sz)) {
                free(chunk_offsets);
                return false;
            }
            d->sample_offsets[sample_index] = offset;
            if (offset > d->file_size || sz > d->file_size - offset) { free(chunk_offsets); return false; }
            offset += sz;
        }
    }

    free(chunk_offsets);
    return sample_index == d->sample_count;
}

static bool parse_stts(mp4_demux_t * d, box_header_t stbl) {
    box_header_t stts_box;
    if (!find_child_box(d->f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stts", &stts_box)) return false;

    uint8_t hdr[8];
    if (!box_payload_has(stts_box, 0, 8)) return false;
    fseek(d->f, stts_box.data_start, SEEK_SET);
    if (fread(hdr, 1, 8, d->f) != 8) return false;
    uint32_t entry_count = audio_read_u32be(hdr + 4);
    if (entry_count == 0 || entry_count > MP4_MAX_STTS_ENTRIES) return false;
    if (!box_payload_has(stts_box, 8, (uint64_t) entry_count * 8U)) return false;

    d->stts = calloc(entry_count, sizeof(*d->stts));
    if (!d->stts) return false;
    d->stts_count = entry_count;
    uint64_t timed_samples = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint8_t entry[8];
        if (fread(entry, 1, 8, d->f) != 8) return false;
        uint32_t sample_count = audio_read_u32be(entry);
        uint32_t sample_delta = audio_read_u32be(entry + 4);
        if (sample_count == 0 || sample_delta == 0 || timed_samples + sample_count < timed_samples) return false;
        timed_samples += sample_count;
        d->stts[i].count = sample_count;
        d->stts[i].delta = sample_delta;
        d->stts[i].first_sample = timed_samples - sample_count;
        if (i == 0) d->first_sample_delta = sample_delta;
    }
    if (!d->media_timescale || !d->first_sample_delta || timed_samples != d->sample_count) return false;
    uint64_t total_ticks = 0;
    for (uint32_t i = 0; i < entry_count; i++) total_ticks += (uint64_t)d->stts[i].count * d->stts[i].delta;
    if (!d->sample_rate) return false;
    d->frames_per_sample = (uint32_t)ticks_to_frames(d->first_sample_delta, d->sample_rate, d->media_timescale);
    d->raw_total_pcm_frames = ticks_to_frames(total_ticks, d->sample_rate, d->media_timescale);
    d->total_pcm_frames = d->raw_total_pcm_frames;
    return total_ticks > 0;
}

static void apply_gapless_metadata(mp4_demux_t *d) {
    d->has_gapless = false;
    d->gapless_priming = 0;
    d->gapless_playable = 0;
    d->total_pcm_frames = d->raw_total_pcm_frames;
    if (d->edit_playable_ticks && d->movie_timescale && d->media_timescale) {
        uint64_t priming = ticks_to_frames(d->edit_priming_ticks, d->sample_rate, d->media_timescale);
        uint64_t playable = ticks_to_frames(d->edit_playable_ticks, d->sample_rate, d->movie_timescale);
        if (priming < d->raw_total_pcm_frames && playable > 0 &&
            playable <= d->raw_total_pcm_frames - priming + d->frames_per_sample) {
            d->gapless_priming = priming;
            d->gapless_playable = playable;
            d->has_gapless = true;
            d->total_pcm_frames = playable;
        }
    }
    if (d->has_itunsmpb && d->sample_entry_rate) {
        /* SMPB values are PCM sample counts, unlike edit-list media/movie
         * ticks. Keep these counts in decoder PCM units. */
        uint64_t priming = d->itunsmpb_priming;
        uint64_t padding = d->itunsmpb_padding;
        uint64_t playable = d->itunsmpb_playable;
        if (priming < d->sample_rate && padding < d->sample_rate && priming < d->raw_total_pcm_frames &&
            playable > 0 && playable <= d->raw_total_pcm_frames - priming + d->frames_per_sample &&
            padding <= d->raw_total_pcm_frames - priming + d->frames_per_sample - playable) {
            d->gapless_priming = priming;
            d->gapless_playable = playable;
            d->has_gapless = true;
            d->total_pcm_frames = playable;
        }
    }
}

static bool parse_media_timing(mp4_demux_t *d, box_header_t mdia) {
    box_header_t mdhd;
    if (!find_child_box(d->f, mdia.data_start, mdia.size - (uint64_t)mdia.header_size, "mdhd", &mdhd)) return false;
    uint8_t b[32];
    if (!box_payload_has(mdhd, 0, 1) || fseek(d->f, mdhd.data_start, SEEK_SET) != 0 || fread(b, 1, 1, d->f) != 1) return false;
    size_t need = b[0] == 1 ? 24 : 20;
    if (!box_payload_has(mdhd, 0, need) || fseek(d->f, mdhd.data_start, SEEK_SET) != 0 || fread(b, 1, need, d->f) != need) return false;
    d->media_timescale = b[0] == 1 ? audio_read_u32be(b + 20) : audio_read_u32be(b + 12);
    return d->media_timescale != 0;
}

static bool parse_edit_list(mp4_demux_t *d, box_header_t trak) {
    box_header_t edts, elst;
    if (!find_child_box(d->f, trak.data_start, trak.size - (uint64_t)trak.header_size, "edts", &edts) ||
        !find_child_box(d->f, edts.data_start, edts.size - (uint64_t)edts.header_size, "elst", &elst)) return true;
    uint8_t h[8];
    if (!box_payload_has(elst, 0, 8) || fseek(d->f, elst.data_start, SEEK_SET) != 0 || fread(h, 1, 8, d->f) != 8) return false;
    bool v1 = h[0] == 1;
    uint32_t count = audio_read_u32be(h + 4);
    size_t entry_size = v1 ? 20 : 12;
    if (count != 1 || !box_payload_has(elst, 8, (uint64_t)count * entry_size)) return true;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t e[20];
        if (fread(e, 1, entry_size, d->f) != entry_size) return false;
        int64_t media_time = v1 ? (int64_t)audio_read_u64be(e + 8) : (int32_t)audio_read_u32be(e + 4);
        size_t rate_at = v1 ? 16 : 8;
        if (media_time < 0 || audio_read_u32be(e + rate_at) != 0x00010000U) return true;
        uint64_t duration = v1 ? audio_read_u64be(e) : audio_read_u32be(e);
        d->edit_priming_ticks = (uint64_t)media_time;
        d->edit_playable_ticks = duration;
        return true;
    }
    return true;
}

static bool parse_movie_timescale(mp4_demux_t *d, box_header_t moov) {
    box_header_t mvhd;
    if (!find_child_box(d->f, moov.data_start, moov.size - (uint64_t)moov.header_size, "mvhd", &mvhd)) return true;
    uint8_t b[24];
    if (!box_payload_has(mvhd, 0, 1) || fseek(d->f, mvhd.data_start, SEEK_SET) != 0 || fread(b, 1, 1, d->f) != 1) return false;
    size_t need = b[0] == 1 ? 24 : 16;
    if (!box_payload_has(mvhd, 0, need) || fseek(d->f, mvhd.data_start, SEEK_SET) != 0 || fread(b, 1, need, d->f) != need) return false;
    d->movie_timescale = b[0] == 1 ? audio_read_u32be(b + 20) : audio_read_u32be(b + 12);
    return true;
}

static bool parse_smpb_text(const uint8_t *data, size_t size, uint64_t *priming,
                            uint64_t *padding, uint64_t *playable) {
    uint64_t values[4] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < 4; i++) {
        while (pos < size && (data[pos] == ' ' || data[pos] == '\t')) pos++;
        size_t digits = 0;
        while (pos < size) {
            uint8_t c = data[pos];
            unsigned int nibble;
            if (c >= '0' && c <= '9') nibble = c - '0';
            else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
            else break;
            if (digits == 16 || values[i] > (UINT64_MAX - nibble) / 16) return false;
            values[i] = values[i] * 16 + nibble;
            digits++;
            pos++;
        }
        if (digits == 0 || (i < 3 && (pos >= size || (data[pos] != ' ' && data[pos] != '\t')))) return false;
    }
    if (values[3] == 0) return false;
    *priming = values[1];
    *padding = values[2];
    *playable = values[3];
    return true;
}

static void parse_itunsmpb_item(mp4_demux_t *d, box_header_t item) {
    uint64_t payload = item.size - (uint64_t)item.header_size;
    if (payload > (uint64_t)LONG_MAX || fseek(d->f, item.data_start, SEEK_SET) != 0) return;
    long end = item.data_start + (long)payload;
    bool named = false;
    uint64_t priming = 0, padding = 0, playable = 0;
    while (ftell(d->f) >= 0 && ftell(d->f) < end) {
        box_header_t child;
        if (!read_box_header(d->f, &child)) return;
        long start = child.data_start - child.header_size;
        if (start < item.data_start || child.size > (uint64_t)(end - start)) return;
        if (strcmp(child.type, "name") == 0 && child.size >= (uint64_t)child.header_size + 4 + 8) {
            char name[16] = {0};
            if (fseek(d->f, child.data_start + 4, SEEK_SET) == 0 && fread(name, 1, 8, d->f) == 8 && memcmp(name, "iTunSMPB", 8) == 0) named = true;
        } else if (strcmp(child.type, "data") == 0 && child.size >= (uint64_t)child.header_size + 8) {
            size_t n = (size_t)(child.size - (uint64_t)child.header_size - 8);
            if (n > 256) n = 256;
            uint8_t text[256];
            if (fseek(d->f, child.data_start + 8, SEEK_SET) == 0 && fread(text, 1, n, d->f) == n)
                (void)parse_smpb_text(text, n, &priming, &padding, &playable);
        }
        if (fseek(d->f, start + (long)child.size, SEEK_SET) != 0) return;
    }
    if (named && playable && d->sample_entry_rate && priming < d->sample_entry_rate &&
        padding < d->sample_entry_rate) {
        d->itunsmpb_priming = priming;
        d->itunsmpb_padding = padding;
        d->itunsmpb_playable = playable;
        d->has_itunsmpb = true;
    }
}

static void parse_itunsmpb(mp4_demux_t *d, box_header_t moov) {
    box_header_t udta, meta, ilst;
    if (!find_child_box(d->f, moov.data_start, moov.size - (uint64_t)moov.header_size, "udta", &udta) ||
        !find_child_box(d->f, udta.data_start, udta.size - (uint64_t)udta.header_size, "meta", &meta) ||
        !box_payload_has(meta, 0, 4) ||
        !find_child_box(d->f, meta.data_start + 4, meta.size - (uint64_t)meta.header_size - 4, "ilst", &ilst)) return;
    long end = ilst.data_start + (long)ilst.size - ilst.header_size;
    if (fseek(d->f, ilst.data_start, SEEK_SET) != 0) return;
    while (ftell(d->f) >= 0 && ftell(d->f) < end) {
        box_header_t item;
        if (!read_box_header(d->f, &item)) return;
        long start = item.data_start - item.header_size;
        if (start < ilst.data_start || item.size > (uint64_t)(end - start)) return;
        if (strcmp(item.type, "----") == 0) {
            parse_itunsmpb_item(d, item);
            if (d->has_itunsmpb) return;
        }
        if (fseek(d->f, start + (long)item.size, SEEK_SET) != 0) return;
    }
}

/* A 20-minute AAC moov is a couple of hundred KB. 4 MiB still covers a long
 * audiobook's sample tables and stays small next to 56 MiB of RAM, and also
 * bounds what a larger moov's sparse copy may hold. Boxes before moov (ftyp,
 * free, uuid) are copied only while small, so an mdat before moov is seeked
 * over instead of downloaded. */
#define MP4_MOOV_CACHE_MAX (4u * 1024u * 1024u)
#define MP4_PREFIX_BOX_MAX (64u * 1024u)
#define MP4_CACHE_MAX_SPANS 256
/* Kept from a large leaf box: covers the stsz (12) and stco/co64 (8) headers
 * the demuxer reads before it windows their entries. */
#define MP4_LARGE_BOX_HEAD 16u

typedef struct {
    FILE * file;
    uint64_t file_size;
    uint64_t pos; /* the FILE's position; reads elsewhere seek first */
    uint8_t * bytes;
    size_t used, capacity;
    mp4_span_t * spans;
    size_t count;
    bool full; /* stop copying; what is already cached stays valid */
    bool failed; /* read error */
} moov_cache_t;

/* Reads length bytes at offset into the cache, or only moves past them once
 * the cache is full. Copies stay in file order, merged when adjacent. */
static void cache_read(moov_cache_t * c, uint64_t offset, uint64_t length) {
    if (c->failed || !length) return;
    if (offset + length > c->file_size) { c->failed = true; return; }
    bool keep = !c->full && length <= MP4_MOOV_CACHE_MAX - c->used;
    bool merge = keep && c->count && c->spans[c->count - 1].offset + c->spans[c->count - 1].length == offset;
    if (keep && !merge && c->count == MP4_CACHE_MAX_SPANS) keep = false;
    if (keep && c->used + length > c->capacity) {
        size_t want = c->capacity ? c->capacity : 64u * 1024u;
        while (want < c->used + length) want *= 2;
        if (want > MP4_MOOV_CACHE_MAX) want = MP4_MOOV_CACHE_MAX;
        uint8_t * grown = realloc(c->bytes, want);
        if (grown) { c->bytes = grown; c->capacity = want; } else keep = false;
    }
    if (keep && !c->spans && !(c->spans = malloc(sizeof(*c->spans) * MP4_CACHE_MAX_SPANS))) keep = false;
    if (!keep) {
        c->full = true;
        return;
    }
    if (c->pos != offset && fseek(c->file, (long) offset, SEEK_SET) != 0) { c->failed = true; return; }
    if (fread(c->bytes + c->used, 1, (size_t) length, c->file) != length) { c->failed = true; return; }
    c->pos = offset + length;
    c->used += (size_t) length;
    if (merge) c->spans[c->count - 1].length += length;
    else c->spans[c->count++] = (mp4_span_t) { offset, length };
}

static const uint8_t * cache_tail(const moov_cache_t * c, size_t length) {
    return c->bytes + c->used - length;
}

/* Box header at offset: copies it and returns the box and header sizes, or
 * false at a malformed box. The header must be cached to be parsed, so a
 * full cache still reads it into a scratch buffer. */
static bool cache_box_header(moov_cache_t * c, uint64_t offset, uint64_t end, uint8_t type[4],
                             uint64_t * box_size, unsigned * header_size) {
    if (offset + 8 > end) return false;
    uint8_t hdr[16];
    size_t before = c->used;
    cache_read(c, offset, 8);
    if (c->failed) return false;
    if (c->used == before + 8) memcpy(hdr, cache_tail(c, 8), 8);
    else {
        if (fseek(c->file, (long) offset, SEEK_SET) != 0 || fread(hdr, 1, 8, c->file) != 8) { c->failed = true; return false; }
        c->pos = offset + 8;
    }
    uint32_t size32 = audio_read_u32be(hdr);
    *header_size = 8;
    if (size32 == 1) {
        if (offset + 16 > end) return false;
        before = c->used;
        cache_read(c, offset + 8, 8);
        if (c->failed) return false;
        if (c->used == before + 8) memcpy(hdr + 8, cache_tail(c, 8), 8);
        else {
            if (fseek(c->file, (long) (offset + 8), SEEK_SET) != 0 || fread(hdr + 8, 1, 8, c->file) != 8) { c->failed = true; return false; }
            c->pos = offset + 16;
        }
        *box_size = audio_read_u64be(hdr + 8);
        *header_size = 16;
    } else {
        *box_size = size32 ? size32 : end - offset;
    }
    memcpy(type, hdr + 4, 4);
    return *box_size >= *header_size && *box_size <= end - offset;
}

/* Sparse copy of a moov too large to cache whole: every box header, every
 * small box, and the first bytes of large leaves (sample tables, cover
 * art, padding). The demuxer's parse then stays in RAM and only its table
 * windows and iTunes metadata inside large boxes touch the network. */
static void cache_moov_sparse(moov_cache_t * c, uint64_t start, uint64_t end, int depth) {
    uint64_t at = start;
    while (!c->failed && at + 8 <= end) {
        uint8_t type[4];
        uint64_t size;
        unsigned header;
        /* A box that does not parse (a guessed layout, such as QuickTime's
         * meta without version bytes) ends this level only: what is cached
         * is still file bytes, and the rest stays on the network. */
        if (!cache_box_header(c, at, end, type, &size, &header)) return;
        uint64_t payload = at + header, next = at + size;
        bool container = !memcmp(type, "trak", 4) || !memcmp(type, "mdia", 4) || !memcmp(type, "minf", 4) ||
                         !memcmp(type, "stbl", 4) || !memcmp(type, "edts", 4) || !memcmp(type, "dinf", 4) ||
                         !memcmp(type, "udta", 4) || !memcmp(type, "ilst", 4) || !memcmp(type, "meta", 4);
        if (container && depth < 8 && size > MP4_PREFIX_BOX_MAX) {
            /* meta is a full box: version and flags precede its children. */
            uint64_t children = payload + (!memcmp(type, "meta", 4) ? 4 : 0);
            if (children > next) return;
            cache_read(c, payload, children - payload);
            cache_moov_sparse(c, children, next, depth + 1);
        } else if (size <= MP4_PREFIX_BOX_MAX) {
            cache_read(c, payload, next - payload);
        } else {
            cache_read(c, payload, next - payload < MP4_LARGE_BOX_HEAD ? next - payload : MP4_LARGE_BOX_HEAD);
        }
        at = next;
    }
}

bool mp4_cache_moov(FILE * file, uint8_t ** out_bytes, mp4_span_t ** out_spans, size_t * out_count) {
    if (out_bytes) *out_bytes = NULL;
    if (out_spans) *out_spans = NULL;
    if (out_count) *out_count = 0;
    if (!file || !out_bytes || !out_spans || !out_count) return false;
    if (fseek(file, 0, SEEK_END) != 0) return false;
    long file_size = ftell(file);
    if (file_size <= 0 || fseek(file, 0, SEEK_SET) != 0) return false;

    moov_cache_t c = { .file = file, .file_size = (uint64_t) file_size };
    bool found = false;
    uint64_t at = 0;
    while (!c.failed && !found && at + 8 <= c.file_size) {
        uint8_t type[4];
        uint64_t size;
        unsigned header;
        if (!cache_box_header(&c, at, c.file_size, type, &size, &header)) break;
        uint64_t payload = at + header, next = at + size;
        if (!memcmp(type, "moov", 4)) {
            found = true;
            if (size <= MP4_MOOV_CACHE_MAX - c.used) cache_read(&c, payload, next - payload);
            else cache_moov_sparse(&c, payload, next, 0);
        } else if (size <= MP4_PREFIX_BOX_MAX) {
            cache_read(&c, payload, next - payload);
        }
        at = next; /* a large box (mdat) is skipped; the next read seeks */
    }
    if (!found || c.failed || !c.count) {
        free(c.bytes);
        free(c.spans);
        return found && !c.failed;
    }
    *out_bytes = c.bytes;
    *out_spans = c.spans;
    *out_count = c.count;
    return true;
}

mp4_demux_t * mp4_demux_open(const char * path) {
    return mp4_demux_open_stream(fopen(path, "rb"));
}

mp4_demux_t * mp4_demux_open_stream(FILE * f) {
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long file_size = ftell(f);
    if (file_size <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    box_header_t moov;
    bool found_moov = false;
    while (ftell(f) < file_size) {
        box_header_t box;
        if (!read_box_header(f, &box)) break;
        if (strcmp(box.type, "moov") == 0) {
            moov = box;
            found_moov = true;
            break;
        }
        long next = (long) (box.data_start - box.header_size) + (long) box.size;
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    if (!found_moov) {
        fclose(f);
        return NULL;
    }

    box_header_t trak;
    if (!find_audio_trak(f, moov, &trak)) {
        fclose(f);
        return NULL;
    }

    box_header_t mdia, minf, stbl;
    if (!find_child_box(f, trak.data_start, trak.size - (uint64_t) trak.header_size, "mdia", &mdia) ||
        !find_child_box(f, mdia.data_start, mdia.size - (uint64_t) mdia.header_size, "minf", &minf) ||
        !find_child_box(f, minf.data_start, minf.size - (uint64_t) minf.header_size, "stbl", &stbl)) {
        fclose(f);
        return NULL;
    }

    box_header_t stsd, stsz;
    if (!find_child_box(f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stsd", &stsd) ||
        !find_child_box(f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stsz", &stsz)) {
        fclose(f);
        return NULL;
    }

    mp4_demux_t * d = calloc(1, sizeof(*d));
    if (!d) {
        fclose(f);
        return NULL;
    }
    d->f = f;
    d->file_size = (uint64_t) file_size;

    if (!parse_stsd(d, stsd) || !parse_media_timing(d, mdia) || !parse_movie_timescale(d, moov) ||
        !parse_edit_list(d, trak) || !parse_stsz(d, stsz) || !parse_sample_offsets(d, stbl) || !parse_stts(d, stbl)) {
        mp4_demux_close(d);
        return NULL;
    }

    parse_itunsmpb(d, moov);
    apply_gapless_metadata(d);

    return d;
}

bool mp4_demux_peek_codec_stream(FILE *f, char out_fourcc[5]) {
    if (!f || !out_fourcc) return false;
    long original = ftell(f);
    if (original < 0 || fseek(f, 0, SEEK_END) != 0) return false;
    long file_size = ftell(f);
    if (file_size <= 0 || fseek(f, 0, SEEK_SET) != 0) { (void)fseek(f, original, SEEK_SET); return false; }
    box_header_t moov = {0};
    bool found = false;
    while (ftell(f) < file_size) {
        box_header_t box;
        if (!read_box_header(f, &box)) break;
        long start = box.data_start - box.header_size;
        if (box.size > (uint64_t) (file_size - start)) break;
        if (strcmp(box.type, "moov") == 0) { moov = box; found = true; break; }
        if (fseek(f, start + (long) box.size, SEEK_SET) != 0) break;
    }
    box_header_t trak, mdia, minf, stbl, stsd;
    uint8_t entry[12];
    bool ok = found && find_audio_trak(f, moov, &trak) &&
              find_child_box(f, trak.data_start, trak.size - (uint64_t) trak.header_size, "mdia", &mdia) &&
              find_child_box(f, mdia.data_start, mdia.size - (uint64_t) mdia.header_size, "minf", &minf) &&
              find_child_box(f, minf.data_start, minf.size - (uint64_t) minf.header_size, "stbl", &stbl) &&
              find_child_box(f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stsd", &stsd) &&
              box_payload_has(stsd, 0, 20) && fseek(f, stsd.data_start + 8, SEEK_SET) == 0 &&
              fread(entry, 1, sizeof(entry), f) == sizeof(entry) && audio_read_u32be(entry) >= 36;
    if (ok) { memcpy(out_fourcc, entry + 4, 4); out_fourcc[4] = '\0'; }
    if (fseek(f, original, SEEK_SET) != 0) return false;
    return ok;
}

bool mp4_demux_peek_codec(const char * path, char out_fourcc[5]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = mp4_demux_peek_codec_stream(f, out_fourcc);
    fclose(f);
    return ok;
}

void mp4_demux_get_codec_fourcc(const mp4_demux_t * d, char out_fourcc[5]) {
    memcpy(out_fourcc, d->codec_fourcc, 5);
}

const uint8_t * mp4_demux_get_codec_config(const mp4_demux_t * d, uint32_t * out_size) {
    *out_size = d->codec_config_size;
    return d->codec_config;
}

uint32_t mp4_demux_get_sample_count(const mp4_demux_t * d) {
    return d->sample_count;
}

uint32_t mp4_demux_get_frames_per_sample(const mp4_demux_t * d) {
    return d->frames_per_sample;
}

uint64_t mp4_demux_get_total_pcm_frame_count(const mp4_demux_t * d) {
    return d->total_pcm_frames;
}

void mp4_demux_set_pcm_sample_rate(mp4_demux_t *d, uint32_t sample_rate) {
    if (!d || !sample_rate || !d->sample_entry_rate || !d->media_timescale) return;
    d->sample_rate = sample_rate;
    uint64_t ticks = 0;
    for (uint32_t i = 0; i < d->stts_count; i++) ticks += (uint64_t)d->stts[i].count * d->stts[i].delta;
    d->raw_total_pcm_frames = ticks_to_frames(ticks, d->sample_rate, d->media_timescale);
    d->frames_per_sample = (uint32_t)ticks_to_frames(d->first_sample_delta, d->sample_rate, d->media_timescale);
    apply_gapless_metadata(d);
}

bool mp4_demux_get_gapless_trim(const mp4_demux_t *d, uint64_t *priming, uint64_t *playable) {
    if (!d || !d->has_gapless) return false;
    if (priming) *priming = d->gapless_priming;
    if (playable) *playable = d->gapless_playable;
    return true;
}

bool mp4_demux_get_sample_pcm_duration(const mp4_demux_t *d, uint32_t sample, uint64_t *duration) {
    if (!d || !duration || sample >= d->sample_count) return false;
    uint64_t ticks = 0;
    for (uint32_t i = 0; i < d->stts_count; i++) {
        const stts_entry_t *e = &d->stts[i];
        if (sample >= e->first_sample && sample - e->first_sample < e->count) {
            uint64_t before = ticks + (uint64_t)(sample - e->first_sample) * e->delta;
            uint64_t after = before + e->delta;
            *duration = ticks_to_frames(after, d->sample_rate, d->media_timescale) -
                        ticks_to_frames(before, d->sample_rate, d->media_timescale);
            return true;
        }
        ticks += (uint64_t)e->count * e->delta;
    }
    return false;
}

bool mp4_demux_map_pcm_frame(const mp4_demux_t *d, uint64_t frame, uint32_t *sample, uint64_t *discard) {
    if (!d || !sample || !discard || frame > d->raw_total_pcm_frames) return false;
    uint64_t target = frame + (d->has_gapless ? d->gapless_priming : 0);
    uint64_t ticks = 0;
    for (uint32_t i = 0; i < d->stts_count; i++) {
        const stts_entry_t *e = &d->stts[i];
        uint64_t run_ticks = (uint64_t)e->count * e->delta;
        uint64_t begin = ticks_to_frames(ticks, d->sample_rate, d->media_timescale);
        uint64_t end = ticks_to_frames(ticks + run_ticks, d->sample_rate, d->media_timescale);
        if (target < end || (target == end && i + 1 == d->stts_count)) {
            uint64_t local = target > begin ? target - begin : 0;
            uint64_t lo = 0, hi = e->count;
            while (lo + 1 < hi) {
                uint64_t mid = lo + (hi - lo) / 2;
                if (ticks_to_frames(ticks + mid * e->delta, d->sample_rate, d->media_timescale) - begin <= local) lo = mid;
                else hi = mid;
            }
            uint64_t sample_offset = lo;
            *sample = (uint32_t)(e->first_sample + sample_offset);
            uint64_t sample_start_ticks = ticks + sample_offset * e->delta;
            *discard = target - ticks_to_frames(sample_start_ticks, d->sample_rate, d->media_timescale);
            return true;
        }
        ticks += run_ticks;
    }
    *sample = d->sample_count - 1;
    *discard = 0;
    return true;
}

bool mp4_demux_read_sample(mp4_demux_t * d, uint32_t sample_index, uint8_t * buf, uint32_t buf_size, uint32_t * out_size) {
    uint32_t size;
    uint64_t offset;
    if (!sample_size_at(d, sample_index, &size) || !sample_offset_at(d, sample_index, &offset)) return false;
    if (size > buf_size || offset > d->file_size || size > d->file_size - offset || offset > (uint64_t) LONG_MAX) return false;

    if (fseek(d->f, (long) offset, SEEK_SET) != 0) return false;
    if (fread(buf, 1, size, d->f) != size) return false;

    *out_size = size;
    return true;
}

void mp4_demux_close(mp4_demux_t * d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    free(d->codec_config);
    free(d->sample_sizes);
    free(d->sample_offsets);
    free(d->size_window.bytes);
    free(d->chunk_window.bytes);
    free(d->stsc);
    free(d->stts);
    free(d);
}
