/* APE container reader modeled on FFmpeg's libavformat/ape.c (LGPL-2.1+). */
#include "ape_demux.h"
#include "audio_helpers.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APE_MAX_FRAMES (1U << 16)
#define APE_MAX_BLOCKS (1U << 24)
#define APE_MAX_FRAME_BYTES (16U * 1024U * 1024U)
#define APE_FORMAT_8_BIT 1U
#define APE_FORMAT_PEAK 4U
#define APE_FORMAT_24_BIT 8U
#define APE_FORMAT_SEEK_ELEMENTS 16U
#define APE_FORMAT_CREATE_WAV 32U

typedef struct {
    uint64_t pos;
    uint64_t size;
    uint32_t nblocks;
    uint32_t skip;
} ape_frame_t;

struct ape_demux {
    FILE *f;
    int version;
    uint16_t compression;
    uint16_t flags;
    uint16_t channels;
    uint16_t bps;
    uint32_t samplerate;
    uint32_t blocksperframe;
    uint32_t finalframeblocks;
    uint32_t totalframes;
    uint64_t total_samples;
    ape_frame_t *frames;
};

static bool seek_abs(FILE *f, uint64_t off) {
    if (off > LONG_MAX) return false;
    return fseek(f, (long)off, SEEK_SET) == 0;
}

static bool file_size(FILE *f, uint64_t *size) {
    if (fseek(f, 0, SEEK_END)) return false;
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET)) return false;
    *size = (uint64_t)n;
    return true;
}

static bool add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (UINT64_MAX - a < b) return false;
    *out = a + b;
    return true;
}

ape_demux_t *ape_demux_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint64_t size;
    uint8_t b[64];
    if (!file_size(f, &size) || size < 32 || fread(b, 1, 6, f) != 6 || memcmp(b, "MAC ", 4)) {
        fclose(f); return NULL;
    }
    int version = (int)audio_read_u16le(b + 4);
    if (version < 3800 || version > 3990) { fclose(f); return NULL; }

    uint64_t descriptor_len = 0;
    uint32_t header_len = 0, seek_len = 0, wav_header_len = 0, wav_tail_len = 0;
    uint32_t blocks_per_frame, final_blocks, frames;
    uint16_t compression, flags, channels, bps;
    uint32_t rate;
    uint64_t table_pos;
    if (version >= 3980) {
        if (fread(b, 1, 46, f) != 46) { fclose(f); return NULL; }
        descriptor_len = audio_read_u32le(b + 2);
        header_len = audio_read_u32le(b + 6);
        seek_len = audio_read_u32le(b + 10);
        wav_header_len = audio_read_u32le(b + 14);
        uint32_t audio_len = audio_read_u32le(b + 18);
        uint32_t audio_len_hi = audio_read_u32le(b + 22);
        wav_tail_len = audio_read_u32le(b + 26);
        if (descriptor_len < 52 || descriptor_len > 4096 || header_len < 24 || header_len > 4096 ||
            !seek_abs(f, descriptor_len)) { fclose(f); return NULL; }
        if (fread(b, 1, 24, f) != 24) { fclose(f); return NULL; }
        compression = audio_read_u16le(b);
        flags = audio_read_u16le(b + 2);
        blocks_per_frame = audio_read_u32le(b + 4);
        final_blocks = audio_read_u32le(b + 8);
        frames = audio_read_u32le(b + 12);
        bps = audio_read_u16le(b + 16);
        channels = audio_read_u16le(b + 18);
        rate = audio_read_u32le(b + 20);
        uint64_t audio_start;
        if (!add_u64(descriptor_len, header_len, &table_pos) ||
            !add_u64(table_pos, seek_len, &audio_start) ||
            !add_u64(audio_start, wav_header_len, &audio_start) || audio_start > size) {
            fclose(f); return NULL;
        }
        uint64_t audio_bytes = ((uint64_t)audio_len_hi << 32) | audio_len;
        uint64_t audio_end;
        if (audio_bytes) {
            uint64_t declared_end;
            if (!add_u64(audio_start, audio_bytes, &declared_end)) { fclose(f); return NULL; }
            audio_end = declared_end < size ? declared_end : size;
        } else {
            if (wav_tail_len > size - audio_start) { fclose(f); return NULL; }
            audio_end = size - wav_tail_len;
        }
        if (audio_end > size || audio_end < audio_start || wav_tail_len > size - audio_end) {
            fclose(f); return NULL;
        }
        if (!seek_abs(f, table_pos)) { fclose(f); return NULL; }
        /* For the modern layout the first seek-table entry is redundant. */
        uint64_t first_pos = audio_start;
        uint64_t end_pos = audio_end;
        ape_demux_t *d = calloc(1, sizeof(*d));
        if (!d) { fclose(f); return NULL; }
        d->f = f; d->version = version; d->compression = compression; d->flags = flags;
        d->channels = channels; d->bps = bps; d->samplerate = rate;
        d->blocksperframe = blocks_per_frame; d->finalframeblocks = final_blocks; d->totalframes = frames;
        uint64_t minimum = (uint64_t)frames * 4;
        if (!frames || frames > APE_MAX_FRAMES || minimum > seek_len || !blocks_per_frame ||
            blocks_per_frame > APE_MAX_BLOCKS || !final_blocks || final_blocks > blocks_per_frame ||
            channels < 1 || channels > 2 || (bps != 8 && bps != 16 && bps != 24) || !rate) {
            ape_demux_close(d); return NULL;
        }
        d->frames = calloc(frames, sizeof(*d->frames));
        if (!d->frames) { ape_demux_close(d); return NULL; }
        uint8_t ent[4];
        if (fread(ent, 1, 4, f) != 4) { ape_demux_close(d); return NULL; }
        d->frames[0].pos = first_pos;
        d->frames[0].nblocks = blocks_per_frame;
        for (uint32_t i = 1; i < frames; i++) {
            if (fread(ent, 1, 4, f) != 4) { ape_demux_close(d); return NULL; }
            uint64_t pos = audio_read_u32le(ent);
            if (pos <= d->frames[i - 1].pos) { ape_demux_close(d); return NULL; }
            d->frames[i].pos = pos;
            d->frames[i].nblocks = blocks_per_frame;
            d->frames[i - 1].size = d->frames[i - 1].pos < end_pos
                ? (pos < end_pos ? pos : end_pos) - d->frames[i - 1].pos : 0;
            d->frames[i].skip = (uint32_t)((pos - first_pos) & 3U);
        }
        d->frames[frames - 1].nblocks = final_blocks;
        uint64_t last_size = d->frames[frames - 1].pos < end_pos
            ? end_pos - d->frames[frames - 1].pos : 0;
        last_size -= last_size & 3U;
        d->frames[frames - 1].size = last_size;
        for (uint32_t i = 0; i < frames; i++) {
            ape_frame_t *fr = &d->frames[i];
            if (fr->pos >= end_pos) { fr->size = 0; continue; }
            if (fr->skip > fr->pos) { ape_demux_close(d); return NULL; }
            fr->pos -= fr->skip;
            fr->size += fr->skip;
            fr->size = (fr->size + 3U) & ~(uint64_t)3U;
            uint64_t fend;
            if (!add_u64(fr->pos, fr->size, &fend) || fend > size || fr->size > APE_MAX_FRAME_BYTES) {
                fr->size = 0;
            }
        }
        d->total_samples = (uint64_t)blocks_per_frame * (frames - 1) + final_blocks;
        return d;
    }

    /* Legacy descriptor (file versions 3.80 through 3.97). */
    if (fread(b, 1, 26, f) != 26) { fclose(f); return NULL; }
    compression = audio_read_u16le(b);
    flags = audio_read_u16le(b + 2);
    channels = audio_read_u16le(b + 4);
    rate = audio_read_u32le(b + 6);
    wav_header_len = audio_read_u32le(b + 10);
    wav_tail_len = audio_read_u32le(b + 14);
    frames = audio_read_u32le(b + 18);
    final_blocks = audio_read_u32le(b + 22);
    header_len = 32;
    if (flags & APE_FORMAT_PEAK) { if (fseek(f, 4, SEEK_CUR)) { fclose(f); return NULL; } header_len += 4; }
    if (flags & APE_FORMAT_SEEK_ELEMENTS) {
        if (fread(b, 1, 4, f) != 4) { fclose(f); return NULL; }
        uint32_t count = audio_read_u32le(b);
        if (count > UINT32_MAX / 4) { fclose(f); return NULL; }
        seek_len = count * 4;
        header_len += 4;
    } else {
        if (frames > UINT32_MAX / 4) { fclose(f); return NULL; }
        seek_len = frames * 4;
    }
    bps = (flags & APE_FORMAT_8_BIT) ? 8 : ((flags & APE_FORMAT_24_BIT) ? 24 : 16);
    if (version >= 3950) blocks_per_frame = 73728U * 4U;
    else if (version >= 3900 || (version >= 3800 && compression >= 4000)) blocks_per_frame = 73728U;
    else blocks_per_frame = 9216U;
    if (!(flags & APE_FORMAT_CREATE_WAV)) {
#if LONG_MAX < UINT32_MAX
        if (wav_header_len > LONG_MAX) { fclose(f); return NULL; }
#endif
        if (fseek(f, (long)wav_header_len, SEEK_CUR)) { fclose(f); return NULL; }
    }
    long tab = ftell(f);
    if (tab < 0) { fclose(f); return NULL; }
    table_pos = (uint64_t)tab;
    uint64_t first_pos = table_pos + seek_len;
    if (version < 3810) first_pos += frames;
    uint64_t end_pos = size - (wav_tail_len <= size ? wav_tail_len : size);
    if (!frames || frames > APE_MAX_FRAMES || !final_blocks || final_blocks > blocks_per_frame ||
        blocks_per_frame > APE_MAX_BLOCKS || channels < 1 || channels > 2 || !rate ||
        seek_len < (uint64_t)frames * 4 || first_pos >= end_pos || !seek_abs(f, table_pos)) {
        fclose(f); return NULL;
    }
    ape_demux_t *d = calloc(1, sizeof(*d));
    if (!d) { fclose(f); return NULL; }
    d->f = f; d->version = version; d->compression = compression; d->flags = flags;
    d->channels = channels; d->bps = bps; d->samplerate = rate;
    d->blocksperframe = blocks_per_frame; d->finalframeblocks = final_blocks; d->totalframes = frames;
    d->frames = calloc(frames, sizeof(*d->frames));
    uint8_t *table = malloc((size_t)frames * 4);
    if (!d->frames || !table || fread(table, 4, frames, f) != frames) {
        free(table); ape_demux_close(d); return NULL;
    }
    d->frames[0].pos = first_pos;
    for (uint32_t i = 1; i < frames; i++) {
        uint64_t pos = (uint64_t)audio_read_u32le(table + (size_t)i * 4);
        if (pos <= d->frames[i - 1].pos) { free(table); ape_demux_close(d); return NULL; }
        d->frames[i].pos = pos;
        d->frames[i - 1].size = d->frames[i - 1].pos < end_pos
            ? (pos < end_pos ? pos : end_pos) - d->frames[i - 1].pos : 0;
        d->frames[i].skip = (uint32_t)((pos - first_pos) & 3U);
    }
    free(table);
    d->frames[frames - 1].nblocks = final_blocks;
    uint64_t final_size = end_pos - d->frames[frames - 1].pos;
    final_size -= final_size & 3U;
    if (!final_size) final_size = (uint64_t)final_blocks * 8;
    d->frames[frames - 1].size = final_size;
    /* Word-align frame starts before applying the pre-3.81 bit table. */
    for (uint32_t i = 0; i < frames; i++) {
        ape_frame_t *fr = &d->frames[i];
        if (fr->pos >= end_pos) { fr->size = 0; continue; }
        if (fr->skip) {
            if (fr->skip > fr->pos) { ape_demux_close(d); return NULL; }
            fr->pos -= fr->skip;
            fr->size += fr->skip;
        }
        fr->size = (fr->size + 3U) & ~(uint64_t)3U;
        uint64_t fend;
        if (fr->size > APE_MAX_FRAME_BYTES || !add_u64(fr->pos, fr->size, &fend) || fend > size)
            fr->size = 0;
    }
    if (version < 3810) {
        uint64_t bit_at = table_pos + seek_len;
        if (!seek_abs(f, bit_at)) { ape_demux_close(d); return NULL; }
        for (uint32_t i = 0; i < frames; i++) {
            int bits = fgetc(f);
            if (bits == EOF) { ape_demux_close(d); return NULL; }
            if (i && bits) d->frames[i - 1].size += 4;
            d->frames[i].skip = (d->frames[i].skip << 3) | (uint32_t)bits;
        }
    }
    for (uint32_t i = 0; i < frames; i++) {
        ape_frame_t *fr = &d->frames[i];
        if (version >= 3900 && fr->skip > 3) { ape_demux_close(d); return NULL; }
        if (fr->size && fr->size < 4) {
            ape_demux_close(d); return NULL;
        }
    }
    d->total_samples = (uint64_t)blocks_per_frame * (frames - 1) + final_blocks;
    return d;
}

unsigned int ape_demux_get_channels(const ape_demux_t *d) { return d ? d->channels : 0; }
unsigned int ape_demux_get_sample_rate(const ape_demux_t *d) { return d ? d->samplerate : 0; }
unsigned int ape_demux_get_bits_per_sample(const ape_demux_t *d) { return d ? d->bps : 0; }
int ape_demux_get_fileversion(const ape_demux_t *d) { return d ? d->version : 0; }
int ape_demux_get_compression_level(const ape_demux_t *d) { return d ? d->compression : 0; }
uint64_t ape_demux_get_total_samples(const ape_demux_t *d) { return d ? d->total_samples : 0; }
uint32_t ape_demux_get_frame_count(const ape_demux_t *d) { return d ? d->totalframes : 0; }
uint32_t ape_demux_get_blocks_per_frame(const ape_demux_t *d) { return d ? d->blocksperframe : 0; }
uint32_t ape_demux_get_final_frame_blocks(const ape_demux_t *d) { return d ? d->finalframeblocks : 0; }
uint32_t ape_demux_get_frame_size(const ape_demux_t *d, uint32_t i) { return d && i < d->totalframes ? (uint32_t)d->frames[i].size : 0; }
uint32_t ape_demux_get_frame_skip(const ape_demux_t *d, uint32_t i) { return d && i < d->totalframes ? d->frames[i].skip : 0; }

bool ape_demux_read_frame(ape_demux_t *d, uint32_t i, uint8_t *buf, uint32_t cap, uint32_t *out) {
    if (!d || i >= d->totalframes || !buf || !out || d->frames[i].size > cap || d->frames[i].pos > LONG_MAX) return false;
    if (fseek(d->f, (long)d->frames[i].pos, SEEK_SET)) return false;
    size_t n = (size_t)d->frames[i].size;
    if (fread(buf, 1, n, d->f) != n) return false;
    *out = (uint32_t)n;
    return true;
}

bool ape_demux_read_frame_at(ape_demux_t *d, uint32_t i, uint32_t rel, uint8_t *buf,
                             uint32_t bytes, uint32_t *out_bytes) {
    if (!d || i >= d->totalframes || !buf || !out_bytes || rel > d->frames[i].size ||
        d->frames[i].pos > LONG_MAX || (uint64_t)rel > (uint64_t)LONG_MAX - d->frames[i].pos)
        return false;
    uint64_t remaining = d->frames[i].size - rel;
    if (bytes > remaining) bytes = (uint32_t)remaining;
    if (fseek(d->f, (long)(d->frames[i].pos + rel), SEEK_SET)) return false;
    *out_bytes = (uint32_t)fread(buf, 1, bytes, d->f);
    return *out_bytes > 0 || bytes == 0;
}

void ape_demux_close(ape_demux_t *d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    free(d->frames);
    free(d);
}
