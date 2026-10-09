#include "wavpack_decoder.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wavpack.h>

#define WAVPACK_READ_CHUNK 4096U
#define WAVPACK_OPEN_FLAGS (OPEN_WVC | OPEN_DSD_AS_PCM | OPEN_NORMALIZE)

struct wavpack_decoder {
    WavpackContext *context;
    unsigned int channels;
    unsigned int sample_rate;
    unsigned int bits;
    uint64_t total_frames;
    uint64_t current_frame;
    bool is_float;
    decoder_read_status_t terminal_status;
    int32_t *scratch;
};

static int32_t float_to_s24(int32_t word) {
    float sample;
    memcpy(&sample, &word, sizeof(sample));
    if (!isfinite(sample)) return 0;
    double scaled = (double)sample * 8388608.0;
    if (scaled >= 8388607.0) return 8388607;
    if (scaled <= -8388608.0) return -8388608;
    return (int32_t)scaled;
}

static int32_t integer_to_s24(int32_t sample, unsigned int bits) {
    if (bits > 24) return sample >> (bits - 24);
    if (bits < 24) return (int32_t)((uint32_t)sample << (24 - bits));
    return sample;
}

static int16_t sample_to_s16(int32_t sample, unsigned int bits, bool is_float) {
    if (is_float) return (int16_t)(float_to_s24(sample) >> 8);
    if (bits > 16) return (int16_t)(sample >> (bits - 16));
    if (bits < 16) return (int16_t)((uint32_t)sample << (16 - bits));
    return (int16_t)sample;
}

wavpack_decoder_t *wavpack_open_file(const char *path) {
    if (!path) return NULL;
    char error[128] = {0};
    WavpackContext *context = WavpackOpenFileInput(path, error, WAVPACK_OPEN_FLAGS, 0);
    if (!context) return NULL;

    int channels = WavpackGetNumChannels(context);
    uint32_t sample_rate = WavpackGetSampleRate(context);
    int bits = WavpackGetBitsPerSample(context);
    bool is_float = (WavpackGetMode(context) & MODE_FLOAT) != 0;
    int64_t frames = WavpackGetNumSamples64(context);
    if (channels < 1 || channels > 8 || sample_rate == 0 || bits < 1 || bits > 32 || frames < 0) {
        WavpackCloseFile(context);
        return NULL;
    }

    wavpack_decoder_t *dec = calloc(1, sizeof(*dec));
    if (!dec) {
        WavpackCloseFile(context);
        return NULL;
    }
    size_t samples = (size_t)WAVPACK_READ_CHUNK * (size_t)channels;
    if (samples > SIZE_MAX / sizeof(*dec->scratch)) {
        WavpackCloseFile(context);
        free(dec);
        return NULL;
    }
    dec->scratch = malloc(samples * sizeof(*dec->scratch));
    if (!dec->scratch) {
        WavpackCloseFile(context);
        free(dec);
        return NULL;
    }
    dec->context = context;
    dec->channels = (unsigned int)channels;
    dec->sample_rate = sample_rate;
    dec->bits = is_float ? 24U : (unsigned int)bits;
    dec->is_float = is_float;
    dec->total_frames = (uint64_t)frames;
    return dec;
}

unsigned int wavpack_get_channels(const wavpack_decoder_t *dec) { return dec ? dec->channels : 0; }
unsigned int wavpack_get_sample_rate(const wavpack_decoder_t *dec) { return dec ? dec->sample_rate : 0; }
unsigned int wavpack_get_bits_per_sample(const wavpack_decoder_t *dec) { return dec ? dec->bits : 0; }
uint64_t wavpack_get_total_pcm_frame_count(const wavpack_decoder_t *dec) { return dec ? dec->total_frames : 0; }

static decoder_read_result_t read_frames(wavpack_decoder_t *dec, uint64_t requested, void *out, bool to_s16) {
    decoder_read_result_t result = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !dec->context || (!out && requested != 0)) {
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
        if (count > WAVPACK_READ_CHUNK) count = WAVPACK_READ_CHUNK;
        uint32_t got = WavpackUnpackSamples(dec->context, dec->scratch, (uint32_t)count);
        if (!got) {
            dec->terminal_status = dec->current_frame >= dec->total_frames ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
            if (!result.frames) result.status = dec->terminal_status;
            break;
        }
        size_t samples = (size_t)got * dec->channels;
        if (to_s16) {
            int16_t *dst = (int16_t *)out + (size_t)result.frames * dec->channels;
            for (size_t i = 0; i < samples; ++i)
                dst[i] = sample_to_s16(dec->scratch[i], dec->is_float ? 24U : dec->bits, dec->is_float);
        } else {
            int32_t *dst = (int32_t *)out + (size_t)result.frames * dec->channels;
            for (size_t i = 0; i < samples; ++i)
                dst[i] = dec->is_float ? float_to_s24(dec->scratch[i]) : integer_to_s24(dec->scratch[i], dec->bits);
        }
        result.frames += got;
        dec->current_frame += got;
        if (got < count && dec->current_frame < dec->total_frames) {
            dec->terminal_status = DECODER_READ_FATAL_ERROR;
            break;
        }
    }
    if (!result.frames && result.status == DECODER_READ_OK && dec->current_frame >= dec->total_frames)
        result.status = DECODER_READ_EOF;
    return result;
}

decoder_read_result_t wavpack_read_pcm_frames_s16(wavpack_decoder_t *dec, uint64_t frames, int16_t *out) {
    return read_frames(dec, frames, out, true);
}

decoder_read_result_t wavpack_read_pcm_frames_s32(wavpack_decoder_t *dec, uint64_t frames, int32_t *out) {
    return read_frames(dec, frames, out, false);
}

bool wavpack_seek_to_pcm_frame(wavpack_decoder_t *dec, uint64_t frame) {
    if (!dec || !dec->context || frame > dec->total_frames || frame > INT64_MAX) return false;
    if (frame == dec->total_frames) {
        dec->current_frame = frame;
        dec->terminal_status = DECODER_READ_EOF;
        return true;
    }
    if (!WavpackSeekSample64(dec->context, (int64_t)frame)) return false;
    dec->current_frame = frame;
    dec->terminal_status = frame >= dec->total_frames ? DECODER_READ_EOF : DECODER_READ_OK;
    return true;
}

void wavpack_close(wavpack_decoder_t *dec) {
    if (!dec) return;
    if (dec->context) WavpackCloseFile(dec->context);
    free(dec->scratch);
    free(dec);
}

static const char *const TAGS[] = {
    "Title", "Artist", "Album", "Album Artist", "AlbumArtist", "album_artist", "Genre", "Track",
    "Year", "date", "replaygain_track_gain", "replaygain_track_peak", "replaygain_album_gain",
    "replaygain_album_peak", "Compilation", "Disc", "DiscNumber", "Disc Number", "Lyrics"
};

void wavpack_get_tags(const char *path, wavpack_tag_callback_t callback, void *user) {
    if (!path || !callback) return;
    char error[128] = {0};
    WavpackContext *context = WavpackOpenFileInput(path, error, OPEN_TAGS, 0);
    if (!context) return;
    for (size_t i = 0; i < sizeof(TAGS) / sizeof(TAGS[0]); ++i) {
        char value[256] = {0};
        int length = WavpackGetTagItem(context, TAGS[i], value, (int)sizeof(value));
        if (length > 0) {
            value[sizeof(value) - 1] = '\0';
            callback(user, TAGS[i], value);
        }
    }
    WavpackCloseFile(context);
}

unsigned char *wavpack_get_cover(const char *path, size_t max_size, size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!path || !max_size) return NULL;
    char error[128] = {0};
    WavpackContext *context = WavpackOpenFileInput(path, error, OPEN_TAGS, 0);
    if (!context) return NULL;
    const char *const keys[] = { "Cover Art (Front)", "Cover Art (Back)" };
    unsigned char *image = NULL;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && !image; ++i) {
        int size = WavpackGetBinaryTagItem(context, keys[i], NULL, 0);
        if (size <= 1 || (size_t)size > INT_MAX) continue;
        if ((size_t)size > max_size) {
            if (out_size) *out_size = (size_t)size;
            continue;
        }
        char *raw = malloc((size_t)size);
        if (!raw) break;
        if (WavpackGetBinaryTagItem(context, keys[i], raw, size) == size) {
            char *separator = memchr(raw, '\0', (size_t)size);
            if (separator) {
                size_t offset = (size_t)(separator - raw) + 1;
                size_t bytes = (size_t)size - offset;
                if (bytes && bytes <= max_size) {
                    image = malloc(bytes);
                    if (image) {
                        memcpy(image, raw + offset, bytes);
                        if (out_size) *out_size = bytes;
                    }
                }
            }
        }
        free(raw);
    }
    WavpackCloseFile(context);
    return image;
}
