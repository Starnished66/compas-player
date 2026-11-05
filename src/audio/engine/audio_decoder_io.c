/* Decoder reads, seeks and close: one switch per codec. */
#include "audio_internal.h"
#include "audio_helpers.h"
#include "../../ui/text/i18n/i18n.h"
#include "../demux/mp4/mp4_demux.h"
#include "../demux/ape/ape_demux.h"
#include "../decoders/ogg/ogg_probe.h"
#include "../effects/peq.h"
#include "../timing/audio_tempo.h"
#include "../../network/http/stream/http_stream_file.h"
#include "../../library/remote/remote_track.h"
#include "../../library/metadata/parsing/metadata.h"
#include "../../library/parsers/cue/cue_parser.h"
#include "../../core/system/logging/debug_log.h"
#include "../../core/persistence/storage_paths.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#ifdef HOST_BUILD
  #include <SDL2/SDL.h>
#else
  #include "../output/audio_output.h"
#endif

static decoder_read_result_t read_result_from_count(uint64_t frames) {
    return (decoder_read_result_t) { .frames = frames,
                                     .status = frames > 0 ? DECODER_READ_OK : DECODER_READ_EOF };
}

/* Limits a read to what is left of the current segment. False when none is. */
static bool segment_clamp(const decoder_t * dec, uint64_t * frames) {
    uint64_t remaining = dec->segment_frame_count > dec->segment_read_cursor
                       ? dec->segment_frame_count - dec->segment_read_cursor : 0;
    if (!remaining) return false;
    if (*frames > remaining) *frames = remaining;
    return true;
}

#ifndef HOST_BUILD
/* dr_libs returns FLAC and WAV 24-bit audio left-justified (MSB at bit 31).
 * An arithmetic right shift by 8 replicates the sign bit and places the
 * sample in the low 24 bits for ALSA S24_LE. */
static void shift_to_s24(int32_t * buf, size_t samples) {
    for (size_t i = 0; i < samples; i++) buf[i] >>= 8;
}
#endif

static decoder_read_result_t decoder_read_s16_raw(decoder_t * dec, uint64_t frames, int16_t * buf) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !buf) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    switch (dec->type) {
        case DECODER_FLAC: {
            uint64_t r = drflac_read_pcm_frames_s16(dec->as.flac, frames, buf);
            return read_result_from_count(r);
        }
        case DECODER_MP3: {
            uint64_t r = drmp3_read_pcm_frames_s16(dec->as.mp3, frames, buf);
            return read_result_from_count(r);
        }
        case DECODER_WAV: {
            uint64_t r = drwav_read_pcm_frames_s16(dec->as.wav, frames, buf);
            return read_result_from_count(r);
        }
        case DECODER_WAVPACK:
            return wavpack_read_pcm_frames_s16(dec->as.wavpack, frames, buf);
        case DECODER_CAF:
            return caf_read_pcm_frames_s16(dec->as.caf, frames, buf);
        case DECODER_AIFF:
            return aiff_read_pcm_frames_s16(dec->as.aiff, frames, buf);
        case DECODER_DSD: {
            decoder_read_result_t r = dsd_read_pcm_frames_s16(dec->as.dsd, frames, buf);
            /* The decoder takes a truncated file's real length at open; this
             * keeps the loop in step if the file shrinks during playback, so
             * that end is not treated as a premature EOF. */
            if (!dec->segmented)
                dec->total_frames = dsd_get_total_pcm_frame_count(dec->as.dsd);
            return r;
        }
        case DECODER_AAC:
            return aac_read_pcm_frames_s16(dec->as.aac, frames, buf);
        case DECODER_ALAC:
            return alac_read_pcm_frames_s16(dec->as.alac, frames, buf);
        case DECODER_APE:
            return ape_read_pcm_frames_s16(dec->as.ape, frames, buf);
        case DECODER_WMA:
            return wma_read_pcm_frames_s16(dec->as.wma, frames, buf);
        case DECODER_OPUS:
            return opus_read_pcm_frames_s16(dec->as.opus, frames, buf);
        case DECODER_VORBIS:
            return vorbis_read_pcm_frames_s16(dec->as.vorbis, frames, buf);
    }
    res.status = DECODER_READ_FATAL_ERROR;
    return res;
}

decoder_read_result_t decoder_read_s16(decoder_t * dec, uint64_t frames, int16_t * buf) {
    if (!dec || !dec->segmented) return decoder_read_s16_raw(dec, frames, buf);
    if (!segment_clamp(dec, &frames)) return (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_EOF };
    decoder_read_result_t result = decoder_read_s16_raw(dec, frames, buf);
    dec->segment_read_cursor += result.frames;
    return result;
}

#ifndef HOST_BUILD
static decoder_read_result_t decoder_read_s32_raw(decoder_t * dec, uint64_t frames, int32_t * buf) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !buf) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    switch (dec->type) {
        case DECODER_FLAC: {
            uint64_t r = drflac_read_pcm_frames_s32(dec->as.flac, frames, buf);
            shift_to_s24(buf, (size_t) r * dec->channels);
            return read_result_from_count(r);
        }
        case DECODER_WAV: {
            uint64_t r = drwav_read_pcm_frames_s32(dec->as.wav, frames, buf);
            shift_to_s24(buf, (size_t) r * dec->channels);
            return read_result_from_count(r);
        }
        case DECODER_WAVPACK:
            return wavpack_read_pcm_frames_s32(dec->as.wavpack, frames, buf);
        case DECODER_CAF:
            return caf_read_pcm_frames_s32(dec->as.caf, frames, buf);
        case DECODER_ALAC:
            /* ALAC decodes internally to right-justified true-magnitude int32 for
             * 24-bit sources: DO NOT shift; already in S24_LE layout. */
            return alac_read_pcm_frames_s32(dec->as.alac, frames, buf);
        case DECODER_APE:
            /* APE decodes internally to right-justified true-magnitude int32 for
             * 24-bit sources: DO NOT shift; already in S24_LE layout. */
            return ape_read_pcm_frames_s32(dec->as.ape, frames, buf);
        case DECODER_AIFF:
            /* AIFF decodes internally to right-justified sign-extended int32 for
             * 24-bit sources (32-bit sources are reduced to 24 there):
             * DO NOT shift; already in S24_LE layout. */
            return aiff_read_pcm_frames_s32(dec->as.aiff, frames, buf);
        case DECODER_DSD:
            /* The DSD decimator's float output is quantized straight to
             * right-justified 24-bit: already in S24_LE layout. */
        {
            decoder_read_result_t r = dsd_read_pcm_frames_s32(dec->as.dsd, frames, buf);
            if (!dec->segmented)
                dec->total_frames = dsd_get_total_pcm_frame_count(dec->as.dsd); /* see decoder_read_s16() */
            return r;
        }
        default:
            /* Only the formats above have a wide read path.
             * Any other decoder reaching here is a caller bug. */
            res.status = DECODER_READ_FATAL_ERROR;
            return res;
    }
}

decoder_read_result_t decoder_read_s32(decoder_t * dec, uint64_t frames, int32_t * buf) {
    if (!dec || !dec->segmented) return decoder_read_s32_raw(dec, frames, buf);
    if (!segment_clamp(dec, &frames)) return (decoder_read_result_t) { .frames = 0, .status = DECODER_READ_EOF };
    decoder_read_result_t result = decoder_read_s32_raw(dec, frames, buf);
    dec->segment_read_cursor += result.frames;
    return result;
}
#endif

bool mp3_needs_seek_index(const decoder_t * dec) {
    return dec && !dec->segmented && !dec->net_stream && dec->type == DECODER_MP3 && dec->sample_rate > 0 &&
           dec->total_frames >= (uint64_t) dec->sample_rate * MP3_SEEK_INDEX_INTERVAL_SECONDS * 2;
}

bool decoder_seek_raw(decoder_t * dec, uint64_t frame) {
    if (!dec || !dec->seekable)
        return false;
    switch (dec->type) {
        case DECODER_FLAC:   return drflac_seek_to_pcm_frame(dec->as.flac, frame) != 0;
        case DECODER_MP3:
            /* Refuse only while an index attempt is still outstanding, so
             * the caller's defer-and-wait path (audio_thread_func's own
             * hold_for_mp3_seek) gets a chance to use the fast table instead
             * of racing it. Once mp3_seek_index_attempted is true (success
             * or a permanent failure), always proceed: drmp3_seek_to_pcm_
             * frame() uses the bound table if there is one, or falls back to
             * its own brute-force seek as a last resort otherwise -- never
             * silently drops the seek. */
            if (frame > 0 && mp3_needs_seek_index(dec) && !dec->mp3_seek_points &&
                !dec->mp3_seek_index_attempted) return false;
            return drmp3_seek_to_pcm_frame(dec->as.mp3, frame) != 0;
        case DECODER_WAVPACK: return wavpack_seek_to_pcm_frame(dec->as.wavpack, frame);
        case DECODER_CAF: return caf_seek_to_pcm_frame(dec->as.caf, frame);
        case DECODER_WAV:    return drwav_seek_to_pcm_frame(dec->as.wav, frame) != 0;
        case DECODER_AIFF:   return aiff_seek_to_pcm_frame(dec->as.aiff, frame);
        case DECODER_DSD:    return dsd_seek_to_pcm_frame(dec->as.dsd, frame);
        case DECODER_AAC:    return aac_seek_to_pcm_frame(dec->as.aac, frame);
        case DECODER_ALAC:   return alac_seek_to_pcm_frame(dec->as.alac, frame);
        case DECODER_APE:    return ape_seek_to_pcm_frame(dec->as.ape, frame);
        case DECODER_WMA:    return wma_seek_to_pcm_frame(dec->as.wma, frame);
        case DECODER_OPUS:   return opus_seek_to_pcm_frame(dec->as.opus, frame);
        case DECODER_VORBIS: return vorbis_seek_to_pcm_frame(dec->as.vorbis, frame);
    }
    return false;
}

bool decoder_seek(decoder_t * dec, uint64_t frame) {
    if (!dec || !dec->seekable) return false;
    if (!dec->segmented) return decoder_seek_raw(dec, frame);
    if (frame > dec->segment_frame_count) frame = dec->segment_frame_count;
    if (dec->segment_start_frame > UINT64_MAX - frame) return false;
    if (!decoder_seek_raw(dec, dec->segment_start_frame + frame)) return false;
    dec->segment_read_cursor = frame;
    return true;
}

/* Only these local PCM-capable decoders have a frame-addressable seek
 * contract suitable for promising sample-accurate loop boundaries. */
bool decoder_supports_sample_accurate_loop(const decoder_t * dec) {
    if (!dec || dec->net_stream || dec->source_bit_depth == 0 ||
        dec->source_bit_depth > 16 || dec->channels == 0 ||
        dec->sample_rate == 0 || dec->total_frames == 0)
        return false;
    return dec->type == DECODER_FLAC || dec->type == DECODER_WAV ||
           dec->type == DECODER_AIFF || dec->type == DECODER_CAF || dec->type == DECODER_ALAC;
}

/* Every branch nulls the union member it just freed, making this safe to
 * call more than once in a row on the same decoder_t without an
 * intervening decoder_open() (whose own memset() would otherwise be the
 * only thing clearing a stale pointer). reopen_decoder_at()'s premature-EOF
 * recovery path relies on exactly this: it calls decoder_close() itself
 * when a post-reopen seek fails, then returns false, and the retry loop
 * that called it goes straight into another decoder_close() (via the next
 * reopen_decoder_at() attempt) without an open in between. Before this
 * nulled the pointer, that second close ran drflac_close() (etc.) again on
 * an already-freed pointer -- a real use-after-free/double-free, reliably
 * reachable by any local file whose decoder opens fine but whose audio data
 * is corrupt at the retry position (confirmed independently with ffmpeg on
 * a real-world file: "invalid sync code" / "invalid frame header"). */
void decoder_close(decoder_t * dec) {
    switch (dec->type) {
        case DECODER_FLAC:
            if (dec->as.flac) { drflac_close(dec->as.flac); dec->as.flac = NULL; }
            if (dec->flac_file) { fclose(dec->flac_file); dec->flac_file = NULL; }
            if (dec->net_stream) { http_stream_close(dec->net_stream); dec->net_stream = NULL; }
            break;
        case DECODER_MP3:
            if (dec->as.mp3) {
                drmp3_bind_seek_table(dec->as.mp3, 0, NULL);
                drmp3_uninit(dec->as.mp3);
                free(dec->as.mp3);
                dec->as.mp3 = NULL;
            }
            free(dec->net_mp3);
            dec->net_mp3 = NULL;
            free(dec->mp3_seek_points);
            dec->mp3_seek_points = NULL;
            dec->mp3_seek_point_count = 0;
            if (dec->net_stream) { http_stream_close(dec->net_stream); dec->net_stream = NULL; }
            break;
        case DECODER_WAV:
            if (dec->as.wav) { drwav_uninit(dec->as.wav); free(dec->as.wav); dec->as.wav = NULL; }
            break;
        case DECODER_AIFF:
            if (dec->as.aiff) { aiff_close(dec->as.aiff); dec->as.aiff = NULL; }
            break;
        case DECODER_DSD:
            if (dec->as.dsd) { dsd_close(dec->as.dsd); dec->as.dsd = NULL; }
            break;
        case DECODER_AAC:
            if (dec->as.aac) { aac_close(dec->as.aac); dec->as.aac = NULL; }
            if (dec->net_stream) { http_stream_close(dec->net_stream); dec->net_stream = NULL; }
            break;
        case DECODER_ALAC:
            if (dec->as.alac) { alac_close(dec->as.alac); dec->as.alac = NULL; }
            break;
        case DECODER_APE:
            if (dec->as.ape) { ape_close(dec->as.ape); dec->as.ape = NULL; }
            break;
        case DECODER_WMA:
            if (dec->as.wma) { wma_close(dec->as.wma); dec->as.wma = NULL; }
            break;
        case DECODER_OPUS:
            if (dec->as.opus) { opus_close(dec->as.opus); dec->as.opus = NULL; }
            break;
        case DECODER_WAVPACK:
            wavpack_close(dec->as.wavpack);
            dec->as.wavpack = NULL;
            break;
        case DECODER_CAF:
            caf_close(dec->as.caf);
            dec->as.caf = NULL;
            break;
        case DECODER_VORBIS:
            if (dec->as.vorbis) { vorbis_close(dec->as.vorbis); dec->as.vorbis = NULL; }
            break;
    }
}

/* Closes and reopens a local decoder at last_confirmed_frame.
 * Only valid for finite local files (not net_stream). Returns true on
 * success; on failure dec is left in a closed/invalid state. */
bool reopen_decoder_at(decoder_t * dec, const char * path,
                               uint64_t last_confirmed_frame) {
    drmp3_seek_point * saved_mp3_points = dec->mp3_seek_points;
    drmp3_uint32 saved_mp3_count = dec->mp3_seek_point_count;
    bool saved_mp3_index_attempted = dec->mp3_seek_index_attempted;
    mp3_stream_id_t saved_mp3_stream = {0};
    if (saved_mp3_points) saved_mp3_stream = mp3_stream_id_of(dec->as.mp3);
    dec->mp3_seek_points = NULL;
    dec->mp3_seek_point_count = 0;
    /* last_confirmed_frame counts DoP frames for a DoP decoder. */
    bool was_dop = decoder_is_dop(dec);
    decoder_close(dec);
    if (!decoder_open(dec, path)) {
        free(saved_mp3_points);
        return false;
    }
#ifndef HOST_BUILD
    if (was_dop && dec->type == DECODER_DSD) {
        /* The new decoder restarts its markers at 0x05; start a new stream
         * so the DAC never sees the alternation break. */
        audio_output_close();
        if (!decoder_set_dop(dec, true, 0, NULL)) {
            decoder_close(dec);
            return false;
        }
    }
#else
    (void) was_dop;
#endif
    if (saved_mp3_points) {
        if (dec->type != DECODER_MP3 ||
            !mp3_stream_matches(dec->as.mp3, &saved_mp3_stream) ||
            !drmp3_bind_seek_table(dec->as.mp3, saved_mp3_count, saved_mp3_points)) {
            free(saved_mp3_points);
            decoder_close(dec);
            return false;
        }
        dec->mp3_seek_points = saved_mp3_points;
        dec->mp3_seek_point_count = saved_mp3_count;
        dec->mp3_seek_index_attempted = true;
    } else if (dec->type == DECODER_MP3 && saved_mp3_index_attempted) {
        /* No table was ever bound and indexing had already permanently given
         * up before this reopen -- decoder_open() just memset the decoder
         * fresh, losing that state. Restore it purely so the normal
         * playback-loop retry/eager paths don't re-scan a file already
         * known not to be indexable; the seek below does not depend on this
         * flag either way (see the DECODER_MP3 branch just under). */
        dec->mp3_seek_index_attempted = true;
    }
    if (last_confirmed_frame > 0) {
        /* This is an internal recovery/fallback seek (premature-EOF retry,
         * or the caller's own failed-seek reopen), not the interactive
         * playback-loop path -- it must always land now, regardless of
         * whether an index attempt is still outstanding for this decoder.
         * decoder_seek()'s defer-until-indexed gate exists for that other
         * path; bypass it here for MP3 and seek directly, exactly as
         * decoder_seek() itself does once an index attempt has resolved
         * (drmp3_seek_to_pcm_frame() uses a bound table if one exists, or
         * falls back to its own brute-force seek otherwise). Without this,
         * every reopen while a background scan is still running for this
         * track (i.e. saved_mp3_points is NULL and attempted was never set)
         * would be refused outright, since mp3_needs_seek_index() covers
         * ordinary track lengths. */
        bool seek_ok = dec->type == DECODER_MP3 && !dec->segmented
            ? drmp3_seek_to_pcm_frame(dec->as.mp3, last_confirmed_frame) != 0
            : decoder_seek(dec, last_confirmed_frame);
        if (!seek_ok) {
            decoder_close(dec);
            return false;
        }
    }
    return true;
}

/* A DSD decoder passing its bits through as DoP. Its frames must reach the
 * DAC untouched: no ReplayGain, EQ, volume, fade or crossfade. */
bool decoder_is_dop(const decoder_t * dec) {
    return dec && dec->type == DECODER_DSD && dsd_is_dop(dec->as.dsd);
}

#ifndef HOST_BUILD
static uint64_t dsd_scale_frames(uint64_t frames, unsigned from, unsigned to) {
    return frames / from * to + frames % from * to / from;
}

/* Segment bounds and playback position always use the decoder's current
 * frame rate. Convert the absolute source position, then restore relative
 * coordinates so ISO/CUE tracks cannot seek into another track on fallback. */
bool decoder_set_dop(decoder_t *dec, bool dop, uint64_t frame, uint64_t *out_frame) {
    unsigned old_rate = dec->sample_rate;
    uint64_t start = dec->segmented ? dec->segment_start_frame : 0;
    if (!old_rate || frame > UINT64_MAX - start) return false;
    uint64_t converted = start + frame;
    bool ok = dsd_set_dop(dec->as.dsd, dop, start + frame, &converted);
    dec->sample_rate = dsd_get_pcm_sample_rate(dec->as.dsd);
    if (dec->segmented) {
        uint64_t end = start + dec->segment_frame_count;
        dec->segment_start_frame = dsd_scale_frames(start, old_rate, dec->sample_rate);
        dec->segment_frame_count = dsd_scale_frames(end, old_rate, dec->sample_rate) - dec->segment_start_frame;
        converted = converted >= dec->segment_start_frame ? converted - dec->segment_start_frame : 0;
        dec->segment_read_cursor = converted;
        dec->total_frames = dec->segment_frame_count;
    } else dec->total_frames = dsd_get_total_pcm_frame_count(dec->as.dsd);
    if (out_frame) *out_frame = converted;
    return ok;
}
#endif
