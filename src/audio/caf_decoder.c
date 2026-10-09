#include "caf_decoder.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sndfile.h>

#define CAF_READ_CHUNK 4096U

struct caf_decoder {
    SNDFILE *file;
    unsigned int channels;
    unsigned int sample_rate;
    unsigned int bits;
    uint64_t total_frames;
    uint64_t current_frame;
    bool alac;
    bool is_float;
    decoder_read_status_t terminal_status;
    int32_t *scratch;
    float *float_scratch;
    double *double_scratch;
};

static unsigned int subtype_bits(int subtype) {
    switch (subtype) {
        case SF_FORMAT_PCM_S8: return 8;
        case SF_FORMAT_PCM_U8: return 8;
        case SF_FORMAT_PCM_16: return 16;
        case SF_FORMAT_PCM_24: return 24;
        case SF_FORMAT_PCM_32: return 32;
        case SF_FORMAT_FLOAT: return 32;
        case SF_FORMAT_DOUBLE: return 64;
        case SF_FORMAT_ALAC_16: return 16;
        case SF_FORMAT_ALAC_20: return 20;
        case SF_FORMAT_ALAC_24: return 24;
        case SF_FORMAT_ALAC_32: return 32;
        default: return 0;
    }
}

caf_decoder_t *caf_open_file(const char *path) {
    if (!path) return NULL;
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE *file = sf_open(path, SFM_READ, &info);
    if (!file) return NULL;
    int subtype = info.format & SF_FORMAT_SUBMASK;
    unsigned int bits = subtype_bits(subtype);
    if ((info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_CAF || info.channels < 1 || info.channels > 8 ||
        info.samplerate <= 0 || info.frames < 0 || !bits) {
        sf_close(file);
        return NULL;
    }

    caf_decoder_t *dec = calloc(1, sizeof(*dec));
    if (!dec) {
        sf_close(file);
        return NULL;
    }
    size_t samples = (size_t)CAF_READ_CHUNK * (size_t)info.channels;
    if (samples > SIZE_MAX / sizeof(*dec->scratch)) {
        sf_close(file);
        free(dec);
        return NULL;
    }
    dec->scratch = malloc(samples * sizeof(*dec->scratch));
    if (!dec->scratch) {
        sf_close(file);
        free(dec);
        return NULL;
    }
    dec->file = file;
    dec->channels = (unsigned int)info.channels;
    dec->sample_rate = (unsigned int)info.samplerate;
    dec->bits = bits;
    dec->total_frames = (uint64_t)info.frames;
    dec->alac = subtype >= SF_FORMAT_ALAC_16 && subtype <= SF_FORMAT_ALAC_32;
    dec->is_float = subtype == SF_FORMAT_FLOAT || subtype == SF_FORMAT_DOUBLE;
    if (subtype == SF_FORMAT_FLOAT) {
        dec->float_scratch = malloc(samples * sizeof(*dec->float_scratch));
        if (!dec->float_scratch) {
            sf_close(file); free(dec->scratch); free(dec); return NULL;
        }
    } else if (subtype == SF_FORMAT_DOUBLE) {
        dec->double_scratch = malloc(samples * sizeof(*dec->double_scratch));
        if (!dec->double_scratch) {
            sf_close(file); free(dec->scratch); free(dec); return NULL;
        }
    }
    return dec;
}

unsigned int caf_get_channels(const caf_decoder_t *dec) { return dec ? dec->channels : 0; }
unsigned int caf_get_sample_rate(const caf_decoder_t *dec) { return dec ? dec->sample_rate : 0; }
unsigned int caf_get_bits_per_sample(const caf_decoder_t *dec) { return dec ? dec->bits : 0; }
uint64_t caf_get_total_pcm_frame_count(const caf_decoder_t *dec) { return dec ? dec->total_frames : 0; }
bool caf_is_alac(const caf_decoder_t *dec) { return dec && dec->alac; }

static decoder_read_result_t read_frames(caf_decoder_t *dec, uint64_t requested, void *out, bool to_s16) {
    decoder_read_result_t result = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !dec->file || (!out && requested)) {
        result.status = DECODER_READ_FATAL_ERROR;
        return result;
    }
    if (!requested) return result;
    size_t output_sample_size = to_s16 ? sizeof(int16_t) : sizeof(int32_t);
    if (requested > SIZE_MAX / output_sample_size / dec->channels) {
        result.status = DECODER_READ_FATAL_ERROR;
        return result;
    }
    if (dec->current_frame >= dec->total_frames) {
        result.status = DECODER_READ_EOF;
        return result;
    }
    if (dec->terminal_status != DECODER_READ_OK) {
        result.status = dec->terminal_status;
        return result;
    }
    uint64_t left = dec->total_frames - dec->current_frame;
    if (requested > left) requested = left;
    while (result.frames < requested) {
        uint64_t count = requested - result.frames;
        if (count > CAF_READ_CHUNK) count = CAF_READ_CHUNK;
        sf_count_t got;
        if (dec->bits == 32 && dec->float_scratch) got = sf_readf_float(dec->file, dec->float_scratch, (sf_count_t)count);
        else if (dec->bits == 64 && dec->double_scratch) got = sf_readf_double(dec->file, dec->double_scratch, (sf_count_t)count);
        else got = sf_readf_int(dec->file, dec->scratch, (sf_count_t)count);
        if (got <= 0) {
            dec->terminal_status = dec->current_frame >= dec->total_frames ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
            if (!result.frames) result.status = dec->terminal_status;
            break;
        }
        size_t samples = (size_t)got * dec->channels;
        if (dec->is_float) {
            for (size_t i = 0; i < samples; ++i) {
                double sample = dec->float_scratch ? dec->float_scratch[i] : dec->double_scratch[i];
                int32_t q24;
                if (!isfinite(sample)) q24 = 0;
                else if (sample >= 1.0) q24 = 8388607;
                else if (sample <= -1.0) q24 = -8388608;
                else q24 = (int32_t)(sample * 8388608.0);
                if (to_s16) ((int16_t *)out)[(size_t)result.frames * dec->channels + i] = (int16_t)(q24 >> 8);
                else ((int32_t *)out)[(size_t)result.frames * dec->channels + i] = q24;
            }
        } else if (to_s16) {
            int16_t *dst = (int16_t *)out + (size_t)result.frames * dec->channels;
            for (size_t i = 0; i < samples; ++i) dst[i] = (int16_t)(dec->scratch[i] >> 16);
        } else {
            int32_t *dst = (int32_t *)out + (size_t)result.frames * dec->channels;
            for (size_t i = 0; i < samples; ++i) dst[i] = dec->scratch[i] >> 8;
        }
        result.frames += (uint64_t)got;
        dec->current_frame += (uint64_t)got;
        if ((uint64_t)got < count) {
            dec->terminal_status = dec->current_frame >= dec->total_frames ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
            break;
        }
    }
    return result;
}

decoder_read_result_t caf_read_pcm_frames_s16(caf_decoder_t *dec, uint64_t frames, int16_t *out) {
    return read_frames(dec, frames, out, true);
}

decoder_read_result_t caf_read_pcm_frames_s32(caf_decoder_t *dec, uint64_t frames, int32_t *out) {
    return read_frames(dec, frames, out, false);
}

bool caf_seek_to_pcm_frame(caf_decoder_t *dec, uint64_t frame) {
    if (!dec || !dec->file || frame > dec->total_frames || frame > (uint64_t)LLONG_MAX) return false;
    sf_count_t actual = sf_seek(dec->file, (sf_count_t)frame, SEEK_SET);
    if (actual < 0 || (uint64_t)actual != frame) return false;
    dec->current_frame = frame;
    dec->terminal_status = frame >= dec->total_frames ? DECODER_READ_EOF : DECODER_READ_OK;
    return true;
}

void caf_close(caf_decoder_t *dec) {
    if (!dec) return;
    if (dec->file) sf_close(dec->file);
    free(dec->scratch);
    free(dec->float_scratch);
    free(dec->double_scratch);
    free(dec);
}

static const struct { int type; const char *key; } TAGS[] = {
    { SF_STR_TITLE, "TITLE" }, { SF_STR_ARTIST, "ARTIST" }, { SF_STR_ALBUM, "ALBUM" },
    { SF_STR_DATE, "DATE" }, { SF_STR_TRACKNUMBER, "TRACKNUMBER" }, { SF_STR_GENRE, "GENRE" }
};

void caf_get_tags(const char *path, caf_tag_callback_t callback, void *user) {
    if (!path || !callback) return;
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE *file = sf_open(path, SFM_READ, &info);
    if (!file) return;
    if ((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_CAF) {
        for (size_t i = 0; i < sizeof(TAGS) / sizeof(TAGS[0]); ++i) {
            const char *value = sf_get_string(file, TAGS[i].type);
            if (value && value[0]) {
                char bounded[512];
                size_t length = 0;
                while (length < sizeof(bounded) - 1 && value[length]) ++length;
                memcpy(bounded, value, length);
                bounded[length] = '\0';
                callback(user, TAGS[i].key, bounded);
            }
        }
    }
    sf_close(file);
}
