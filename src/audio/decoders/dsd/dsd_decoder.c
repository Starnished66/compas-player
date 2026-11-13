#include "dsd_decoder.h"
#include "dsd_filter.h"
#include "../../readers/sacd/sacd_reader.h"
#include "../../engine/audio_helpers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <wavpack.h>

#define DSD_MAX_CHANNELS 8
#define DSD_MAX_BYTES_PER_FRAME 16 /* DSD256 */
/* DoP: 16 DSD bits per channel per frame under an alternating marker. */
#define DOP_BYTES_PER_FRAME 2
#define DOP_MARKER_A 0x05
#define DOP_MARKER_B 0xFA
#define DFF_BUFFER_SIZE 4096
/* DSF blocks are 4096 bytes per channel by specification; accept any sane
 * size rather than rejecting an unusual writer, but bound the allocation. */
#define DSF_MAX_BLOCK_SIZE (1u << 20)
#define WAVPACK_DSD_BUFFER_FRAMES 2048

typedef enum {
    DSD_CONTAINER_DSF,
    DSD_CONTAINER_DFF,
    DSD_CONTAINER_WAVPACK,
    DSD_CONTAINER_SACD
} dsd_container_t;

struct dsd_decoder {
    FILE * f;
    WavpackContext * wavpack;
    sacd_reader_t * sacd;
    dsd_container_t container;

    unsigned int channels;
    float downmix[2][DSD_MAX_CHANNELS];
    bool downmix_stereo;
    unsigned int dsd_sample_rate;
    unsigned int pcm_sample_rate;
    unsigned int bytes_per_frame;     /* DSD bytes per channel per output frame, in the current mode */
    unsigned int pcm_bytes_per_frame; /* the same when converting to PCM */
    uint64_t total_bytes;             /* playable DSD bytes per channel */
    uint64_t total_pcm_frames;        /* output frames in the current mode */
    uint64_t current_pcm_frame;
    bool dop;
    uint8_t dop_marker;
    bool lsb_first; /* DSF "bits per sample" 1; DFF and DSF value 8 are MSB first */
    bool read_error;

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

    /* Native WavPack DSD stores each returned byte in the low byte of an
     * int32_t; data is already MSB first. */
    int32_t * wavpack_buffer;
    uint32_t wavpack_buffer_len;
    uint32_t wavpack_buffer_pos;
    int wavpack_errors;

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
    dec->pcm_bytes_per_frame = (unsigned int) dsd_filter_bytes_per_output(dec->filter_design);
    dec->bytes_per_frame = dec->pcm_bytes_per_frame;
    dec->total_bytes = total_dsd_samples_per_channel / 8u;
    dec->total_pcm_frames = dec->total_bytes / dec->bytes_per_frame;
    dec->dop_marker = DOP_MARKER_A;
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
    free(dec->wavpack_buffer);
    sacd_reader_close(dec->sacd);
    if (dec->wavpack) WavpackCloseFile(dec->wavpack);
    free(dec);
}

static dsd_decoder_t * open_wavpack(const char * path) {
    char error[128] = {0};
    WavpackContext * wpc = WavpackOpenFileInput(path, error, OPEN_WVC | OPEN_DSD_NATIVE, 0);
    if (!wpc) return NULL;
    int channels = WavpackGetNumChannels(wpc);
    uint32_t rate = WavpackGetNativeSampleRate(wpc);
    int64_t byte_frames = WavpackGetNumSamples64(wpc);
    if (!(WavpackGetQualifyMode(wpc) & QMODE_DSD_AUDIO) || channels < 1 ||
        channels > DSD_MAX_CHANNELS || !rate || byte_frames < 0 ||
        (uint64_t) byte_frames > UINT64_MAX / 8u) {
        WavpackCloseFile(wpc);
        return NULL;
    }
    dsd_decoder_t * dec = calloc(1, sizeof(*dec));
    if (!dec) {
        WavpackCloseFile(wpc);
        return NULL;
    }
    dec->container = DSD_CONTAINER_WAVPACK;
    dec->wavpack = wpc;
    dec->channels = (unsigned int) channels;
    dec->dsd_sample_rate = rate;
    dec->wavpack_buffer = malloc((size_t) WAVPACK_DSD_BUFFER_FRAMES * dec->channels * sizeof(*dec->wavpack_buffer));
    if (!dec->wavpack_buffer) {
        free_decoder(dec);
        return NULL;
    }
    dec->wavpack_errors = WavpackGetNumErrors(wpc);
    if (!finish_setup(dec, (uint64_t) byte_frames * 8u)) {
        free_decoder(dec);
        return NULL;
    }
    return dec;
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
    return start >= 0 && start <= end && size <= (uint64_t) (end - start);
}

/* DFF channel IDs define storage order, including nonstandard permutations. */
static bool dff_read_channels(FILE *f, uint64_t sub_size, unsigned *count,
                              float mix[2][DSD_MAX_CHANNELS]) {
    uint8_t buf[4];
    if (fread(buf, 1, 2, f) != 2) return false;
    unsigned channels = audio_read_u16be(buf);
    *count = channels;
    if (!channels || channels > DSD_MAX_CHANNELS || sub_size < 2u + 4u * channels) return false;
    unsigned roles = 0;
    memset(mix, 0, 2 * DSD_MAX_CHANNELS * sizeof(float));
    for (unsigned ch = 0; ch < channels; ch++) {
        if (fread(buf, 1, 4, f) != 4) return false;
        unsigned role = 0;
        if (!memcmp(buf, "MLFT", 4) || !memcmp(buf, "SLFT", 4)) { mix[0][ch] = 1; role = 1; }
        else if (!memcmp(buf, "MRGT", 4) || !memcmp(buf, "SRGT", 4)) { mix[1][ch] = 1; role = 2; }
        else if (!memcmp(buf, "C   ", 4)) { mix[0][ch] = mix[1][ch] = 0.70710678f; role = 4; }
        else if (!memcmp(buf, "LS  ", 4)) { mix[0][ch] = 0.70710678f; role = 8; }
        else if (!memcmp(buf, "RS  ", 4)) { mix[1][ch] = 0.70710678f; role = 16; }
        else if (!memcmp(buf, "LFE ", 4)) role = 32;
        if (!role || (roles & role)) {
            if (channels > 2) return false;
        }
        roles |= role;
    }
    return channels <= 2 || (channels == 5 && roles == 31) || (channels == 6 && roles == 63);
}

/* ITU Lo/Ro: omit effects-only LFE and reserve the sum of absolute gains. */
static void setup_downmix(dsd_decoder_t *dec, float mix[2][DSD_MAX_CHANNELS]) {
    if (dec->channels <= 2) return;
    dec->downmix_stereo = true;
    for (unsigned out = 0; out < 2; out++)
        for (unsigned ch = 0; ch < dec->channels; ch++)
            dec->downmix[out][ch] = mix[out][ch] / 2.41421356f;
}

static dsd_decoder_t * open_dff(FILE * f) {
    /* "FRM8" master chunk: magic(4) + size u64BE(8) + formType(4, "DSD ").
     * The caller only peeked at the magic and rewound, so it's still unread here. */
    char frm8_id[5];
    uint64_t frm8_size;
    if (!dff_read_chunk_header(f, frm8_id, &frm8_size)) return NULL;
    if (strcmp(frm8_id, "FRM8") != 0 || frm8_size < 4 || frm8_size > (uint64_t) INT64_MAX - 12) return NULL;
    off_t frm8_end = 12 + (off_t) frm8_size;
    uint64_t available = bytes_present_from(f, 0);
    if ((uint64_t)frm8_end > available) frm8_end = (off_t)available;

    uint8_t form_type[4];
    if (fread(form_type, 1, sizeof(form_type), f) != sizeof(form_type)) return NULL;
    if (memcmp(form_type, "DSD ", 4) != 0) return NULL;

    unsigned int channels = 0;
    unsigned int sample_rate = 0;
    bool compressed = false, unknown_compression = false;
    float mix[2][DSD_MAX_CHANNELS] = {{0}};
    off_t data_start_offset = 0;
    uint64_t data_size = 0, index_start = 0, index_size = 0;
    bool have_data = false, dst_data = false;

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
                    if (!dff_chunk_fits(sub_data_start, sub_size, chunk_end)) return NULL;

                    uint8_t buf[4];
                    /* DFF chunk IDs are always 4 bytes: "FS" padded with two spaces */
                    if (strcmp(sub_id, "FS  ") == 0 && sub_size >= 4) {
                        if (fread(buf, 1, 4, f) == 4) sample_rate = audio_read_u32be(buf);
                    } else if (strcmp(sub_id, "CHNL") == 0 && sub_size >= 2) {
                        if (!dff_read_channels(f, sub_size, &channels, mix)) return NULL;
                    } else if (strcmp(sub_id, "CMPR") == 0 && sub_size >= 4) {
                        if (fread(buf, 1, 4, f) != 4) return NULL;
                        compressed = memcmp(buf, "DST ", 4) == 0;
                        unknown_compression = !compressed && memcmp(buf, "DSD ", 4) != 0;
                    }
                    if ((sub_size & 1) > (uint64_t)(chunk_end - sub_data_start) - sub_size) return NULL;
                    off_t next = sub_data_start + (off_t) sub_size + (off_t) (sub_size & 1);
                    if (next > chunk_end || fseeko(f, next, SEEK_SET) != 0) return NULL;
                }
            }
        } else if (strcmp(id, "DSD ") == 0) {
            data_start_offset = chunk_data_start;
            data_size = size;
            have_data = true;
        } else if (strcmp(id, "DST ") == 0) {
            dst_data = true;
            data_start_offset = chunk_data_start;
            data_size = size;
            have_data = true;
        }
        if (strcmp(id, "DSTI") == 0) {
            if (index_start) return NULL;
            index_start = (uint64_t)chunk_data_start;
            index_size = size;
        }
        if (have_data && channels && sample_rate && !compressed) break;
        if ((size & 1) > (uint64_t)(frm8_end - chunk_end)) break;
        off_t next = chunk_end + (off_t) (size & 1);
        if (next > frm8_end || fseeko(f, next, SEEK_SET) != 0) break;
    }

    if (compressed != dst_data || unknown_compression || !have_data || channels == 0 || channels > DSD_MAX_CHANNELS || sample_rate == 0) return NULL;

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
    setup_downmix(dec, mix);
    if (compressed && sample_rate == 2822400)
        dec->sacd = sacd_reader_open_dff(f, (uint64_t)data_start_offset, data_size, channels, index_start, index_size, &total_samples_per_channel);
    if ((compressed && !dec->sacd) || !finish_setup(dec, total_samples_per_channel) ||
        (!dec->sacd && fseeko(f, dec->data_start_offset, SEEK_SET) != 0)) {
        free_decoder(dec);
        return NULL;
    }
    return dec;
}

static dsd_decoder_t * open_sacd(FILE *f) {
    dsd_decoder_t *dec = calloc(1, sizeof(*dec));
    if (!dec) return NULL;
    uint64_t samples;
    dec->f = f;
    dec->container = DSD_CONTAINER_SACD;
    dec->dsd_sample_rate = 2822400;
    dec->sacd = sacd_reader_open(f, &samples);
    if (dec->sacd) dec->channels = sacd_reader_channels(dec->sacd);
    if (dec->channels > 2) {
        float mix[2][DSD_MAX_CHANNELS] = {{0}};
        mix[0][0] = mix[1][1] = 1;
        mix[0][2] = mix[1][2] = 0.70710678f;
        unsigned surround = dec->channels == 6 ? 4 : 3;
        mix[0][surround] = mix[1][surround + 1] = 0.70710678f;
        setup_downmix(dec, mix);
    }
    if (!dec->sacd || !finish_setup(dec, samples)) {
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
    } else if (memcmp(magic, "wvpk", 4) == 0) {
        fclose(f);
        return open_wavpack(path);
    } else {
        dec = open_sacd(f);
    }

    if (!dec) {
        fclose(f);
        return NULL;
    }
    return dec;
}

unsigned int dsd_get_channels(const dsd_decoder_t * dec) {
    return dec->downmix_stereo ? 2 : dec->channels;
}

unsigned int dsd_get_dst_channels(const dsd_decoder_t * dec) {
    return dec->sacd && sacd_reader_is_dst(dec->sacd) ? dec->channels : 0;
}

unsigned int dsd_get_source_sample_rate(const dsd_decoder_t * dec) {
    return dec->dsd_sample_rate;
}

unsigned int dsd_get_pcm_sample_rate(const dsd_decoder_t * dec) {
    return dec->dop ? dsd_get_dop_sample_rate(dec) : dec->pcm_sample_rate;
}

unsigned int dsd_get_dop_sample_rate(const dsd_decoder_t * dec) {
    return dec->downmix_stereo ? 0 : dec->dsd_sample_rate / (8u * DOP_BYTES_PER_FRAME);
}

bool dsd_is_dop(const dsd_decoder_t * dec) {
    return dec->dop;
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
    } else if (dec->container == DSD_CONTAINER_DFF) {
        if (dec->dff_buffer_len - dec->dff_buffer_pos < dec->channels) {
            size_t whole_rows = (sizeof(dec->dff_buffer) / dec->channels) * dec->channels;
            dec->dff_buffer_len = fread(dec->dff_buffer, 1, whole_rows, dec->f);
            dec->dff_buffer_pos = 0;
            if (dec->dff_buffer_len < dec->channels) return false;
        }
        memcpy(row, dec->dff_buffer + dec->dff_buffer_pos, dec->channels);
        dec->dff_buffer_pos += dec->channels;
    } else {
        if (dec->wavpack_buffer_pos >= dec->wavpack_buffer_len) {
            if (dec->read_error) return false;
            uint32_t got = WavpackUnpackSamples(dec->wavpack, dec->wavpack_buffer,
                                                WAVPACK_DSD_BUFFER_FRAMES);
            if (WavpackGetNumErrors(dec->wavpack) != dec->wavpack_errors) {
                dec->read_error = true;
                dec->wavpack_buffer_len = 0;
                return false;
            }
            dec->wavpack_buffer_len = got;
            dec->wavpack_buffer_pos = 0;
            int64_t sample_index = WavpackGetSampleIndex64(dec->wavpack);
            if (got < WAVPACK_DSD_BUFFER_FRAMES && sample_index >= 0 &&
                sample_index < WavpackGetNumSamples64(dec->wavpack))
                dec->read_error = true;
            if (!got) return false;
        }
        const int32_t * samples = dec->wavpack_buffer + (size_t) dec->wavpack_buffer_pos * dec->channels;
        for (unsigned int ch = 0; ch < dec->channels; ch++) row[ch] = (uint8_t) samples[ch];
        dec->wavpack_buffer_pos++;
    }
    if (dec->lsb_first)
        for (unsigned int ch = 0; ch < dec->channels; ch++) row[ch] = bit_reverse[row[ch]];
    return true;
}

/* One PCM frame's bytes per channel, oldest first (MSB first). Whole
 * frames inside the buffered DSF block or DFF buffer are copied directly;
 * a frame that straddles a buffer edge goes byte row by byte row. */
static bool read_frame_bytes(dsd_decoder_t * dec, uint8_t out[DSD_MAX_CHANNELS][DSD_MAX_BYTES_PER_FRAME]) {
    unsigned int channels = dec->channels, count = dec->bytes_per_frame;
    if (dec->sacd) {
        uint8_t *planes[DSD_MAX_CHANNELS];
        for (unsigned ch = 0; ch < channels; ch++) planes[ch] = out[ch];
        return sacd_reader_bytes(dec->sacd, planes, count);
    }
    if (dec->container == DSD_CONTAINER_DSF) {
        if (dec->dsf_block_pos >= dec->dsf_block_size) {
            dsf_load_block(dec);
            dec->dsf_block_pos = 0;
        }
        if (dec->dsf_block_pos + count <= dec->dsf_block_rows) {
            for (unsigned int ch = 0; ch < channels; ch++) {
                const uint8_t * src = dec->dsf_block_buffer + (size_t) ch * dec->dsf_block_size + dec->dsf_block_pos;
                if (dec->lsb_first)
                    for (unsigned int i = 0; i < count; i++) out[ch][i] = bit_reverse[src[i]];
                else
                    memcpy(out[ch], src, count);
            }
            dec->dsf_block_pos += count;
            return true;
        }
    } else if (dec->container == DSD_CONTAINER_DFF && dec->dff_buffer_len - dec->dff_buffer_pos >= (size_t) count * channels) {
        const uint8_t * src = dec->dff_buffer + dec->dff_buffer_pos;
        for (unsigned int i = 0; i < count; i++, src += channels)
            for (unsigned int ch = 0; ch < channels; ch++) out[ch][i] = src[ch];
        dec->dff_buffer_pos += (size_t) count * channels;
        return true;
    }
    for (unsigned int i = 0; i < count; i++) {
        uint8_t row[DSD_MAX_CHANNELS];
        if (!next_byte_row(dec, row)) return false;
        for (unsigned int ch = 0; ch < channels; ch++) out[ch][i] = row[ch];
    }
    return true;
}

/* The data ended before its declared length (a truncated file): the track
 * ends here in either mode. Only a read error is fatal. */
static void end_at_current_frame(dsd_decoder_t * dec) {
    /* Corrupt compressed data is fatal. A clean SACD area end can be
     * earlier than the TOC duration, and follows the normal EOF path. */
    if (dec->container == DSD_CONTAINER_WAVPACK ||
        (dec->sacd && !sacd_reader_eof(dec->sacd))) {
        dec->read_error = true;
        return;
    }
    if (ferror(dec->f)) return;
    dec->total_pcm_frames = dec->current_pcm_frame;
    dec->total_bytes = dec->current_pcm_frame * dec->bytes_per_frame;
}

static decoder_read_result_t finish_read(const dsd_decoder_t * dec, decoder_read_result_t res) {
    if (res.frames == 0)
        res.status = dec->current_pcm_frame >= dec->total_pcm_frames ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
    return res;
}

/* DoP frames: per channel, the marker above two DSD bytes (earliest first),
 * as a sign-extended 24-bit sample right-justified in int32 for S24_LE. */
static decoder_read_result_t decode_dop(dsd_decoder_t * dec, uint64_t frames_to_read, int32_t * out) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    unsigned int channels = dec->channels;
    while (res.frames < frames_to_read && dec->current_pcm_frame < dec->total_pcm_frames) {
        uint8_t frame_bytes[DSD_MAX_CHANNELS][DSD_MAX_BYTES_PER_FRAME];
        if (!read_frame_bytes(dec, frame_bytes)) {
            end_at_current_frame(dec);
            break;
        }
        uint32_t marker = dec->dop_marker;
        dec->dop_marker = marker == DOP_MARKER_A ? DOP_MARKER_B : DOP_MARKER_A;
        size_t base = (size_t) res.frames * channels;
        for (unsigned int ch = 0; ch < channels; ch++) {
            uint32_t word = (marker << 16) | ((uint32_t) frame_bytes[ch][0] << 8) | frame_bytes[ch][1];
            out[base + ch] = (int32_t) (word << 8) >> 8;
        }
        res.frames++;
        dec->current_pcm_frame++;
    }
    return finish_read(dec, res);
}

/* Decodes up to frames_to_read frames into s16 or s32 (24-bit, right
 * justified, like the other wide decoders), or discards them when both
 * are NULL. */
static decoder_read_result_t decode_frames(dsd_decoder_t * dec, uint64_t frames_to_read, int16_t * out_s16,
                                           int32_t * out_s32) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    unsigned int channels = dec->channels;
    while (res.frames < frames_to_read && dec->current_pcm_frame < dec->total_pcm_frames) {
        uint8_t frame_bytes[DSD_MAX_CHANNELS][DSD_MAX_BYTES_PER_FRAME];
        if (!read_frame_bytes(dec, frame_bytes)) {
            end_at_current_frame(dec);
            break;
        }

        size_t base = (size_t) res.frames * dsd_get_channels(dec);
        float mixed[2] = {0};
        for (unsigned int ch = 0; ch < channels; ch++) {
            if (dec->downmix_stereo && dec->downmix[0][ch] == 0 && dec->downmix[1][ch] == 0) continue;
            float v = dsd_channel_frame(&dec->channel_states[ch], dec->filter_design, frame_bytes[ch]);
            if (dec->downmix_stereo) {
                mixed[0] += v * dec->downmix[0][ch];
                mixed[1] += v * dec->downmix[1][ch];
                continue;
            }
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            if (out_s16) out_s16[base + ch] = (int16_t) lrintf(v * 32767.0f);
            if (out_s32) out_s32[base + ch] = (int32_t) lrintf(v * 8388607.0f);
        }
        if (dec->downmix_stereo) {
            for (unsigned ch = 0; ch < 2; ch++) {
                float v = fmaxf(-1.0f, fminf(1.0f, mixed[ch]));
                if (out_s16) out_s16[base + ch] = (int16_t)lrintf(v * 32767.0f);
                if (out_s32) out_s32[base + ch] = (int32_t)lrintf(v * 8388607.0f);
            }
        }
        res.frames++;
        dec->current_pcm_frame++;
    }
    return finish_read(dec, res);
}

decoder_read_result_t dsd_read_pcm_frames_s16(dsd_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    /* DoP words carry DSD bits; narrowing them to 16 bits would destroy them. */
    if (!dec || (!dec->f && !dec->wavpack) || !buffer_out || dec->dop)
        return (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_FATAL_ERROR };
    return decode_frames(dec, frames_to_read, buffer_out, NULL);
}

decoder_read_result_t dsd_read_pcm_frames_s32(dsd_decoder_t * dec, uint64_t frames_to_read, int32_t * buffer_out) {
    if (!dec || (!dec->f && !dec->wavpack) || !buffer_out) return (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_FATAL_ERROR };
    if (dec->dop) return decode_dop(dec, frames_to_read, buffer_out);
    return decode_frames(dec, frames_to_read, NULL, buffer_out);
}

/* Positions the byte reader at a PCM frame boundary without touching the
 * filter state. */
static bool position_at_frame(dsd_decoder_t * dec, uint64_t frame_index) {
    uint64_t byte_index = frame_index * dec->bytes_per_frame; /* per channel */
    if (dec->sacd)
        return sacd_reader_seek(dec->sacd, byte_index);
    if (dec->container == DSD_CONTAINER_WAVPACK) {
        /* libwavpack treats a failed seek as fatal. Once decoding has
         * failed, the caller must close this context rather than reuse it. */
        if (dec->read_error || byte_index > INT64_MAX) return false;
        dec->wavpack_buffer_len = dec->wavpack_buffer_pos = 0;
        if (byte_index == (uint64_t) WavpackGetNumSamples64(dec->wavpack)) return true;
        int errors_before_seek = WavpackGetNumErrors(dec->wavpack);
        if (!WavpackSeekSample64(dec->wavpack, (int64_t) byte_index)) {
            dec->read_error = true;
            return false;
        }
        int errors_after_seek = WavpackGetNumErrors(dec->wavpack);
        if (errors_after_seek != errors_before_seek) {
            dec->read_error = true;
            return false;
        }
        return true;
    }
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
    if (!dec || (!dec->f && !dec->wavpack)) return false;
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;
    if (dec->dop) {
        /* No filter to refill: DoP frames are the raw bits. */
        if (!position_at_frame(dec, frame_index)) return false;
        dec->current_pcm_frame = frame_index;
        return true;
    }

    /* Start early enough to refill the filter history, then decode and drop
     * the lead-in, so output after a seek matches continuous playback
     * instead of ramping up from silence. */
    uint64_t warmup = (uint64_t) dsd_filter_warmup_outputs(dec->filter_design);
    uint64_t start = frame_index > warmup ? frame_index - warmup : 0;
    if (!position_at_frame(dec, start)) return false;
    reset_channels(dec);
    dec->current_pcm_frame = start;
    if (frame_index > start) (void) decode_frames(dec, frame_index - start, NULL, NULL);
    /* Short only when the data ended first (later reads report EOF) or on a
     * read error, which fails the seek. */
    return dec->current_pcm_frame == frame_index || (dec->f && !ferror(dec->f) && !dec->read_error);
}

bool dsd_set_dop(dsd_decoder_t * dec, bool dop, uint64_t frame_index, uint64_t * out_frame_index) {
    if (!dec || (!dec->f && !dec->wavpack)) return false;
    if (dop && !dsd_get_dop_sample_rate(dec)) return false;
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;
    uint64_t byte_index = frame_index * dec->bytes_per_frame;
    dec->dop = dop;
    dec->bytes_per_frame = dop ? DOP_BYTES_PER_FRAME : dec->pcm_bytes_per_frame;
    dec->total_pcm_frames = dec->total_bytes / dec->bytes_per_frame;
    uint64_t target = byte_index / dec->bytes_per_frame;
    if (target > dec->total_pcm_frames) target = dec->total_pcm_frames;
    if (out_frame_index) *out_frame_index = target;
    return dsd_seek_to_pcm_frame(dec, target);
}

void dsd_continue_dop_markers(dsd_decoder_t * next, const dsd_decoder_t * prev) {
    if (next && prev) next->dop_marker = prev->dop_marker;
}

void dsd_close(dsd_decoder_t * dec) {
    if (!dec) return;
    if (dec->f) fclose(dec->f);
    free_decoder(dec);
}
