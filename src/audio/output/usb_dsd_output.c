#include "usb_dsd_output.h"
#include "../../core/system/logging/debug_log.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <tinyalsa/asoundlib.h>

#define STAGE_BYTES 8192

typedef struct {
    enum pcm_format format;
    unsigned int word_bytes;
    bool little_endian;
} dsd_format_t;

static const dsd_format_t native_formats[] = {
    { PCM_FORMAT_DSD_U8, 1, false },
    { PCM_FORMAT_DSD_U16_LE, 2, true },
    { PCM_FORMAT_DSD_U16_BE, 2, false },
    { PCM_FORMAT_DSD_U32_LE, 4, true },
    { PCM_FORMAT_DSD_U32_BE, 4, false },
};

static const enum pcm_format dop_formats[] = {
    PCM_FORMAT_S24_LE, PCM_FORMAT_S24_3LE, PCM_FORMAT_S32_LE
};

struct usb_dsd_output {
    struct pcm *pcm;
    bool native;
    unsigned int channels;
    unsigned int rate;
    unsigned int word_bytes;
    bool little_endian;
    enum pcm_format format;
    uint8_t stage[STAGE_BYTES];
    int32_t carry[8];
    bool carry_valid;
    bool poisoned;
};

#ifdef TEST_BUILD_TAG
static const char *format_name(enum pcm_format format) {
    switch (format) {
    case PCM_FORMAT_DSD_U8: return "DSD_U8";
    case PCM_FORMAT_DSD_U16_LE: return "DSD_U16_LE";
    case PCM_FORMAT_DSD_U16_BE: return "DSD_U16_BE";
    case PCM_FORMAT_DSD_U32_LE: return "DSD_U32_LE";
    case PCM_FORMAT_DSD_U32_BE: return "DSD_U32_BE";
    case PCM_FORMAT_S24_LE: return "S24_LE";
    case PCM_FORMAT_S24_3LE: return "S24_3LE";
    case PCM_FORMAT_S32_LE: return "S32_LE";
    default: return "unknown";
    }
}
#endif

static bool rate_in_range(const struct pcm_params *params, unsigned int rate) {
    return rate >= pcm_params_get_min(params, PCM_PARAM_RATE) &&
           rate <= pcm_params_get_max(params, PCM_PARAM_RATE);
}

static unsigned int initial_period_size(unsigned int rate) {
    uint64_t budget = (uint64_t)rate * 17u / 400u;
    unsigned int size = 1024;
    while ((uint64_t)size * 2u <= budget && size <= UINT_MAX / 2u) size *= 2u;
    return size;
}

static struct pcm * try_open(unsigned int card, unsigned int device, unsigned int channels,
                             unsigned int rate, enum pcm_format format) {
    struct pcm_config config;
    memset(&config, 0, sizeof(config));
    config.channels = channels;
    config.rate = rate;
    config.format = format;
    config.period_count = 4;
    for (config.period_size = initial_period_size(rate);; config.period_size /= 2u) {
        config.start_threshold = config.period_size * config.period_count;
        config.stop_threshold = config.start_threshold;
        struct pcm *pcm = pcm_open(card, device, PCM_OUT, &config);
        if (pcm && pcm_is_ready(pcm)) return pcm;
        DBG_LOG("usb_dsd: hw:%u,%u rejected %s channels=%u rate=%u period=%u: %s\n",
                card, device, format_name(format), channels, rate, config.period_size,
                pcm ? pcm_get_error(pcm) : "pcm_open returned NULL");
        if (pcm) pcm_close(pcm);
        if (config.period_size == 1024) break;
    }
    return NULL;
}

usb_dsd_output_t * usb_dsd_output_open(unsigned int card, unsigned int device,
                                        bool native, unsigned int channels,
                                        unsigned int dop_rate) {
    if (!channels || channels > 8 || !dop_rate || dop_rate > UINT_MAX / 2u) return NULL;
    struct pcm_params *params = pcm_params_get(card, device, PCM_OUT);
    if (!params) {
        DBG_LOG("usb_dsd: cannot query hw:%u,%u capabilities for %s (DoP rate=%u)\n",
                card, device, native ? "native DSD" : "DoP", dop_rate);
        return NULL;
    }
    DBG_LOG("usb_dsd: hw:%u,%u requested %s channels=%u DoP-rate=%u, ALSA rate range=%u..%u\n",
            card, device, native ? "native DSD" : "DoP", channels, dop_rate,
            pcm_params_get_min(params, PCM_PARAM_RATE), pcm_params_get_max(params, PCM_PARAM_RATE));
    usb_dsd_output_t *out = calloc(1, sizeof(*out));
    if (!out) { pcm_params_free(params); return NULL; }
    out->native = native;
    out->channels = channels;

    if (native) {
        for (size_t i = 0; i < sizeof(native_formats) / sizeof(native_formats[0]); i++) {
            const dsd_format_t *f = &native_formats[i];
            unsigned int rate = dop_rate * 2u / f->word_bytes;
            if (!pcm_params_format_test(params, f->format)) {
                DBG_LOG("usb_dsd: %s is not advertised by hw:%u,%u\n", format_name(f->format), card, device);
                continue;
            }
            if (!rate || !rate_in_range(params, rate)) {
                DBG_LOG("usb_dsd: %s rate=%u is outside advertised range\n", format_name(f->format), rate);
                continue;
            }
            out->pcm = try_open(card, device, channels, rate, f->format);
            if (out->pcm) {
                out->format = f->format;
                out->word_bytes = f->word_bytes;
                out->little_endian = f->little_endian;
                out->rate = rate;
                break;
            }
        }
    } else {
        for (size_t i = 0; i < sizeof(dop_formats) / sizeof(dop_formats[0]); i++) {
            enum pcm_format format = dop_formats[i];
            if (!pcm_params_format_test(params, format) || !rate_in_range(params, dop_rate)) {
                DBG_LOG("usb_dsd: DoP %s at %u Hz is not advertised by hw:%u,%u\n",
                        format_name(format), dop_rate, card, device);
                continue;
            }
            out->pcm = try_open(card, device, channels, dop_rate, format);
            if (out->pcm) { out->format = format; out->rate = dop_rate; break; }
        }
    }
    pcm_params_free(params);
    if (!out->pcm) {
        DBG_LOG("usb_dsd: hw:%u,%u has no usable %s configuration; passthrough refused\n",
                card, device, native ? "native DSD" : "DoP");
        free(out);
        return NULL;
    }
    DBG_LOG("usb_dsd: hw:%u,%u opened %s, format=%s channels=%u hardware-rate=%u\n",
            card, device, native ? "native DSD" : "DoP", format_name(out->format), channels, out->rate);
    return out;
}

static uint8_t dop_byte(int32_t word, unsigned int byte) {
    return byte == 0 ? (uint8_t)((uint32_t)word >> 8) : (uint8_t)word;
}

static bool write_stage(usb_dsd_output_t *out, const uint8_t *data, uint64_t frames,
                        size_t bytes_per_frame, uint64_t *frames_done) {
    uint64_t done = 0;
    while (done < frames) {
        int ret = pcm_writei(out->pcm, data + done * bytes_per_frame,
                             (unsigned int)(frames - done));
        if (ret <= 0) {
            if (frames_done) *frames_done = done;
            return false;
        }
        done += (unsigned int)ret;
    }
    if (frames_done) *frames_done = done;
    return true;
}

bool usb_dsd_output_write(usb_dsd_output_t *out, const int32_t *dop_words,
                          uint64_t frames, uint64_t *frames_written) {
    if (frames_written) *frames_written = 0;
    if (!out || !out->pcm || out->poisoned || (!dop_words && frames)) return false;
    if (!frames) return true;
    if (out->native) {
        /* Convert frame-wise per channel; interleaved channel bytes must not
         * be grouped together into a wider native DSD word. */
        uint64_t source_pos = 0;
        if (out->word_bytes == 4 && out->carry_valid) {
            int32_t pair[16];
            for (unsigned int ch = 0; ch < out->channels; ch++) {
                pair[ch] = out->carry[ch];
                pair[out->channels + ch] = dop_words[ch];
            }
            for (unsigned int ch = 0; ch < out->channels; ch++) {
                uint8_t b[4] = { dop_byte(pair[ch],0), dop_byte(pair[ch],1),
                                 dop_byte(pair[out->channels + ch],0), dop_byte(pair[out->channels + ch],1) };
                for (unsigned int j = 0; j < 4; j++) out->stage[ch * 4 + j] = b[out->little_endian ? 3-j : j];
            }
            uint64_t hardware_done = 0;
            if (!write_stage(out, out->stage, 1, out->channels * 4u, &hardware_done)) {
                out->poisoned = true;
                return false;
            }
            out->carry_valid = false;
            source_pos = 1;
        }
        uint64_t available = frames - source_pos;
        uint64_t output_frames = out->word_bytes == 1 ? available * 2 :
                                 out->word_bytes == 2 ? available : available / 2;
        uint64_t output_done = 0;
        while (output_done < output_frames) {
            size_t max_out = sizeof(out->stage) / (out->channels * out->word_bytes);
            uint64_t count = output_frames - output_done;
            if (count > max_out) count = max_out;
            for (uint64_t f = 0; f < count; f++) {
                for (unsigned int ch = 0; ch < out->channels; ch++) {
                    uint8_t b[4];
                    if (out->word_bytes == 1) {
                        uint64_t src = source_pos + (output_done + f) / 2;
                        unsigned int which = (unsigned int)((output_done + f) % 2);
                        b[0] = dop_byte(dop_words[src * out->channels + ch], which);
                    } else if (out->word_bytes == 2) {
                        uint64_t src = source_pos + output_done + f;
                        b[0] = dop_byte(dop_words[src * out->channels + ch], 0);
                        b[1] = dop_byte(dop_words[src * out->channels + ch], 1);
                    } else {
                        uint64_t src = source_pos + 2 * (output_done + f);
                        b[0] = dop_byte(dop_words[src * out->channels + ch], 0);
                        b[1] = dop_byte(dop_words[src * out->channels + ch], 1);
                        b[2] = dop_byte(dop_words[(src + 1) * out->channels + ch], 0);
                        b[3] = dop_byte(dop_words[(src + 1) * out->channels + ch], 1);
                    }
                    size_t at = ((size_t)f * out->channels + ch) * out->word_bytes;
                    for (unsigned int j = 0; j < out->word_bytes; j++)
                        out->stage[at + j] = b[out->little_endian ? out->word_bytes - 1 - j : j];
                }
            }
            uint64_t hardware_done = 0;
            if (!write_stage(out, out->stage, count, out->channels * out->word_bytes, &hardware_done)) {
                uint64_t native_done = output_done + hardware_done;
                uint64_t source_done = out->word_bytes == 1 ? native_done / 2 :
                                       out->word_bytes == 2 ? native_done : native_done * 2;
                if (frames_written) *frames_written = source_pos + source_done;
                out->poisoned = true;
                return false;
            }
            output_done += count;
        }
        if (out->word_bytes == 4 && (available & 1u)) {
            for (unsigned int ch = 0; ch < out->channels; ch++) out->carry[ch] = dop_words[(frames - 1) * out->channels + ch];
            out->carry_valid = true;
        }
        if (frames_written) *frames_written = frames;
        return true;
    }
    uint64_t done = 0;
    while (done < frames) {
        uint64_t count = frames - done;
        size_t cap = sizeof(out->stage) / (out->channels * 4u);
        if (count > cap) count = cap;
        size_t samples = (size_t) count * out->channels;
        for (size_t i = 0; i < samples; i++) {
            uint32_t value = (uint32_t)dop_words[done * out->channels + i];
            if (out->format == PCM_FORMAT_S24_LE) memcpy(out->stage + i * 4u, &value, 4);
            else if (out->format == PCM_FORMAT_S24_3LE) {
                out->stage[i * 3u] = value & 0xff;
                out->stage[i * 3u + 1] = (value >> 8) & 0xff;
                out->stage[i * 3u + 2] = (value >> 16) & 0xff;
            } else {
                uint32_t aligned = value << 8;
                memcpy(out->stage + i * 4u, &aligned, 4);
            }
        }
        size_t bytes_per_frame = out->channels * (out->format == PCM_FORMAT_S24_3LE ? 3u : 4u);
        uint64_t hardware_done = 0;
        if (!write_stage(out, out->stage, count, bytes_per_frame, &hardware_done)) {
            if (frames_written) *frames_written = done + hardware_done;
            out->poisoned = true;
            return false;
        }
        done += count;
    }
    if (frames_written) *frames_written = frames;
    return true;
}

void usb_dsd_output_close(usb_dsd_output_t *out) {
    if (!out) return;
    if (out->pcm) pcm_close(out->pcm);
    free(out);
}

bool usb_dsd_output_is_native(const usb_dsd_output_t *out) { return out && out->native; }
unsigned int usb_dsd_output_hardware_rate(const usb_dsd_output_t *out) { return out ? out->rate : 0; }
unsigned int usb_dsd_output_hardware_bits(const usb_dsd_output_t *out) {
    if (!out) return 0;
    if (out->native) return out->word_bytes * 8u;
    switch (out->format) {
    case PCM_FORMAT_S32_LE: return 32;
    case PCM_FORMAT_S24_LE:
    case PCM_FORMAT_S24_3LE: return 24;
    default: return 0;
    }
}
