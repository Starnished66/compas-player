#include "ape_demux.h"
#include "audio_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

#define APE_MAX_BLOCKS_PER_FRAME (73728U * 16U)
/* Prevent a tiny or sparse seek table from requesting impractical long-lived
 * index memory: ape_frame_t is 24 bytes, so this caps the index near 3 MiB. */
#define APE_MAX_FRAME_COUNT 131072U

static bool ape_seek_abs(FILE * f, uint64_t offset) {
    if (offset > (uint64_t)LONG_MAX) return false;
    return fseek(f, (long)offset, SEEK_SET) == 0;
}

static bool ape_get_file_size(FILE * f, uint64_t * out_size) {
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) return false;
    *out_size = (uint64_t)size;
    return true;
}

static bool ape_add_u64(uint64_t a, uint64_t b, uint64_t * out) {
    if (UINT64_MAX - a < b) return false;
    *out = a + b;
    return true;
}

typedef struct {
    uint64_t pos;
    uint64_t size;
    uint32_t nblocks;
    uint32_t skip;
} ape_frame_t;

struct ape_demux {
    FILE * f;

    int fileversion;
    uint16_t compressiontype;
    uint16_t formatflags;
    uint32_t blocksperframe;
    uint32_t finalframeblocks;
    uint32_t totalframes;
    uint16_t bps;
    uint16_t channels;
    uint32_t samplerate;

    uint64_t total_samples;
    ape_frame_t * frames;
};

ape_demux_t * ape_demux_open(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    uint64_t file_size;
    if (!ape_get_file_size(f, &file_size)) { fclose(f); return NULL; }

    uint8_t hdr[4];
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "MAC ", 4) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t buf[36];
    if (fread(buf, 1, 2, f) != 2) { fclose(f); return NULL; }
    int fileversion = audio_read_u16le(buf);
    /* Only the modern (fileversion >= 3980) descriptor+header layout is
     * supported -- see ape_demux.h for why that's not a practical
     * limitation. */
    if (fileversion < 3980 || fileversion > 3990) {
        fclose(f);
        return NULL;
    }

    /* The rest of the fixed 52-byte descriptor (52 = 4 "MAC " + 2
     * fileversion + this 46-byte tail): padding1(2, ignored),
     * descriptorlength(4), headerlength(4), seektablelength(4),
     * wavheaderlength(4), audiodatalength(4), audiodatalength_high(4,
     * ignored), wavtaillength(4), md5(16, ignored). */
    uint8_t desc[46];
    if (fread(desc, 1, 46, f) != 46) { fclose(f); return NULL; }
    uint32_t descriptorlength = audio_read_u32le(desc + 2);
    uint32_t headerlength = audio_read_u32le(desc + 6);
    uint32_t seektablelength = audio_read_u32le(desc + 10);
    uint32_t wavheaderlength = audio_read_u32le(desc + 14);
    uint32_t audiodatalength = audio_read_u32le(desc + 18);
    uint32_t audiodatalength_high = audio_read_u32le(desc + 22);
    uint32_t wavtaillength = audio_read_u32le(desc + 26);

    if (descriptorlength < 52 || headerlength < 24) { fclose(f); return NULL; }
    uint64_t header_start = descriptorlength;
    uint64_t table_start, audio_start;
    if (!ape_add_u64(header_start, headerlength, &table_start) ||
        !ape_add_u64(table_start, seektablelength, &audio_start) ||
        !ape_add_u64(audio_start, wavheaderlength, &audio_start) ||
        audio_start > file_size || !ape_seek_abs(f, header_start)) {
        fclose(f); return NULL;
    }

    uint8_t hb[24];
    if (fread(hb, 1, 24, f) != 24) { fclose(f); return NULL; }

    if (!ape_seek_abs(f, table_start)) { fclose(f); return NULL; }

    ape_demux_t * d = calloc(1, sizeof(*d));
    if (!d) { fclose(f); return NULL; }
    d->f = f;
    d->fileversion = fileversion;
    d->compressiontype = audio_read_u16le(hb + 0);
    d->formatflags = audio_read_u16le(hb + 2);
    d->blocksperframe = audio_read_u32le(hb + 4);
    d->finalframeblocks = audio_read_u32le(hb + 8);
    d->totalframes = audio_read_u32le(hb + 12);
    d->bps = audio_read_u16le(hb + 16);
    d->channels = audio_read_u16le(hb + 18);
    d->samplerate = audio_read_u32le(hb + 20);

    uint64_t min_table_size = (uint64_t)d->totalframes * 4U;
    uint64_t audio_data_size = ((uint64_t)audiodatalength_high << 32) | audiodatalength;
    uint64_t audio_end;
    if (!d->totalframes || d->totalframes > APE_MAX_FRAME_COUNT ||
        min_table_size > seektablelength ||
        d->blocksperframe == 0 || d->blocksperframe > APE_MAX_BLOCKS_PER_FRAME ||
        d->finalframeblocks == 0 || d->finalframeblocks > d->blocksperframe ||
        d->channels == 0 || d->channels > 2 || (d->bps != 16 && d->bps != 24) ||
        d->samplerate == 0 ||
        (uint64_t)d->totalframes > SIZE_MAX / sizeof(ape_frame_t) ||
        (audio_data_size != 0 && !ape_add_u64(audio_start, audio_data_size, &audio_end))) {
        fclose(f);
        free(d);
        return NULL;
    }

    if (audio_data_size == 0) {
        if (wavtaillength > file_size - audio_start) { fclose(f); free(d); return NULL; }
        audio_end = file_size - wavtaillength;
    }
    if (audio_end < audio_start || audio_end > file_size ||
        wavtaillength > file_size - audio_end) { fclose(f); free(d); return NULL; }

    d->total_samples = d->finalframeblocks;
    if (d->totalframes > 1) d->total_samples += (uint64_t) d->blocksperframe * (d->totalframes - 1);

    d->frames = calloc((size_t)d->totalframes, sizeof(ape_frame_t));
    if (!d->frames) {
        fclose(f);
        free(d);
        return NULL;
    }

    d->frames[0].pos = audio_start;
    d->frames[0].nblocks = d->blocksperframe;

    uint8_t entry[4];
    if (fread(entry, 1, 4, f) != 4) { /* first entry is redundant in this layout */
        ape_demux_close(d); return NULL;
    }
    for (uint32_t i = 1; i < d->totalframes; i++) {
        if (fread(entry, 1, 4, f) != 4) { ape_demux_close(d); return NULL; }
        uint32_t seektable_entry = audio_read_u32le(entry);
        if ((uint64_t)seektable_entry <= d->frames[i - 1].pos ||
            (uint64_t)seektable_entry >= audio_end) { ape_demux_close(d); return NULL; }
        d->frames[i].pos = seektable_entry;
        d->frames[i].nblocks = d->blocksperframe;
        d->frames[i - 1].size = d->frames[i].pos - d->frames[i - 1].pos;
        d->frames[i].skip = (uint32_t)((d->frames[i].pos - d->frames[0].pos) & 3U);
    }
    d->frames[0].skip = 0;

    d->frames[d->totalframes - 1].nblocks = d->finalframeblocks;

    uint64_t last_pos = d->frames[d->totalframes - 1].pos;
    if (last_pos >= audio_end) { ape_demux_close(d); return NULL; }
    uint64_t final_size = audio_end - last_pos;
    final_size -= final_size & 3U; /* preserve the decoder's whole-word tail behavior */
    if (final_size == 0) { ape_demux_close(d); return NULL; }
    d->frames[d->totalframes - 1].size = final_size;

    for (uint32_t i = 0; i < d->totalframes; i++) {
        if (UINT64_MAX - d->frames[i].size < d->frames[i].skip) {
            ape_demux_close(d); return NULL;
        }
        if (d->frames[i].skip > d->frames[i].pos) {
            ape_demux_close(d); return NULL;
        }
        if (d->frames[i].skip) {
            d->frames[i].pos -= d->frames[i].skip;
            d->frames[i].size += d->frames[i].skip;
        }
        if (UINT64_MAX - d->frames[i].size < 3U) {
            ape_demux_close(d); return NULL;
        }
        d->frames[i].size = (d->frames[i].size + 3U) & ~(uint64_t)3U;
        uint64_t rounded_end;
        if (!ape_add_u64(d->frames[i].pos, d->frames[i].size, &rounded_end) ||
            rounded_end > file_size || d->frames[i].size > UINT32_MAX) {
            ape_demux_close(d); return NULL;
        }
    }

    return d;
}

unsigned int ape_demux_get_channels(const ape_demux_t * d) { return d->channels; }
unsigned int ape_demux_get_sample_rate(const ape_demux_t * d) { return d->samplerate; }
unsigned int ape_demux_get_bits_per_sample(const ape_demux_t * d) { return d->bps; }
int ape_demux_get_fileversion(const ape_demux_t * d) { return d->fileversion; }
int ape_demux_get_compression_level(const ape_demux_t * d) { return d->compressiontype; }
uint64_t ape_demux_get_total_samples(const ape_demux_t * d) { return d->total_samples; }
uint32_t ape_demux_get_frame_count(const ape_demux_t * d) { return d->totalframes; }
uint32_t ape_demux_get_blocks_per_frame(const ape_demux_t * d) { return d->blocksperframe; }
uint32_t ape_demux_get_final_frame_blocks(const ape_demux_t * d) { return d->finalframeblocks; }

bool ape_demux_read_frame(ape_demux_t * d, uint32_t frame_index, uint8_t * buf, uint32_t buf_size, uint32_t * out_size) {
    if (frame_index >= d->totalframes) return false;
    uint64_t size = d->frames[frame_index].size;
    if (size > buf_size) return false;

    if (fseek(d->f, (long) d->frames[frame_index].pos, SEEK_SET) != 0) return false;
    if (fread(buf, 1, (size_t) size, d->f) != (size_t) size) return false;

    *out_size = (uint32_t) size;
    return true;
}

uint32_t ape_demux_get_frame_skip(const ape_demux_t * d, uint32_t frame_index) {
    if (frame_index >= d->totalframes) return 0;
    return d->frames[frame_index].skip;
}

void ape_demux_close(ape_demux_t * d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    free(d->frames);
    free(d);
}
