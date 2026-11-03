/* The only translation unit holding the dr_libs implementations, plus the two
 * engine functions that use dr_libs internals (static in the implementation). */
#include "audio_dr_config.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "audio_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Faithful single-pass port of dr_mp3's own drmp3_calculate_seek_points()
 * (identical DRMP3_SEEK_LEADING_MP3_FRAMES-based reservoir priming via the
 * same sliding frame_info[] window and drmp3__accumulate_running_pcm_frame_
 * count() sample-rate-conversion accounting it already uses internally --
 * both reused directly rather than reimplemented, since dr_mp3's own
 * implementation is compiled into this same translation unit) with three
 * differences: (1) it's given total_pcm_frames instead of re-deriving both
 * that AND a separate MP3-frame count via drmp3_get_mp3_and_pcm_frame_
 * count()'s own full-file counting pass, so this reads the file once, not
 * twice; (2) it polls its `cancel` callback once per MP3 frame and
 * returns whatever prefix was already placed, via *out_cancelled, instead of
 * always running to completion; (3) since it only ever runs against
 * mp3_index_worker()'s own throwaway `scan` decoder (never the live playback
 * one), there is no original position to restore on exit -- unlike the
 * general-purpose original, which must reseek back to wherever the caller's
 * decoder started, even on failure. */
drmp3_uint32 mp3_build_seek_points_cancellable(drmp3 * scan, uint64_t total_pcm_frames,
                                                       drmp3_uint32 desired_count,
                                                       drmp3_seek_point * points, bool * out_cancelled,
                                                       bool (*cancel)(void *), void * cancel_user) {
    *out_cancelled = false;
    if (desired_count == 0 || total_pcm_frames == 0) return 0;

    uint64_t pcm_frames_between_points = total_pcm_frames / ((uint64_t) desired_count + 1);
    if (pcm_frames_between_points == 0) return 0;
    if (!drmp3_seek_to_start_of_stream(scan)) return 0;

    drmp3__seeking_mp3_frame_info frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES + 1];
    drmp3_uint64 running_pcm_frame_count = 0;
    float running_fractional = 0.0f;

    for (uint32_t i = 0; i < DRMP3_SEEK_LEADING_MP3_FRAMES + 1; i++) {
        frame_info[i].bytePos = scan->streamCursor - scan->dataSize;
        frame_info[i].pcmFrameIndex = running_pcm_frame_count;
        drmp3_uint32 pcm_in_frame = drmp3_decode_next_frame_ex(scan, NULL, NULL, NULL);
        if (pcm_in_frame == 0) return 0; /* fewer frames than the leading window -- too short to index */
        drmp3__accumulate_running_pcm_frame_count(scan, pcm_in_frame, &running_pcm_frame_count, &running_fractional);
    }

    uint64_t next_target = 0;
    drmp3_uint32 placed = 0;
    while (placed < desired_count) {
        if (cancel(cancel_user)) {
            *out_cancelled = true; return placed;
        }
        next_target += pcm_frames_between_points;

        for (;;) {
            if (next_target < running_pcm_frame_count) {
                points[placed].seekPosInBytes = frame_info[0].bytePos;
                points[placed].pcmFrameIndex = next_target;
                points[placed].mp3FramesToDiscard = DRMP3_SEEK_LEADING_MP3_FRAMES;
                points[placed].pcmFramesToDiscard =
                    (drmp3_uint16) (next_target - frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES - 1].pcmFrameIndex);
                break;
            }
            for (size_t i = 0; i < DRMP3_SEEK_LEADING_MP3_FRAMES; i++) frame_info[i] = frame_info[i + 1];
            frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES].bytePos = scan->streamCursor - scan->dataSize;
            frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES].pcmFrameIndex = running_pcm_frame_count;

            if (cancel(cancel_user)) {
                *out_cancelled = true; return placed;
            }

            drmp3_uint32 pcm_in_frame = drmp3_decode_next_frame_ex(scan, NULL, NULL, NULL);
            if (pcm_in_frame == 0) {
                /* Ran out of frames before reaching this point's exact
                 * target (rounding, or total_pcm_frames from the Xing tag
                 * was too high) -- record a best-effort final point at the
                 * real last decoded position (running_pcm_frame_count), not
                 * next_target, which past this point no longer corresponds
                 * to anywhere actually reachable in the file. Then stop;
                 * there is nothing further to place. Not a cancellation. */
                points[placed].seekPosInBytes = frame_info[0].bytePos;
                points[placed].pcmFrameIndex = running_pcm_frame_count;
                points[placed].mp3FramesToDiscard = DRMP3_SEEK_LEADING_MP3_FRAMES;
                points[placed].pcmFramesToDiscard =
                    (drmp3_uint16) (running_pcm_frame_count - frame_info[DRMP3_SEEK_LEADING_MP3_FRAMES - 1].pcmFrameIndex);
                return placed + 1;
            }
            drmp3__accumulate_running_pcm_frame_count(scan, pcm_in_frame, &running_pcm_frame_count, &running_fractional);
        }
        placed++;
    }
    return placed;
}

/* Local waveform previews can use the compressed file span as an approximate
 * PCM-to-byte map. dr_flac validates each probe by decoding its candidate
 * frame and checking CRC; the returned byte offset is the probe location, not
 * the candidate frame header. At most four interpolated probes tighten both
 * PCM and byte bounds. If those bounds cannot be anchored and verified, use
 * the public decoder seek path. Playback never calls this helper. */
bool waveform_flac_seeded_seek(decoder_t * dec, uint64_t frame,
                               uint64_t file_size,
                               bool (*cancel)(void *), void * user,
                               waveform_seek_anchor_t * anchor) {
    if (cancel && cancel(user)) return false;
#if !defined(DR_FLAC_NO_CRC)
    drflac * flac = dec && dec->type == DECODER_FLAC && !dec->net_stream
        ? dec->as.flac : NULL;
    if (flac && flac->container == drflac_container_native &&
        flac->seekpointCount == 0 &&
        flac->totalPCMFrameCount > frame && frame > 0 &&
        file_size > 1 && flac->firstFLACFramePosInBytes < file_size - 1) {
        uint64_t byte_lo = flac->firstFLACFramePosInBytes;
        uint64_t byte_hi = file_size - 1;
        uint64_t pcm_lo = 0;
        uint64_t pcm_hi = flac->totalPCMFrameCount;
        uint64_t close_enough = (uint64_t)flac->maxBlockSizeInPCMFrames * 2;
        if (close_enough == 0) close_enough = 4096;
        /* Dense samples can reuse the decoder's current frame and move a
         * short distance forward instead of probing the file again. */
        if (frame >= flac->currentPCMFrame &&
            frame - flac->currentPCMFrame <= close_enough) {
            uint64_t distance = frame - flac->currentPCMFrame;
            if (drflac__seek_forward_by_pcm_frames(flac, distance) == distance) {
                uint64_t block_frames = flac->currentFLACFrame.header.blockSizeInPCMFrames;
                uint64_t remaining = flac->currentFLACFrame.pcmFramesRemaining;
                if (block_frames > 0 && remaining <= block_frames) {
                    drflac_uint64 frame_start = 0;
                    drflac__get_pcm_frame_range_of_current_flac_frame(flac, &frame_start, NULL);
                    uint64_t consumed = block_frames - remaining;
                    if (frame_start <= UINT64_MAX - consumed && frame_start + consumed == frame)
                        return !(cancel && cancel(user));
                }
            }
            if (cancel && cancel(user)) return false;
            /* Skipping a CRC-bad frame can advance decoded sample counts
             * without landing at the requested header-based PCM position. */
            anchor->valid = false;
            (void)drflac__seek_to_first_frame(flac);
        }
        if (anchor->valid && frame >= anchor->target &&
            anchor->pcm <= frame && anchor->pcm < pcm_hi &&
            anchor->byte >= byte_lo && anchor->byte < byte_hi) {
            byte_lo = anchor->byte;
            pcm_lo = anchor->pcm;
        }

        for (unsigned attempt = 0; attempt < 4 && byte_lo < byte_hi &&
             pcm_lo <= frame && pcm_hi > frame; ++attempt) {
            if (cancel && cancel(user)) return false;

            /* Bias one maximum block earlier so interpolation can settle
             * on a candidate frame before the exact sample target. */
            uint64_t bias = close_enough / 2;
            uint64_t wanted = frame - pcm_lo > bias ? frame - bias : pcm_lo;
            double fraction = (double)(wanted - pcm_lo) / (double)(pcm_hi - pcm_lo);
            double scaled = (double)(byte_hi - byte_lo) * fraction;
            if (!isfinite(scaled) || scaled < 0.0 ||
                scaled >= (double)(byte_hi - byte_lo)) break;

            uint64_t delta = (uint64_t)scaled;
            if (delta >= byte_hi - byte_lo) break;
            uint64_t probe = byte_lo + delta;
            drflac_uint64 probe_offset = probe;
            if (!drflac__seek_to_approximate_flac_frame_to_byte(
                    flac, probe, byte_lo, byte_hi, &probe_offset)) break;
            if (cancel && cancel(user)) return false;

            uint64_t candidate_pcm = flac->currentPCMFrame;
            if (candidate_pcm <= frame && probe_offset >= byte_lo &&
                probe_offset < byte_hi) {
                anchor->byte = probe_offset;
                anchor->pcm = candidate_pcm;
                anchor->target = frame;
                anchor->valid = true;
            }
            if (candidate_pcm <= frame && frame - candidate_pcm <= close_enough) {
                uint64_t distance = frame - candidate_pcm;
                if (drflac__seek_forward_by_pcm_frames(flac, distance) == distance) {
                    flac->currentPCMFrame = frame;
                    return !(cancel && cancel(user));
                }
                break;
            }
            if (candidate_pcm <= frame) {
                if (candidate_pcm <= pcm_lo || probe_offset <= byte_lo ||
                    probe_offset >= byte_hi) break;
                byte_lo = probe_offset;
                pcm_lo = candidate_pcm;
            } else {
                if (candidate_pcm >= pcm_hi || probe_offset <= byte_lo ||
                    probe_offset >= byte_hi) break;
                byte_hi = probe_offset;
                pcm_hi = candidate_pcm;
            }
        }

        if (cancel && cancel(user)) return false;
        if (byte_lo < byte_hi && pcm_lo <= frame && pcm_hi > frame) {
            drflac_uint64 restored_offset = byte_lo;
            bool anchor_ok = byte_lo == flac->firstFLACFramePosInBytes
                ? drflac__seek_to_first_frame(flac)
                : drflac__seek_to_approximate_flac_frame_to_byte(
                    flac, byte_lo, byte_lo, byte_hi, &restored_offset);
            if (cancel && cancel(user)) return false;
            if (anchor_ok && flac->currentPCMFrame == pcm_lo &&
                restored_offset == byte_lo &&
                drflac__seek_to_pcm_frame__binary_search_internal(
                    flac, frame, byte_lo, byte_hi)) {
                flac->currentPCMFrame = frame;
                return !(cancel && cancel(user));
            }
        }
    }
#else
    (void)file_size;
#endif
    if (cancel && cancel(user)) return false;
    anchor->valid = false;
    bool ok = decoder_seek(dec, frame);
    return ok && !(cancel && cancel(user));
}
