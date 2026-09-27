#include "dsd_decoder.h"
#include "dsd_filter.h"
#include "audio_helpers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DSD_MAX_CHANNELS 8
#define DFF_BUFFER_SIZE 4096
/* DSF blocks are 4096 bytes per channel by specification; accept any sane
 * size rather than rejecting an unusual writer, but bound the allocation. */
#define DSF_MAX_BLOCK_SIZE (1u << 20)

typedef enum {
    DSD_CONTAINER_DSF,
    DSD_CONTAINER_DFF
} dsd_container_t;

struct dsd_decoder {
    FILE * f;
    dsd_container_t container;

    unsigned int channels;
    unsigned int dsd_sample_rate;
    unsigned int pcm_sample_rate;
    unsigned int bytes_per_frame; /* DSD bytes per channel per PCM frame */
    uint64_t total_pcm_frames;
    uint64_t current_pcm_frame;
    bool lsb_first; /* DSF "bits per sample" 1; DFF and DSF value 8 are MSB first */

    off_t data_start_offset;
    off_t data_end_offset; /* DSF: end of the declared data chunk */

    /* DSF: block-interleaved -- all of channel 0's block, then channel 1's,
     * and so on. One multi-channel block is buffered at a time. */
    uint32_t dsf_block_size;
    uint8_t * dsf_block_buffer;
    uint32_t dsf_block_pos;  /* byte position within each channel's block */
    uint32_t dsf_block_rows; /* rows present for every channel in the buffered block */

    /* DFF: byte-interleaved (ch0, ch1, ..., ch0, ...). The buffer holds a
     * whole number of channel rows. */
    uint8_t dff_buffer[DFF_BUFFER_SIZE];
    size_t dff_buffer_len;
    size_t dff_buffer_pos;

    const dsd_filter_design_t * filter_design; /* shared; see dsd_filter_design_acquire() */
    dsd_channel_state_t channel_states[DSD_MAX_CHANNELS];
};

/* Byte bit reversal, built at compile time so concurrent opens never see a
 * partial table. */
#define DSD_R2(n) n, n + 2 * 64, n + 1 * 64, n + 3 * 64
#define DSD_R4(n) DSD_R2(n), DSD_R2(n + 2 * 16), DSD_R2(n + 1 * 16), DSD_R2(n + 3 * 16)
#define DSD_R6(n) DSD_R4(n), DSD_R4(n + 2 * 4), DSD_R4(n + 1 * 4), DSD_R4(n + 3 * 4)
static const uint8_t bit_reverse[256] = { DSD_R6(0), DSD_R6(2), DSD_R6(1), DSD_R6(3) };

static void reset_channels(dsd_decoder_t * dec) {
    for (unsigned int ch = 0; ch < dec->channels; ch++) dsd_channel_reset(&dec->channel_states[ch]);
}

/* DSD rates are 64/128/256 times 44.1 kHz or 48 kHz. Output is twice the
 * base rate (88.2 or 96 kHz). DSD512 and above are rejected: the filter
 * cost doubles with each step and would not fit the audio thread. */
static bool finish_setup(dsd_decoder_t * dec, uint64_t total_dsd_samples_per_channel) {
    if (dec->channels == 0 || dec->channels > DSD_MAX_CHANNELS || dec->dsd_sample_rate == 0) return false;
    unsigned int base = dec->dsd_sample_rate % 44100 == 0 ? 44100 : dec->dsd_sample_rate % 48000 == 0 ? 48000 : 0;
    if (!base) return false;
    unsigned int multiple = dec->dsd_sample_rate / base;
    dec->filter_design = dsd_filter_design_acquire(multiple, base);
    if (!dec->filter_design) return false;

    dec->pcm_sample_rate = 2 * base;
    dec->bytes_per_frame = (unsigned int) dsd_filter_bytes_per_output(dec->filter_design);
    dec->total_pcm_frames = total_dsd_samples_per_channel / (8u * dec->bytes_per_frame);
    reset_channels(dec);
    return true;
}

/* Bytes present in the file from `start`, so a declared chunk longer than
 * the file (a truncated copy) gives its real length at open and every
 * cached duration is right from the first frame. */
static uint64_t bytes_present_from(FILE * f, off_t start) {
    struct stat st;
    if (start < 0 || fstat(fileno(f), &st) != 0 || st.st_size <= start) return 0;
    return (uint64_t) (st.st_size - start);
}

static void free_decoder(dsd_decoder_t * dec) {
    dsd_filter_design_release(dec->filter_design);
    free(dec->dsf_block_buffer);
    free(dec);
}

static dsd_decoder_t * open_dsf(FILE * f) {
    /* "DSD " chunk: magic(4) + chunkSize u64LE(8, =28) + totalFileSize u64LE(8) + metadataPointer u64LE(8).
     * The caller only peeked at the magic and rewound, so it's still unread here. */
    uint8_t dsd_chunk[28];
    if (fread(dsd_chunk, 1, sizeof(dsd_chunk), f) != sizeof(dsd_chunk)) return NULL;
    if (memcmp(dsd_chunk, "DSD ", 4) != 0 || audio_read_u64le(dsd_chunk + 4) != 28) return NULL;

    /* "fmt " chunk: magic(4) + chunkSize u64LE(8, =52) + 40 bytes of fields */
    uint8_t fmt_header[12];
    if (fread(fmt_header, 1, sizeof(fmt_header), f) != sizeof(fmt_header)) return NULL;
    uint64_t fmt_size = audio_read_u64le(fmt_header + 4);
    if (memcmp(fmt_header, "fmt ", 4) != 0 || fmt_size < 52 || fmt_size > 4096) return NULL;

    uint8_t fmt[40];
    if (fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) return NULL;
    if (fmt_size > 52 && fseeko(f, (off_t) (fmt_size - 52), SEEK_CUR) != 0) return NULL;

    uint32_t format_id = audio_read_u32le(fmt + 4);
    unsigned int channels = audio_read_u32le(fmt + 12);
    unsigned int sample_rate = audio_read_u32le(fmt + 16);
    uint32_t bits_per_sample = audio_read_u32le(fmt + 20);
    uint64_t sample_count = audio_read_u64le(fmt + 24);
    uint32_t block_size = audio_read_u32le(fmt + 32);
    /* Format ID 0 is raw DSD; bits per sample 1 is LSB first, 8 MSB first. */
    if (format_id != 0 || (bits_per_sample != 1 && bits_per_sample != 8) ||
        block_size == 0 || block_size > DSF_MAX_BLOCK_SIZE || channels == 0 || channels > DSD_MAX_CHANNELS)
        return NULL;

    /* "data" chunk: magic(4) + chunkSize u64LE(8, includes this header) + block-interleaved data */
    uint8_t data_header[12];
    if (fread(data_header, 1, sizeof(data_header), f) != sizeof(data_header)) return NULL;
    uint64_t data_chunk_size = audio_read_u64le(data_header + 4);
    if (memcmp(data_header, "data", 4) != 0 || data_chunk_size < 12) return NULL;
    /* Never trust the sample count past the data actually declared. Data
     * is whole multi-channel blocks; a short last block holds rows for every
     * channel only past the other channels' full segments. */
    uint64_t payload = data_chunk_size - 12, stride = (uint64_t) channels * block_size;
    uint64_t present = bytes_present_from(f, ftello(f));
    if (payload > present) payload = present;
    uint64_t tail = payload % stride, last_channel = (uint64_t) (channels - 1) * block_size;
    uint64_t tail_rows = tail > last_channel ? tail - last_channel : 0;
    if (tail_rows > block_size) tail_rows = block_size;
    uint64_t max_samples = ((payload / stride) * block_size + tail_rows) * 8;
    if (sample_count > max_samples) sample_count = max_samples;

    dsd_decoder_t * dec = calloc(1, sizeof(*dec));
    if (!dec) return NULL;
    dec->f = f;
    dec->container = DSD_CONTAINER_DSF;
    dec->channels = channels;
    dec->dsd_sample_rate = sample_rate;
    dec->lsb_first = bits_per_sample == 1;
    dec->dsf_block_size = block_size;
    dec->data_start_offset = ftello(f);
    dec->data_end_offset = dec->data_start_offset + (off_t) payload;
    dec->dsf_block_pos = block_size; /* forces a fresh block load on first read */
    dec->dsf_block_buffer = malloc((size_t) channels * block_size);
    if (dec->data_start_offset < 0 || !dec->dsf_block_buffer || !finish_setup(dec, sample_count)) {
        free_decoder(dec);
        return NULL;
    }
    return dec;
}

/* Reads a DFF chunk header (big-endian, 64-bit size -- unlike AIFF's 32-bit). */
static bool dff_read_chunk_header(FILE * f, char id_out[5], uint64_t * size_out) {
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) return false;
    memcpy(id_out, header, 4);
    id_out[4] = '\0';
    *size_out = audio_read_u64be(header + 4);
    return true;
}

/* Chunk sizes come from the file; reject any that would run past `end`. */
static bool dff_chunk_fits(off_t start, uint64_t size, off_t end) {
    return start >= 0 && size <= (uint64_t) (end - start);
}

static dsd_decoder_t * open_dff(FILE * f) {
    /* "FRM8" master chunk: magic(4) + size u64BE(8) + formType(4, "DSD ").
     * The caller only peeked at the magic and rewound, so it's still unread here. */
    char frm8_id[5];
    uint64_t frm8_size;
    if (!dff_read_chunk_header(f, frm8_id, &frm8_size)) return NULL;
    if (strcmp(frm8_id, "FRM8") != 0 || frm8_size < 4 || frm8_size > (uint64_t) INT64_MAX - 12) return NULL;
    off_t frm8_end = 12 + (off_t) frm8_size;

    uint8_t form_type[4];
    if (fread(form_type, 1, sizeof(form_type), f) != sizeof(form_type)) return NULL;
    if (memcmp(form_type, "DSD ", 4) != 0) return NULL;

    unsigned int channels = 0;
    unsigned int sample_rate = 0;
    bool compressed = false;
    off_t data_start_offset = 0;
    uint64_t data_size = 0;
    bool have_data = false;

    while (ftello(f) + 12 <= frm8_end) {
        char id[5];
        uint64_t size;
        if (!dff_read_chunk_header(f, id, &size)) break;
        off_t chunk_data_start = ftello(f);
        if (!dff_chunk_fits(chunk_data_start, size, frm8_end)) {
            /* Some writers leave the sound chunk size short of the real
             * data; accept a DSD chunk that runs to the end of the form. */
            if (strcmp(id, "DSD ") != 0 || chunk_data_start < 0 || chunk_data_start > frm8_end) break;
            size = (uint64_t) (frm8_end - chunk_data_start);
        }
        off_t chunk_end = chunk_data_start + (off_t) size;

        if (strcmp(id, "PROP") == 0) {
            uint8_t prop_type[4];
            if (size < 4 || fread(prop_type, 1, sizeof(prop_type), f) != sizeof(prop_type)) break;
            if (memcmp(prop_type, "SND ", 4) == 0) {
                while (ftello(f) + 12 <= chunk_end) {
                    char sub_id[5];
                    uint64_t sub_size;
                    if (!dff_read_chunk_header(f, sub_id, &sub_size)) break;
                    off_t sub_data_start = ftello(f);
                    if (!dff_chunk_fits(sub_data_start, sub_size, chunk_end)) break;

                    uint8_t buf[4];
                    /* DFF chunk IDs are always 4 bytes: "FS" padded with two spaces */
                    if (strcmp(sub_id, "FS  ") == 0 && sub_size >= 4) {
                        if (fread(buf, 1, 4, f) == 4) sample_rate = audio_read_u32be(buf);
                    } else if (strcmp(sub_id, "CHNL") == 0 && sub_size >= 2) {
                        if (fread(buf, 1, 2, f) == 2) channels = audio_read_u16be(buf);
                    } else if (strcmp(sub_id, "CMPR") == 0 && sub_size >= 4) {
                        /* "DST " is compressed DSD, which this decoder cannot read. */
                        if (fread(buf, 1, 4, f) == 4) compressed = memcmp(buf, "DSD ", 4) != 0;
                    }
                    off_t next = sub_data_start + (off_t) sub_size + (off_t) (sub_size & 1);
                    if (fseeko(f, next, SEEK_SET) != 0) break;
                }
            }
        } else if (strcmp(id, "DSD ") == 0) {
            data_start_offset = chunk_data_start;
            data_size = size;
            have_data = true;
        } else if (strcmp(id, "DST ") == 0) {
            compressed = true;
        }
        if (have_data && channels && sample_rate) break; /* sound data is conventionally last */
        off_t next = chunk_end + (off_t) (size & 1);
        if (next > frm8_end || fseeko(f, next, SEEK_SET) != 0) break;
    }

    if (compressed || !have_data || channels == 0 || channels > DSD_MAX_CHANNELS || sample_rate == 0) return NULL;

    dsd_decoder_t * dec = calloc(1, sizeof(*dec));
    if (!dec) return NULL;
    dec->f = f;
    dec->container = DSD_CONTAINER_DFF;
    dec->channels = channels;
    dec->dsd_sample_rate = sample_rate;
    dec->data_start_offset = data_start_offset;

    uint64_t present = bytes_present_from(f, data_start_offset);
    if (data_size > present) data_size = present;
    uint64_t total_samples_per_channel = (data_size / channels) * 8;
    if (!finish_setup(dec, total_samples_per_channel) || fseeko(f, dec->data_start_offset, SEEK_SET) != 0) {
        free_decoder(dec);
        return NULL;
    }
    return dec;
}

dsd_decoder_t * dsd_open_file(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    uint8_t magic[4];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) || fseeko(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    dsd_decoder_t * dec = NULL;
    if (memcmp(magic, "DSD ", 4) == 0) {
        dec = open_dsf(f);
    } else if (memcmp(magic, "FRM8", 4) == 0) {
        dec = open_dff(f);
    }

    if (!dec) {
        fclose(f);
        return NULL;
    }
    return dec;
}

unsigned int dsd_get_channels(const dsd_decoder_t * dec) {
    return dec->channels;
}

unsigned int dsd_get_source_sample_rate(const dsd_decoder_t * dec) {
    return dec->dsd_sample_rate;
}

unsigned int dsd_get_pcm_sample_rate(const dsd_decoder_t * dec) {
    return dec->pcm_sample_rate;
}

uint64_t dsd_get_total_pcm_frame_count(const dsd_decoder_t * dec) {
    return dec->total_pcm_frames;
}

/* Reads the next multi-channel DSF block. In a block cut short by the end
 * of the file only the rows every channel still has are playable. */
static void dsf_load_block(dsd_decoder_t * dec) {
    size_t block = dec->dsf_block_size;
    size_t to_read = (size_t) dec->channels * block;
    /* Stop at the declared data end: metadata (ID3) usually follows it. */
    off_t at = ftello(dec->f);
    off_t left = at >= 0 && at < dec->data_end_offset ? dec->data_end_offset - at : 0;
    if ((uint64_t) left < to_read) to_read = (size_t) left;
    size_t got = to_read ? fread(dec->dsf_block_buffer, 1, to_read, dec->f) : 0;
    size_t last_channel = (size_t) (dec->channels - 1) * block;
    size_t rows = got >= last_channel ? got - last_channel : 0;
    dec->dsf_block_rows = (uint32_t) (rows < block ? rows : block);
}

/* One byte per channel for the next byte time, in time order (MSB first). */
static bool next_byte_row(dsd_decoder_t * dec, uint8_t row[DSD_MAX_CHANNELS]) {
    if (dec->container == DSD_CONTAINER_DSF) {
        if (dec->dsf_block_pos >= dec->dsf_block_size) {
            dsf_load_block(dec);
            dec->dsf_block_pos = 0;
        }
        if (dec->dsf_block_pos >= dec->dsf_block_rows) return false;
        for (unsigned int ch = 0; ch < dec->channels; ch++)
            row[ch] = dec->dsf_block_buffer[ch * dec->dsf_block_size + dec->dsf_block_pos];
        dec->dsf_block_pos++;
    } else {
        if (dec->dff_buffer_len - dec->dff_buffer_pos < dec->channels) {
            size_t whole_rows = (sizeof(dec->dff_buffer) / dec->channels) * dec->channels;
            dec->dff_buffer_len = fread(dec->dff_buffer, 1, whole_rows, dec->f);
            dec->dff_buffer_pos = 0;
            if (dec->dff_buffer_len < dec->channels) return false;
        }
        memcpy(row, dec->dff_buffer + dec->dff_buffer_pos, dec->channels);
        dec->dff_buffer_pos += dec->channels;
    }
    if (dec->lsb_first)
        for (unsigned int ch = 0; ch < dec->channels; ch++) row[ch] = bit_reverse[row[ch]];
    return true;
}

/* Decodes up to frames_to_read frames into s16 or s32 (24-bit, right
 * justified, like the other wide decoders), or discards them when both
 * are NULL. */
static decoder_read_result_t decode_frames(dsd_decoder_t * dec, uint64_t frames_to_read, int16_t * out_s16,
                                           int32_t * out_s32) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    unsigned int channels = dec->channels;
    while (res.frames < frames_to_read && dec->current_pcm_frame < dec->total_pcm_frames) {
        float samples[DSD_MAX_CHANNELS] = {0};
        bool ok = true;
        for (unsigned int r = 0; r < dec->bytes_per_frame && ok; r++) {
            uint8_t row[DSD_MAX_CHANNELS];
            ok = next_byte_row(dec, row);
            for (unsigned int ch = 0; ok && ch < channels; ch++)
                (void) dsd_channel_feed_byte(&dec->channel_states[ch], dec->filter_design, row[ch], &samples[ch]);
        }
        if (!ok) {
            /* Data ended before the declared length (a truncated file):
             * finish the track here. Only a read error is fatal. */
            if (!ferror(dec->f)) dec->total_pcm_frames = dec->current_pcm_frame;
            break;
        }

        size_t base = (size_t) res.frames * channels;
        for (unsigned int ch = 0; ch < channels; ch++) {
            float v = samples[ch];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            if (out_s16) out_s16[base + ch] = (int16_t) lrintf(v * 32767.0f);
            if (out_s32) out_s32[base + ch] = (int32_t) lrintf(v * 8388607.0f);
        }
        res.frames++;
        dec->current_pcm_frame++;
    }
    if (res.frames == 0)
        res.status = dec->current_pcm_frame >= dec->total_pcm_frames ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
    return res;
}

decoder_read_result_t dsd_read_pcm_frames_s16(dsd_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    if (!dec || !dec->f || !buffer_out) return (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_FATAL_ERROR };
    return decode_frames(dec, frames_to_read, buffer_out, NULL);
}

decoder_read_result_t dsd_read_pcm_frames_s32(dsd_decoder_t * dec, uint64_t frames_to_read, int32_t * buffer_out) {
    if (!dec || !dec->f || !buffer_out) return (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_FATAL_ERROR };
    return decode_frames(dec, frames_to_read, NULL, buffer_out);
}

/* Positions the byte reader at a PCM frame boundary without touching the
 * filter state. */
static bool position_at_frame(dsd_decoder_t * dec, uint64_t frame_index) {
    uint64_t byte_index = frame_index * dec->bytes_per_frame; /* per channel */
    clearerr(dec->f); /* a past read error must not fail every later seek */
    if (dec->container == DSD_CONTAINER_DSF) {
        uint64_t block_index = byte_index / dec->dsf_block_size;
        off_t block_offset = dec->data_start_offset +
                             (off_t) (block_index * dec->channels * dec->dsf_block_size);
        if (fseeko(dec->f, block_offset, SEEK_SET) != 0) return false;
        dsf_load_block(dec);
        dec->dsf_block_pos = (uint32_t) (byte_index % dec->dsf_block_size);
    } else {
        off_t byte_offset = dec->data_start_offset + (off_t) (byte_index * dec->channels);
        if (fseeko(dec->f, byte_offset, SEEK_SET) != 0) return false;
        dec->dff_buffer_len = 0;
        dec->dff_buffer_pos = 0;
    }
    return true;
}

bool dsd_seek_to_pcm_frame(dsd_decoder_t * dec, uint64_t frame_index) {
    if (!dec || !dec->f) return false;
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;

    /* Start early enough to refill the filter history, then decode and drop
     * the lead-in, so output after a seek matches continuous playback
     * instead of ramping up from silence. */
    uint64_t warmup = (uint64_t) (dsd_filter_groups(dec->filter_design) + dec->bytes_per_frame - 1) /
                      dec->bytes_per_frame + 1;
    uint64_t start = frame_index > warmup ? frame_index - warmup : 0;
    if (!position_at_frame(dec, start)) return false;
    reset_channels(dec);
    dec->current_pcm_frame = start;
    if (frame_index > start) (void) decode_frames(dec, frame_index - start, NULL, NULL);
    /* Short only when the data ended first (later reads report EOF) or on a
     * read error, which fails the seek. */
    return dec->current_pcm_frame == frame_index || !ferror(dec->f);
}

void dsd_close(dsd_decoder_t * dec) {
    if (!dec) return;
    fclose(dec->f);
    free_decoder(dec);
}
