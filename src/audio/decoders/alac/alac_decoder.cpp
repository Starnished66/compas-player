#include "alac_decoder.h"

extern "C" {
#include "../../demux/mp4/mp4_demux.h"
#include "../../demux/caf/caf_demux.h"
}

#include "ALACDecoder.h"
#include "ALACBitUtilities.h"

#include <cstdlib>
#include <cstring>
#include <new>

/* Where compressed ALAC packets come from: exactly one of the two is set. */
struct alac_source {
    mp4_demux_t * mp4;
    caf_demux_t * caf;
};

struct alac_decoder {
    alac_source src;
    ALACDecoder * decoder;

    unsigned int channels;
    unsigned int sample_rate;
    unsigned int bit_depth;
    uint32_t frame_size; /* PCM frames per demuxed sample, e.g. 4096 */
    uint64_t total_pcm_frames;
    uint64_t pcm_position;

    uint32_t current_sample_index;
    unsigned int consecutive_errors;

    uint8_t * compressed_buf;
    uint32_t compressed_buf_capacity;

    uint8_t * raw_output_buf; /* native bit-depth output from ALACDecoder::Decode() */

    int16_t * carry_buffer; /* converted to S16, ready to deliver */
    int32_t * carry_buffer_s32; /* converted to S32/S24_LE layout, ready to deliver */
    uint32_t carry_frames;
    uint32_t carry_read_pos;
};

#define ALAC_MAX_CONSECUTIVE_ERRORS 5

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool validate_alac_cookie(const uint8_t *cookie, uint32_t size,
                                 uint32_t *frame_length, uint32_t *sample_rate,
                                 uint8_t *channels, uint8_t *bit_depth) {
    if (!cookie || size < 8) return false;
    if (memcmp(cookie + 4, "frma", 4) == 0) {
        if (size < 12) return false;
        cookie += 12;
        size -= 12;
    }
    if (size >= 8 && memcmp(cookie + 4, "alac", 4) == 0) {
        if (size < 12) return false;
        cookie += 12;
        size -= 12;
    }
    if (size < 24) return false;
    *frame_length = read_be32(cookie);
    *bit_depth = cookie[5];
    *channels = cookie[9];
    *sample_rate = read_be32(cookie + 20);
    return *frame_length > 0 && *frame_length <= 65536 &&
           *channels > 0 && *channels <= 8 &&
           (*bit_depth == 16 || *bit_depth == 20 || *bit_depth == 24 || *bit_depth == 32) &&
           *sample_rate >= 8000 && *sample_rate <= 384000;
}

static void src_close(alac_source * src) {
    mp4_demux_close(src->mp4);
    caf_demux_close(src->caf);
    *src = alac_source();
}

/* Opens an ALAC stream from a CAF or MP4 file. */
static bool src_open(const char * path, alac_source * src) {
    char fourcc[5];
    *src = alac_source();
    src->caf = caf_demux_open(path);
    if (src->caf) {
        caf_demux_get_codec_fourcc(src->caf, fourcc);
    } else {
        src->mp4 = mp4_demux_open(path);
        if (!src->mp4) return false;
        mp4_demux_get_codec_fourcc(src->mp4, fourcc);
    }
    if (strcmp(fourcc, "alac") == 0) return true;
    src_close(src);
    return false;
}

static const uint8_t * src_config(const alac_source * src, uint32_t * size) {
    return src->caf ? caf_demux_get_codec_config(src->caf, size)
                    : mp4_demux_get_codec_config(src->mp4, size);
}

static uint32_t src_count(const alac_source * src) {
    return src->caf ? caf_demux_get_packet_count(src->caf) : mp4_demux_get_sample_count(src->mp4);
}

static uint32_t src_frames_per_packet(const alac_source * src) {
    return src->caf ? caf_demux_get_frames_per_packet(src->caf) : mp4_demux_get_frames_per_sample(src->mp4);
}

static bool src_read(alac_source * src, uint32_t index, uint8_t * buf, uint32_t cap, uint32_t * size) {
    return src->caf ? caf_demux_read_packet(src->caf, index, buf, cap, size)
                    : mp4_demux_read_sample(src->mp4, index, buf, cap, size);
}

/* Playable frames after the container's own priming/padding removal. */
static uint64_t src_total_frames(const alac_source * src, uint32_t sample_rate) {
    if (src->caf) return caf_demux_get_valid_frames(src->caf);
    mp4_demux_set_pcm_sample_rate(src->mp4, sample_rate);
    return mp4_demux_get_total_pcm_frame_count(src->mp4);
}

static uint64_t src_priming(const alac_source * src) {
    if (src->caf) return caf_demux_get_priming_frames(src->caf);
    uint64_t priming = 0, playable = 0;
    return mp4_demux_get_gapless_trim(src->mp4, &priming, &playable) ? priming : 0;
}

/* Packet holding a playable frame, and how many of its frames precede it. */
static bool src_map_frame(const alac_source * src, uint64_t frame, uint32_t * packet, uint64_t * discard) {
    if (!src->caf) return mp4_demux_map_pcm_frame(src->mp4, frame, packet, discard);
    uint64_t abs_frame = frame + caf_demux_get_priming_frames(src->caf);
    uint32_t fpp = caf_demux_get_frames_per_packet(src->caf);
    if (abs_frame / fpp > UINT32_MAX) return false;
    *packet = (uint32_t) (abs_frame / fpp);
    *discard = abs_frame % fpp;
    return true;
}

/* Native ALAC output as 16-bit, and as 24-bit right-justified when wanted.
 * 20-bit comes out left-justified in 3 bytes, so it reads like 24-bit. */
static void convert_output(alac_decoder_t * dec, uint32_t frames) {
    size_t count = (size_t) frames * dec->channels;
    const uint8_t * raw = dec->raw_output_buf;
    for (size_t i = 0; i < count; i++) {
        int32_t v16, v24;
        if (dec->bit_depth == 16) {
            int16_t s;
            memcpy(&s, raw + i * 2, sizeof(s));
            v16 = v24 = s;
        } else if (dec->bit_depth == 32) {
            int32_t w;
            memcpy(&w, raw + i * 4, sizeof(w));
            v16 = w >> 16;
            v24 = w >> 8;
        } else { /* packed 3-byte little-endian samples */
            v24 = raw[i * 3] | (raw[i * 3 + 1] << 8) | ((int8_t) raw[i * 3 + 2] * 65536);
            v16 = v24 >> 8;
        }
        dec->carry_buffer[i] = (int16_t) v16;
        if (dec->carry_buffer_s32) dec->carry_buffer_s32[i] = v24;
    }
}

static bool decode_next_sample_ex(alac_decoder_t * dec, decoder_read_status_t * out_status) {
    if (dec->current_sample_index >= src_count(&dec->src)) {
        if (out_status) *out_status = DECODER_READ_EOF;
        return false;
    }

    uint32_t compressed_size;
    if (!src_read(&dec->src, dec->current_sample_index, dec->compressed_buf,
                                dec->compressed_buf_capacity, &compressed_size)) {
        dec->consecutive_errors++;
        dec->current_sample_index++;
        if (dec->consecutive_errors >= ALAC_MAX_CONSECUTIVE_ERRORS) {
            if (out_status) *out_status = DECODER_READ_FATAL_ERROR;
            return false;
        }
        if (out_status) *out_status = DECODER_READ_RECOVERABLE_ERROR;
        return false;
    }

    BitBuffer bits;
    BitBufferInit(&bits, dec->compressed_buf, compressed_size);

    uint32_t out_frames = 0;
    int32_t status = dec->decoder->Decode(&bits, dec->raw_output_buf, dec->frame_size, dec->channels, &out_frames);
    if (status != 0) {
        dec->consecutive_errors++;
        dec->current_sample_index++;
        if (dec->consecutive_errors >= ALAC_MAX_CONSECUTIVE_ERRORS) {
            if (out_status) *out_status = DECODER_READ_FATAL_ERROR;
            return false;
        }
        if (out_status) *out_status = DECODER_READ_RECOVERABLE_ERROR;
        return false;
    }

    convert_output(dec, out_frames);

    dec->consecutive_errors = 0;
    dec->carry_frames = out_frames;
    dec->carry_read_pos = 0;
    dec->current_sample_index++;
    if (out_status) *out_status = DECODER_READ_OK;
    return true;
}

static bool decode_next_sample(alac_decoder_t * dec) {
    return decode_next_sample_ex(dec, NULL);
}

/* Builds the ALAC decoder from the stream's cookie, accepting 16/20/24/32-bit. */
static ALACDecoder * create_codec(const alac_source * src) {
    uint32_t config_size;
    const uint8_t * config = src_config(src, &config_size);
    uint32_t frame_length, sample_rate;
    uint8_t channels, bit_depth;
    if (!config || !validate_alac_cookie(config, config_size, &frame_length,
                                         &sample_rate, &channels, &bit_depth))
        return NULL;
    ALACDecoder * decoder = new (std::nothrow) ALACDecoder();
    if (!decoder) return NULL;
    /* Init() only reads from this buffer (parses header fields), never writes it,
     * despite the non-const signature. */
    if (decoder->Init((void *) config, config_size) != 0 ||
        decoder->mConfig.frameLength != frame_length ||
        decoder->mConfig.sampleRate != sample_rate ||
        decoder->mConfig.numChannels != channels ||
        decoder->mConfig.bitDepth != bit_depth) {
        delete decoder;
        return NULL;
    }
    return decoder;
}

/* Drops the encoder priming frames from the start of the first packet(s). */
static bool skip_priming(alac_decoder_t * dec) {
    uint64_t trim = src_priming(&dec->src);
    while (trim > 0) {
        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        if (trim < available) {
            dec->carry_read_pos += (uint32_t) trim;
            break;
        }
        trim -= available;
        dec->carry_read_pos = dec->carry_frames;
        if (trim > 0 && !decode_next_sample(dec)) return false;
    }
    return true;
}

static bool alloc_buffers(alac_decoder_t * dec) {
    /* Generous: worst case is uncompressed-ish escape frames, channels *
     * bytes-per-sample-at-full-depth * frame_size, plus header slack. */
    dec->compressed_buf_capacity = dec->frame_size * dec->channels * 4 + 4096;
    dec->compressed_buf = (uint8_t *) malloc(dec->compressed_buf_capacity);

    size_t bytes_per_sample = dec->bit_depth == 16 ? 2 : dec->bit_depth == 32 ? 4 : 3;
    dec->raw_output_buf = (uint8_t *) malloc((size_t) dec->frame_size * dec->channels * bytes_per_sample);
    dec->carry_buffer = (int16_t *) malloc((size_t) dec->frame_size * dec->channels * sizeof(int16_t));
    return dec->compressed_buf && dec->raw_output_buf && dec->carry_buffer;
}

extern "C" alac_decoder_t * alac_open_file(const char * path) {
    alac_source src;
    if (!src_open(path, &src)) return NULL;
    ALACDecoder * decoder = create_codec(&src);
    alac_decoder_t * dec = decoder ? (alac_decoder_t *) calloc(1, sizeof(alac_decoder_t)) : NULL;
    if (!dec) {
        delete decoder;
        src_close(&src);
        return NULL;
    }
    dec->src = src;
    dec->decoder = decoder;
    dec->channels = decoder->mConfig.numChannels;
    dec->sample_rate = decoder->mConfig.sampleRate;
    dec->bit_depth = decoder->mConfig.bitDepth;
    /* The container's timescale can differ from sample_rate; the cookie sizes
     * the output buffers. */
    dec->frame_size = decoder->mConfig.frameLength;
    if (dec->frame_size == 0) dec->frame_size = src_frames_per_packet(&src);

    /* Exact total (not packet count * frame_size): the last packet usually
     * holds fewer frames than a full one. */
    dec->total_pcm_frames = src_total_frames(&src, dec->sample_rate);

    if (!alloc_buffers(dec) || !decode_next_sample(dec) || !skip_priming(dec)) {
        alac_close(dec);
        return NULL;
    }
    return dec;
}

unsigned int alac_get_channels(const alac_decoder_t * dec) {
    return dec->channels;
}

unsigned int alac_get_sample_rate(const alac_decoder_t * dec) {
    return dec->sample_rate;
}

unsigned int alac_get_bit_depth(const alac_decoder_t * dec) {
    return dec->bit_depth;
}

uint64_t alac_get_total_pcm_frame_count(const alac_decoder_t * dec) {
    return dec->total_pcm_frames;
}

decoder_read_result_t alac_read_pcm_frames_s16(alac_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !buffer_out) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    while (res.frames < frames_to_read) {
        if (dec->pcm_position >= dec->total_pcm_frames) { res.status = DECODER_READ_EOF; break; }
        if (dec->carry_read_pos >= dec->carry_frames) {
            decoder_read_status_t status = DECODER_READ_OK;
            if (!decode_next_sample_ex(dec, &status)) {
                if (res.frames > 0) {
                    res.status = DECODER_READ_OK;
                } else {
                    res.status = status;
                }
                break;
            }
        }

        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        uint64_t to_copy = frames_to_read - res.frames;
        if (to_copy > available) to_copy = available;
        if (to_copy > dec->total_pcm_frames - dec->pcm_position) to_copy = dec->total_pcm_frames - dec->pcm_position;

        memcpy(buffer_out + res.frames * dec->channels,
               dec->carry_buffer + dec->carry_read_pos * dec->channels,
               (size_t) to_copy * dec->channels * sizeof(int16_t));

        dec->carry_read_pos += (uint32_t) to_copy;
        res.frames += to_copy;
        dec->pcm_position += to_copy;
    }

    return res;
}

decoder_read_result_t alac_read_pcm_frames_s32(alac_decoder_t * dec, uint64_t frames_to_read, int32_t * buffer_out) {
    if (dec && !dec->carry_buffer_s32) {
        dec->carry_buffer_s32 = (int32_t *) malloc((size_t) dec->frame_size * dec->channels * sizeof(int32_t));
        if (dec->carry_buffer_s32 && dec->carry_frames > 0) convert_output(dec, dec->carry_frames);
    }

    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !buffer_out || !dec->carry_buffer_s32) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    while (res.frames < frames_to_read) {
        if (dec->pcm_position >= dec->total_pcm_frames) { res.status = DECODER_READ_EOF; break; }
        if (dec->carry_read_pos >= dec->carry_frames) {
            decoder_read_status_t status = DECODER_READ_OK;
            if (!decode_next_sample_ex(dec, &status)) {
                if (res.frames > 0) {
                    res.status = DECODER_READ_OK;
                } else {
                    res.status = status;
                }
                break;
            }
        }

        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        uint64_t to_copy = frames_to_read - res.frames;
        if (to_copy > available) to_copy = available;
        if (to_copy > dec->total_pcm_frames - dec->pcm_position) to_copy = dec->total_pcm_frames - dec->pcm_position;

        memcpy(buffer_out + res.frames * dec->channels,
               dec->carry_buffer_s32 + dec->carry_read_pos * dec->channels,
               (size_t) to_copy * dec->channels * sizeof(int32_t));

        dec->carry_read_pos += (uint32_t) to_copy;
        res.frames += to_copy;
        dec->pcm_position += to_copy;
    }

    return res;
}

bool alac_seek_to_pcm_frame(alac_decoder_t * dec, uint64_t frame_index) {
    if (!dec) return false;
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;
    uint32_t target_sample;
    uint64_t discard;
    if (!src_map_frame(&dec->src, frame_index, &target_sample, &discard)) return false;
    uint32_t sample_count = src_count(&dec->src);
    if (target_sample >= sample_count) target_sample = sample_count > 0 ? sample_count - 1 : 0;

    dec->current_sample_index = target_sample;
    dec->carry_frames = 0;
    dec->carry_read_pos = 0;
    dec->consecutive_errors = 0;

    if (!decode_next_sample(dec)) return false;
    dec->carry_read_pos = (uint32_t)(discard < dec->carry_frames ? discard : dec->carry_frames);
    dec->pcm_position = frame_index;
    return true;
}

void alac_close(alac_decoder_t * dec) {
    if (!dec) return;
    delete dec->decoder;
    src_close(&dec->src);
    free(dec->compressed_buf);
    free(dec->raw_output_buf);
    free(dec->carry_buffer);
    free(dec->carry_buffer_s32);
    free(dec);
}
