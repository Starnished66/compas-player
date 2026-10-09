#include "opus_decoder.h"
#include "opusfile_alloc.h"

#include <limits.h>
#include <stdlib.h>
#include <stdint.h>

#define OPUS_MAX_CONSECUTIVE_HOLES 64

struct opus_decoder_wrap {
    OggOpusFile * file;
    unsigned int channels;
    uint64_t total_pcm_frames;
    decoder_read_status_t terminal_status;
};

static bool output_channels_for_file(const OggOpusFile * file, unsigned int * channels_out) {
    int links = op_link_count(file);
    if (links <= 0) return false;

    unsigned int output_channels = 1;
    for (int link = 0; link < links; link++) {
        int channels = op_channel_count(file, link);
        if (channels < 1 || channels > 8) return false;
        if (channels > 1) output_channels = 2;
    }
    *channels_out = output_channels;
    return true;
}

opus_decoder_wrap_t * opus_open_file(const char * path) {
    if (!path) return NULL;

    OggOpusFile * file = compas_opusfile_open(path, false);
    if (!file) return NULL;

    unsigned int channels;
    if (!output_channels_for_file(file, &channels)) {
        op_free(file);
        return NULL;
    }

    opus_decoder_wrap_t * dec = calloc(1, sizeof(*dec));
    if (!dec) {
        op_free(file);
        return NULL;
    }

    ogg_int64_t total = op_pcm_total(file, -1);
    dec->file = file;
    dec->channels = channels;
    dec->total_pcm_frames = total > 0 ? (uint64_t) total : 0;
    dec->terminal_status = DECODER_READ_OK;
    return dec;
}

unsigned int opus_get_channels(const opus_decoder_wrap_t * dec) {
    return dec ? dec->channels : 0;
}

unsigned int opus_get_sample_rate(const opus_decoder_wrap_t * dec) {
    (void) dec;
    return 48000;
}

uint64_t opus_get_total_pcm_frame_count(const opus_decoder_wrap_t * dec) {
    return dec ? dec->total_pcm_frames : 0;
}

decoder_read_result_t opus_read_pcm_frames_s16(opus_decoder_wrap_t * dec, uint64_t frames_to_read,
                                                 int16_t * buffer_out) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !dec->file || (!buffer_out && frames_to_read > 0) ||
        (dec && dec->channels > 0 && frames_to_read > SIZE_MAX / sizeof(*buffer_out) / dec->channels)) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }
    if (frames_to_read == 0) return res;
    if (dec->terminal_status != DECODER_READ_OK) {
        res.status = dec->terminal_status;
        return res;
    }

    unsigned int holes = 0;
    while (res.frames < frames_to_read) {
        uint64_t remaining = frames_to_read - res.frames;
        uint64_t max_frames_for_int = (uint64_t) INT_MAX / dec->channels;
        if (remaining > max_frames_for_int) remaining = max_frames_for_int;
        if (remaining == 0) {
            dec->terminal_status = DECODER_READ_FATAL_ERROR;
            break;
        }

        int sample_capacity = (int) (remaining * dec->channels);
        int16_t * dst = buffer_out + (size_t) res.frames * dec->channels;
        int got = dec->channels == 1
            ? op_read(dec->file, dst, sample_capacity, NULL)
            : op_read_stereo(dec->file, dst, sample_capacity);

        if (got == OP_HOLE) {
            if (++holes <= OPUS_MAX_CONSECUTIVE_HOLES) continue;
            dec->terminal_status = DECODER_READ_FATAL_ERROR;
            break;
        }
        if (got < 0) {
            dec->terminal_status = DECODER_READ_FATAL_ERROR;
            break;
        }
        if (got == 0) {
            dec->terminal_status = DECODER_READ_EOF;
            break;
        }

        holes = 0;
        res.frames += (uint64_t) got;
    }

    if (res.frames == 0 && dec->terminal_status != DECODER_READ_OK) {
        res.status = dec->terminal_status;
    }
    return res;
}

bool opus_seek_to_pcm_frame(opus_decoder_wrap_t * dec, uint64_t frame_index) {
    if (!dec || !dec->file) return false;
    bool seek_to_end = dec->total_pcm_frames > 0 && frame_index >= dec->total_pcm_frames;
    if (seek_to_end) {
        /* libopusfile may reject the exact EOS offset. Land on the last
         * decodable frame, then expose the logical end position to callers. */
        frame_index = dec->total_pcm_frames - 1;
    }
    if (frame_index > (uint64_t) INT64_MAX) frame_index = (uint64_t) INT64_MAX;
    if (op_pcm_seek(dec->file, (ogg_int64_t) frame_index) != 0) return false;
    dec->terminal_status = seek_to_end ? DECODER_READ_EOF : DECODER_READ_OK;
    return true;
}

void opus_close(opus_decoder_wrap_t * dec) {
    if (!dec) return;
    if (dec->file) op_free(dec->file);
    free(dec);
}
