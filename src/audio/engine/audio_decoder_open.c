/* Decoder opening: stream/URL helpers, per-format open dispatch, open policy. */
#include "audio_internal.h"
#include "audio_helpers.h"
#include "../../ui/text/i18n/i18n.h"
#include "../demux/mp4/mp4_demux.h"
#include "../demux/caf/caf_demux.h"
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

/* Most internet-radio URLs have no usable extension (an opaque mount point,
 * or a query string). A recognized path extension or Content-Type still
 * selects a decoder; anything else stays MP3. */
bool is_stream_url(const char * path) {
    return strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0;
}

/* A trailing "#.<ext>" on a stream URL is a local-only format hint (never
 * sent to the server -- http_conn_parse_url() strips it, see that
 * function's own comment): the '#' can't legally appear in a Subsonic/
 * plugin-built URL's own path or query otherwise (this app always
 * percent-encodes it via url_encode()), so a literal one is unambiguously
 * ours. Absent a recognized hint, the path extension and Content-Type are
 * tried next; MP3 stays the default so plain radio URLs work as before. */
static const char * stream_format_hint(const char * url) {
    const char * frag = strrchr(url, '#');
    return frag ? frag + 1 : NULL;
}

/* Final suffix of the URL path. Query and fragment are not part of the path,
 * and a dot in the host is not an extension. */
static bool url_path_ext_is(const char * url, const char * ext) {
    const char * scheme = strstr(url, "://");
    const char * begin = scheme ? scheme + 3 : url;
    const char * end = begin;
    while (*end && *end != '?' && *end != '#') end++;
    const char * slash = NULL;
    for (const char * p = begin; p < end; p++) if (*p == '/') slash = p;
    if (!slash) return false;
    const char * dot = NULL;
    for (const char * p = slash + 1; p < end; p++) if (*p == '.') dot = p;
    if (!dot) return false;
    size_t have = (size_t) (end - dot);
    size_t want = strlen(ext);
    return have == want && strncasecmp(dot, ext, have) == 0;
}

static bool content_type_is(const char * value, const char * mime) {
    size_t n = strlen(mime);
    if (strncasecmp(value, mime, n) != 0) return false;
    char tail = value[n];
    return tail == '\0' || tail == ' ' || tail == '\t';
}

static bool content_type_is_mp4(const char * value) {
    return strncasecmp(value, "audio/mp4", 9) == 0 ||
           strncasecmp(value, "video/mp4", 9) == 0 ||
           strncasecmp(value, "application/mp4", 15) == 0;
}

static size_t stream_read_cb(void * user_data, void * buf, size_t bytes_to_read) {
    return http_stream_read((http_stream_t *) user_data, buf, bytes_to_read);
}

/* FLAC requires onSeek. Live streams retain their old forward-only skip;
 * finite HTTP range streams use the same callback for real byte seeks. */
static drflac_bool32 flac_stream_seek_cb(void * user_data, int offset, drflac_seek_origin origin) {
    http_stream_t * stream = (http_stream_t *)user_data;
    if (http_stream_can_seek(stream)) {
        int whence = origin == DRFLAC_SEEK_SET   ? SEEK_SET
                     : origin == DRFLAC_SEEK_END ? SEEK_END
                                                 : SEEK_CUR;
        return http_stream_seek(stream, offset, whence) ? DRFLAC_TRUE
                                                        : DRFLAC_FALSE;
    }
    if (origin != DRFLAC_SEEK_CUR || offset < 0) return DRFLAC_FALSE;
    uint8_t discard[1024];
    int remaining = offset;
    while (remaining > 0) {
        size_t take = (size_t) remaining < sizeof(discard) ? (size_t) remaining : sizeof(discard);
        if (http_stream_read(stream, discard, take) != take) return DRFLAC_FALSE;
        remaining -= (int) take;
    }
    return DRFLAC_TRUE;
}

static drflac_bool32 flac_stream_tell_cb(void * user_data,
                                         drflac_int64 * cursor) {
    if (!cursor)
        return DRFLAC_FALSE;
    int64_t position = http_stream_tell((http_stream_t *)user_data);
    if (position < 0)
        return DRFLAC_FALSE;
    *cursor = position;
    return DRFLAC_TRUE;
}

static drmp3_bool32 mp3_stream_seek_cb(void * user_data, int offset,
                                       drmp3_seek_origin origin) {
    http_stream_t * stream = (http_stream_t *)user_data;
    if (!http_stream_can_seek(stream))
        return DRMP3_FALSE;
    int whence = origin == DRMP3_SEEK_SET   ? SEEK_SET
                 : origin == DRMP3_SEEK_END ? SEEK_END
                                            : SEEK_CUR;
    return http_stream_seek(stream, offset, whence) ? DRMP3_TRUE : DRMP3_FALSE;
}

/* About one synthetic seek point per this many seconds of finite HTTP MP3. */
#define NET_MP3_SEEK_POINT_SECONDS 10
#define NET_MP3_SEEK_POINTS_MAX 2048

/* dr_mp3 reads until it finds a frame. A mislabelled non-MP3 body has none,
 * so an unbounded scan downloads the whole entity (measured: an 18.5 MB
 * FLAC served as MP3). 256 KiB is one stream ring: a real frame, including
 * after a modest ID3v2 tag that dr_mp3 seeks past, starts inside it. A
 * larger leading tag fails the network open instead of being downloaded.
 * Local files are not capped. */
#define NET_MP3_SYNC_SCAN_BYTES (256u * 1024u)

struct net_mp3_reader {
    http_stream_t * stream;
    /* Bytes dr_mp3 may still pull while searching for a frame. SIZE_MAX once
     * a frame is found, so playback itself is not capped. */
    size_t sync_left;
};

static size_t mp3_net_read(void * user_data, void * buf, size_t bytes_to_read) {
    struct net_mp3_reader * reader = user_data;
    if (reader->sync_left != SIZE_MAX) {
        if (!reader->sync_left || !bytes_to_read) return 0;
        if (bytes_to_read > reader->sync_left) bytes_to_read = reader->sync_left;
    }
    size_t got = http_stream_read(reader->stream, buf, bytes_to_read);
    if (reader->sync_left != SIZE_MAX) {
        reader->sync_left -= got;
    }
    return got;
}

static drmp3_bool32 mp3_net_seek(void * user_data, int offset, drmp3_seek_origin origin) {
    return mp3_stream_seek_cb(((struct net_mp3_reader *) user_data)->stream, offset, origin);
}

/* Finite HTTP MP3 gets no background seek index. Without a table dr_mp3
 * seeks by decoding from the first frame, which over the network blocks the
 * audio thread for as long as the target is far into the file. Points spread
 * linearly over the audio bytes make a seek one range request plus at most
 * a point's worth of decoding. Positions are exact for CBR and estimates
 * for VBR. Each point after the first discards one frame because the
 * decoder starts cold there (bit reservoir). */
static bool bind_network_mp3_seek_table(decoder_t * dec) {
    int64_t length = http_stream_length(dec->net_stream);
    uint64_t start = dec->as.mp3->streamStartOffset;
    if (length <= 0 || (uint64_t)length <= start || dec->total_frames == 0 || dec->sample_rate == 0)
        return false;
    uint64_t span = (uint64_t)length - start;
    uint64_t count = dec->total_frames / dec->sample_rate / NET_MP3_SEEK_POINT_SECONDS + 1;
    if (count > NET_MP3_SEEK_POINTS_MAX)
        count = NET_MP3_SEEK_POINTS_MAX;
    drmp3_seek_point * points = calloc((size_t)count, sizeof(*points));
    if (!points)
        return false;
    for (uint64_t i = 0; i < count; i++) {
        double fraction = (double)i / (double)count;
        points[i].seekPosInBytes = start + (uint64_t)((double)span * fraction);
        points[i].pcmFrameIndex = (uint64_t)((double)dec->total_frames * fraction);
        points[i].mp3FramesToDiscard = i ? 1 : 0;
    }
    if (!drmp3_bind_seek_table(dec->as.mp3, (drmp3_uint32)count, points)) {
        free(points);
        return false;
    }
    dec->mp3_seek_points = points;
    dec->mp3_seek_point_count = (drmp3_uint32)count;
    dec->mp3_seek_index_attempted = true;
    return true;
}

/* Use relaxed native-container parsing for local FLAC files. This tolerates
 * damaged metadata and lets dr_flac skip malformed frames in otherwise
 * playable files, as desktop decoders do. */
static size_t flac_file_read_cb(void * user_data, void * buffer_out, size_t bytes_to_read) {
    return fread(buffer_out, 1, bytes_to_read, (FILE *) user_data);
}

/* Remote current-open policy needs a generation/path snapshot; definitions
 * live with the rest of playback state in audio.c. */

static drflac_bool32 flac_file_seek_cb(void * user_data, int offset, drflac_seek_origin origin) {
    int whence = origin == DRFLAC_SEEK_SET ? SEEK_SET
               : origin == DRFLAC_SEEK_END ? SEEK_END : SEEK_CUR;
    return fseek((FILE *) user_data, (long) offset, whence) == 0 ? DRFLAC_TRUE : DRFLAC_FALSE;
}

/* .alac is used for both MP4 and CAF files; sniff the container signature. */
static bool file_is_caf(const char * path) {
    FILE * file = fopen(path, "rb");
    if (!file) return false;
    char magic[4];
    bool matches = fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
                   memcmp(magic, "caff", sizeof(magic)) == 0;
    fclose(file);
    return matches;
}

static bool open_alac_file(decoder_t * dec, const char * path) {
    dec->type = DECODER_ALAC;
    dec->as.alac = alac_open_file(path);
    if (!dec->as.alac) return false;
    dec->channels = alac_get_channels(dec->as.alac);
    dec->sample_rate = alac_get_sample_rate(dec->as.alac);
    dec->source_sample_rate = dec->sample_rate;
    dec->source_bit_depth = alac_get_bit_depth(dec->as.alac);
    dec->total_frames = alac_get_total_pcm_frame_count(dec->as.alac);
    return true;
}

static bool open_opus_file(decoder_t * dec, const char * path) {
    dec->type = DECODER_OPUS;
    dec->as.opus = opus_open_file(path);
    if (!dec->as.opus) return false;
    dec->channels = opus_get_channels(dec->as.opus);
    dec->sample_rate = opus_get_sample_rate(dec->as.opus);
    dec->source_sample_rate = dec->sample_rate;
    dec->total_frames = opus_get_total_pcm_frame_count(dec->as.opus);
    return true;
}

/* Releases the network stream of a failed open; always returns false. */
static bool net_open_fail(decoder_t * dec) {
    http_stream_close(dec->net_stream);
    dec->net_stream = NULL;
    return false;
}

typedef enum { NET_FMT_MP3, NET_FMT_AAC, NET_FMT_MP4, NET_FMT_FLAC } net_format_t;

static bool net_resolve_format(decoder_t * dec, const char * path, bool remote,
                               const remote_track_meta_t * m, net_format_t * out) {
    /* Fragment, then path extension, then Content-Type. A fragment or
     * extension that names a container wins over a later Content-Type so
     * a .flac URL is not opened as MP3. .mp3 and unknown suffixes (a
     * .m3u playlist URL, an opaque mount) are not specific and still
     * fall through, with MP3 as the default. */
    const char * hint = remote ? NULL : stream_format_hint(path);
    bool fragment_flac = hint && strcasecmp(hint, ".flac") == 0;
    bool fragment_aac = hint && (strcasecmp(hint, ".aac") == 0 || strcasecmp(hint, ".aacp") == 0);
    bool fragment_mp4 = hint && (strcasecmp(hint, ".m4a") == 0 || strcasecmp(hint, ".m4b") == 0 ||
                                 strcasecmp(hint, ".mp4") == 0);
    bool fragment_specific = fragment_flac || fragment_aac || fragment_mp4;
    bool path_flac = !remote && !fragment_specific && url_path_ext_is(path, ".flac");
    bool path_aac = !remote && !fragment_specific &&
                    (url_path_ext_is(path, ".aac") || url_path_ext_is(path, ".aacp"));
    bool path_mp4 = !remote && !fragment_specific &&
                    (url_path_ext_is(path, ".m4a") || url_path_ext_is(path, ".m4b") ||
                     url_path_ext_is(path, ".mp4"));
    bool path_specific = path_flac || path_aac || path_mp4;
    bool is_flac = remote ? (strcasecmp(m->codec, "flac") == 0) : (fragment_flac || path_flac);
    bool is_aac = remote ? (strcasecmp(m->codec, "aac") == 0) : (fragment_aac || path_aac);
    bool is_mp4 = !remote && (fragment_mp4 || path_mp4);

    const char * content_type = http_stream_content_type(dec->net_stream);
    if (remote) {
        /* An MP4 media type still selects the container when the plugin
         * named a different codec. audio/aac does not, matching before. */
        is_mp4 = content_type_is_mp4(content_type);
    } else if (!fragment_specific && !path_specific) {
        if (strncasecmp(content_type, "audio/aac", 9) == 0) is_aac = true;
        else if (content_type_is(content_type, "audio/flac") ||
                 content_type_is(content_type, "audio/x-flac")) is_flac = true;
        else if (content_type_is_mp4(content_type)) is_mp4 = true;
    }
    dec->seekable = http_stream_can_seek(dec->net_stream);

    /* Providers sometimes describe AAC without a useful media type (or
     * extension). For a finite byte-range stream, sniff only the MP4
     * file-type box; this is a constant 12-byte read with a rewind, not
     * an ADTS index scan. Live AAC never takes this path. */
    if (!is_mp4 && is_aac && dec->seekable) {
        FILE * probe = http_stream_open_file(dec->net_stream);
        if (probe) {
            int64_t probe_start = http_stream_tell(dec->net_stream);
            unsigned char header[12];
            is_mp4 = fread(header, 1, sizeof(header), probe) == sizeof(header) &&
                     memcmp(header + 4, "ftyp", 4) == 0;
            bool rewound = probe_start >= 0 &&
                           http_stream_seek(dec->net_stream, probe_start, SEEK_SET);
            fclose(probe);
            if (!rewound) {
                return false;
            }
        }
    }

    *out = is_mp4 ? NET_FMT_MP4 : is_aac ? NET_FMT_AAC : is_flac ? NET_FMT_FLAC : NET_FMT_MP3;
    return true;
}

static bool decoder_take_aac(decoder_t * dec, aac_decoder_t * a) {
    dec->type = DECODER_AAC;
    dec->as.aac = a;
    if (!a) return false;
    dec->channels = aac_get_channels(a);
    dec->sample_rate = aac_get_sample_rate(a);
    dec->source_sample_rate = dec->sample_rate;
    return true;
}

static uint64_t remote_declared_frames(const remote_track_meta_t * m, unsigned rate) {
    return (uint64_t) ((double) m->duration_ms / 1000.0 * rate);
}

/* Finite AAC-in-MP4 streams use a seekable stdio view so the existing
 * MP4 demuxer can read its sample table and issue HTTP byte-range
 * seeks. Raw ADTS AAC retains the incremental stream decoder: its
 * frame indexer scans a whole input and would make arbitrary remote
 * episodes expensive to open. aac_open_mp4_file_stream() rejects a
 * non-mp4a moov, so a separate peek would parse it twice. */
static bool open_network_mp4(decoder_t * dec, bool remote, const remote_track_meta_t * m) {
    if (!dec->seekable) {
        return net_open_fail(dec);
    }
    FILE * probe = http_stream_open_file(dec->net_stream);
    uint8_t * cached = NULL;
    mp4_span_t * mp4_spans = NULL;
    size_t span_count = 0;
    bool have_moov = probe && mp4_cache_moov(probe, &cached, &mp4_spans, &span_count);
    if (probe) fclose(probe);
    http_span_t * spans = span_count ? malloc(span_count * sizeof(*spans)) : NULL;
    for (size_t i = 0; spans && i < span_count; i++)
        spans[i] = (http_span_t) { mp4_spans[i].offset, mp4_spans[i].length };
    free(mp4_spans);
    if (span_count && !spans) {
        free(cached);
        have_moov = false;
    }
    FILE * file = have_moov ? http_stream_open_spans_file(dec->net_stream, cached, spans, span_count) : NULL;
    if (!file) {
        return net_open_fail(dec);
    }
    if (!decoder_take_aac(dec, aac_open_mp4_file_stream(file))) { /* takes FILE ownership */
        return net_open_fail(dec);
    }
    dec->source_bit_depth = remote ? m->bit_depth : 0;
    dec->bitrate_kbps = remote ? m->bitrate_kbps : 0;
    dec->total_frames = aac_get_total_pcm_frame_count(dec->as.aac);
    return true;
}

static bool open_network_aac(decoder_t * dec, bool remote, const remote_track_meta_t * m) {
    /* AAC_SOURCE_STREAM decodes incrementally but has no compressed
     * frame index. Do not advertise seeking merely because HTTP
     * happens to support byte ranges; raw ADTS indexing remains an
     * explicit local-file path with its existing full scan. */
    dec->seekable = false;
    if (!decoder_take_aac(dec, aac_open_stream(stream_read_cb, dec->net_stream))) {
        return net_open_fail(dec);
    }
    dec->source_bit_depth = remote ? m->bit_depth : 0;
    dec->bitrate_kbps = remote ? m->bitrate_kbps : 0;
    /* A remote track declares its own duration up front (the
     * plugin's catalog metadata) -- unlike plain internet radio,
     * there's no need to leave total_frames at 0 (unknown duration);
     * a plain stream URL (remote == false) still has no
     * such source and keeps today's behavior exactly. */
    dec->total_frames = (remote && m->duration_ms > 0)
                             ? remote_declared_frames(m, dec->sample_rate)
                             : 0;
    return true;
}

static bool open_network_flac(decoder_t * dec, bool remote, const remote_track_meta_t * m) {
    dec->type = DECODER_FLAC;
    dec->as.flac = drflac_open(stream_read_cb, flac_stream_seek_cb,
                               dec->seekable ? flac_stream_tell_cb : NULL, dec->net_stream, NULL);
    if (!dec->as.flac) {
        return net_open_fail(dec);
    }
    dec->channels = dec->as.flac->channels;
    dec->sample_rate = dec->as.flac->sampleRate;
    dec->source_sample_rate = dec->sample_rate;
    dec->source_bit_depth = dec->as.flac->bitsPerSample;
    dec->bitrate_kbps = remote ? m->bitrate_kbps : 0;
    /* Unlike MP3, this is a REAL, authoritative count straight from
     * the STREAMINFO metadata block (near the top of the file, not
     * a full-stream prescan) -- safe to use for real, matching
     * local FLAC playback. Gives a Subsonic FLAC stream a
     * correct duration/progress bar for free, unlike MP3 streams
     * (which have no equivalent authoritative source without a
     * full prescan or a declared remote duration). */
    dec->total_frames = dec->as.flac->totalPCMFrameCount;
    return true;
}

static bool open_network_mp3(decoder_t * dec, bool remote, const remote_track_meta_t * m) {
    dec->type = DECODER_MP3;
    dec->as.mp3 = malloc(sizeof(drmp3));
    dec->net_mp3 = malloc(sizeof(*dec->net_mp3));
    if (dec->net_mp3) {
        dec->net_mp3->stream = dec->net_stream;
        dec->net_mp3->sync_left = NET_MP3_SYNC_SCAN_BYTES;
    }
    /* No onTell: dr_mp3 uses it only to look for ID3v1/APE tags at the
     * end, which over HTTP costs two extra connections per open. The
     * frame scanner skips a trailing tag's bytes anyway. */
    if (!dec->as.mp3 || !dec->net_mp3 ||
        !drmp3_init(dec->as.mp3, mp3_net_read, dec->seekable ? mp3_net_seek : NULL,
                    NULL, NULL, dec->net_mp3, NULL)) {
        free(dec->as.mp3);
        dec->as.mp3 = NULL;
        free(dec->net_mp3);
        dec->net_mp3 = NULL;
        return net_open_fail(dec);
    }
    dec->net_mp3->sync_left = SIZE_MAX;
    dec->channels = dec->as.mp3->channels;
    dec->sample_rate = dec->as.mp3->sampleRate;
    dec->source_sample_rate = dec->sample_rate;
    dec->source_bit_depth = remote ? m->bit_depth : 0;
    dec->bitrate_kbps = remote ? m->bitrate_kbps : 0;
    if (dec->as.mp3->totalPCMFrameCount != DRMP3_UINT64_MAX)
        dec->total_frames = dec->as.mp3->totalPCMFrameCount;
    /* See open_network_aac() -- a remote track's
     * declared duration_ms substitutes for the prescan this never does
     * on a live stream; a plain MP3 stream URL (remote == false, e.g.
     * internet radio or a Subsonic stream) has no such source and
     * keeps today's unknown-duration-until-EOF behavior exactly. */
    if (remote && m->duration_ms > 0)
        dec->total_frames = remote_declared_frames(m, dec->sample_rate);
    /* Without a table a seek would decode from the start over the
     * network, so such a stream is reported as not seekable. */
    if (dec->seekable && !bind_network_mp3_seek_table(dec))
        dec->seekable = false;
    return true;
}

static bool decoder_open_internal(decoder_t * dec, const char * path,
                                  const remote_track_meta_t * remote_snapshot) {
    memset(dec, 0, sizeof(*dec));
    dec->open_error = AUDIO_ERROR_DECODER_FAILED;

    /* A remote-provider track (plugin.play_remote(), see remote_track.h):
     * path is the stable "remote://<provider>/<track_id>" synthetic key
     * used for Favorites/History/resume, never a fetchable URL itself --
     * the real, possibly time-limited fetch URL and verify_tls live in the
     * looked-up descriptor. Its codec is declared up front by the plugin
     * (already known from the catalog API, no server round trip needed to
     * find out), so this skips the extension/fragment-hint/Content-Type
     * sniffing below entirely -- that sniffing stays exactly as it was for
     * radio/Subsonic streams, which don't have a declared codec. */
    remote_track_meta_t remote_meta;
    bool remote = false;
    if (remote_track_path_is_remote(path)) {
        if (remote_snapshot) {
            remote_meta = *remote_snapshot;
            remote = true;
        } else {
            remote = remote_track_meta_copy_for_path(path, &remote_meta);
        }
    }
    if (remote_track_path_is_remote(path) && !remote) return false; /* stale/replaced queue entry */

    if (remote || is_stream_url(path)) {
        /* Live network stream -- MP3/FLAC use their callback-based decoder
         * APIs, while ADTS AAC uses aac_open_stream()'s incremental framing
         * path. FLAC's callback seek constraints are documented above;
         * dr_mp3 tolerates NULL onSeek/onTell, and live AAC never seeks or
         * prescans. Other formats still require a finite file/container.
         * Absent a recognized hint, path extension, or audio Content-Type,
         * MP3 remains the default for ordinary internet-radio URLs. */
        dec->net_stream = http_stream_open(remote ? remote_meta.stream_url : path, remote ? remote_meta.verify_tls : true);
        if (!dec->net_stream) return false;

        net_format_t format;
        if (!net_resolve_format(dec, path, remote, &remote_meta, &format))
            return net_open_fail(dec);
        switch (format) {
            case NET_FMT_MP4: return open_network_mp4(dec, remote, &remote_meta);
            case NET_FMT_AAC: return open_network_aac(dec, remote, &remote_meta);
            case NET_FMT_FLAC: return open_network_flac(dec, remote, &remote_meta);
            case NET_FMT_MP3: return open_network_mp3(dec, remote, &remote_meta);
        }
        return net_open_fail(dec);
    }

    const char * ext = strrchr(path, '.');
    if (!ext) { dec->open_error = AUDIO_ERROR_UNSUPPORTED_FORMAT; return false; }

    if (strcasecmp(ext, ".flac") == 0) {
        dec->type = DECODER_FLAC;
        dec->flac_file = fopen(path, "rb");
        if (!dec->flac_file) return false;
        dec->as.flac = drflac_open_relaxed(flac_file_read_cb, flac_file_seek_cb,
                                           NULL, drflac_container_native,
                                           dec->flac_file, NULL);
        if (!dec->as.flac) {
            fclose(dec->flac_file);
            dec->flac_file = NULL;
            return false;
        }
        dec->channels = dec->as.flac->channels;
        dec->sample_rate = dec->as.flac->sampleRate;
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = dec->as.flac->bitsPerSample;
        dec->total_frames = dec->as.flac->totalPCMFrameCount;
        return true;
    }

    if (strcasecmp(ext, ".mp3") == 0) {
        dec->type = DECODER_MP3;
        dec->as.mp3 = malloc(sizeof(drmp3));
        if (!dec->as.mp3 || !drmp3_init_file(dec->as.mp3, path, NULL)) {
            free(dec->as.mp3);
            dec->as.mp3 = NULL;
            return false;
        }
        dec->channels = dec->as.mp3->channels;
        dec->sample_rate = dec->as.mp3->sampleRate;
        dec->source_sample_rate = dec->sample_rate;
        dec->total_frames = drmp3_get_pcm_frame_count(dec->as.mp3);
        return true;
    }

    if (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".rf64") == 0 ||
        strcasecmp(ext, ".w64") == 0) {
        dec->type = DECODER_WAV;
        dec->as.wav = malloc(sizeof(drwav));
        if (!dec->as.wav || !drwav_init_file(dec->as.wav, path, NULL)) {
            free(dec->as.wav);
            dec->as.wav = NULL;
            return false;
        }
        dec->channels = dec->as.wav->channels;
        dec->sample_rate = dec->as.wav->sampleRate;
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = dec->as.wav->bitsPerSample;
        dec->total_frames = dec->as.wav->totalPCMFrameCount;
        return true;
    }

    if (strcasecmp(ext, ".aiff") == 0 || strcasecmp(ext, ".aif") == 0 ||
        strcasecmp(ext, ".aifc") == 0) {
        dec->type = DECODER_AIFF;
        dec->as.aiff = aiff_open_file(path);
        if (!dec->as.aiff) return false;
        dec->channels = aiff_get_channels(dec->as.aiff);
        dec->sample_rate = aiff_get_sample_rate(dec->as.aiff);
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = aiff_get_bits_per_sample(dec->as.aiff);
        dec->total_frames = aiff_get_total_pcm_frame_count(dec->as.aiff);
        return true;
    }

    /* Compressed DSD must use the same DoP/fallback pipeline as DSF/DFF.
     * Ordinary PCM WavPack falls through to its existing decoder below. */
    if (strcasecmp(ext, ".dsf") == 0 || strcasecmp(ext, ".dff") == 0 || strcasecmp(ext, ".iso") == 0 ||
        strcasecmp(ext, ".wv") == 0) {
        dec->type = DECODER_DSD;
        dec->as.dsd = dsd_open_file(path);
        if (!dec->as.dsd) {
            if (strcasecmp(ext, ".wv") != 0) return false;
        } else {
            dec->channels = dsd_get_channels(dec->as.dsd);
            dec->sample_rate = dsd_get_pcm_sample_rate(dec->as.dsd); /* decimated PCM rate, not the raw DSD rate */
            dec->source_sample_rate = dsd_get_source_sample_rate(dec->as.dsd);
            dec->source_bit_depth = 1;
            dec->total_frames = dsd_get_total_pcm_frame_count(dec->as.dsd);
            return true;
        }
    }

    if (strcasecmp(ext, ".aac") == 0) {
        if (!decoder_take_aac(dec, aac_open_file(path))) return false;
        dec->total_frames = aac_get_total_pcm_frame_count(dec->as.aac);
        return true;
    }

    if (strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".m4b") == 0 ||
        strcasecmp(ext, ".mp4") == 0 ||
        (strcasecmp(ext, ".alac") == 0 && !file_is_caf(path))) {
        /* .m4a/.m4b is a container, not a codec -- peek which one is actually
         * inside (ALAC or AAC) before picking a decoder. The real decoders
         * each open their own mp4_demux_t; this one is just for the peek. */
        char fourcc[5];
        errno = 0;
        if (!mp4_demux_peek_codec(path, fourcc)) {
            /* A read or open failure is not a verdict on the file (see the probe). */
            dec->open_io_error = errno != 0;
            return false;
        }

        if (strcmp(fourcc, "alac") == 0) return open_alac_file(dec, path);
        if (strcmp(fourcc, "mp4a") == 0) {
            if (!decoder_take_aac(dec, aac_open_file_mp4(path))) return false;
            dec->total_frames = aac_get_total_pcm_frame_count(dec->as.aac);
            return true;
        }
        dec->open_error = AUDIO_ERROR_UNSUPPORTED_FORMAT;
        return false;
    }

    if (strcasecmp(ext, ".wv") == 0) {
        dec->type = DECODER_WAVPACK;
        dec->as.wavpack = wavpack_open_file(path);
        if (!dec->as.wavpack) return false;
        dec->channels = wavpack_get_channels(dec->as.wavpack);
        dec->sample_rate = wavpack_get_sample_rate(dec->as.wavpack);
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = wavpack_get_bits_per_sample(dec->as.wavpack);
        dec->total_frames = wavpack_get_total_pcm_frame_count(dec->as.wavpack);
        return true;
    }

    if (strcasecmp(ext, ".caf") == 0 || strcasecmp(ext, ".alac") == 0) {
        /* ALAC inside CAF goes to the native ALAC decoder; PCM/float stays here. */
        char fourcc[5];
        if (caf_demux_peek_codec(path, fourcc) && strcmp(fourcc, "alac") == 0)
            return open_alac_file(dec, path);
        dec->type = DECODER_CAF;
        dec->as.caf = caf_open_file(path);
        if (!dec->as.caf) return false;
        dec->channels = caf_get_channels(dec->as.caf);
        dec->sample_rate = caf_get_sample_rate(dec->as.caf);
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = caf_get_bits_per_sample(dec->as.caf);
        dec->total_frames = caf_get_total_pcm_frame_count(dec->as.caf);
        return true;
    }

    if (strcasecmp(ext, ".ape") == 0) {
        dec->type = DECODER_APE;
        dec->as.ape = ape_open_file(path);
        if (!dec->as.ape) return false;
        dec->channels = ape_get_channels(dec->as.ape);
        dec->sample_rate = ape_get_sample_rate(dec->as.ape);
        dec->source_sample_rate = dec->sample_rate;
        dec->source_bit_depth = ape_get_bits_per_sample(dec->as.ape);
        dec->total_frames = ape_get_total_pcm_frame_count(dec->as.ape);
        return true;
    }

    if (strcasecmp(ext, ".wma") == 0) {
        dec->type = DECODER_WMA;
        dec->as.wma = wma_open_file(path);
        if (!dec->as.wma) return false;
        dec->channels = wma_get_channels(dec->as.wma);
        dec->sample_rate = wma_get_sample_rate(dec->as.wma);
        dec->source_sample_rate = dec->sample_rate;
        dec->total_frames = wma_get_total_pcm_frame_count(dec->as.wma);
        return true;
    }

    if (strcasecmp(ext, ".opus") == 0) return open_opus_file(dec, path);

    if (strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext, ".oga") == 0) {
        ogg_codec_t codec = ogg_detect_codec(path);
        if (codec == OGG_CODEC_OPUS) return open_opus_file(dec, path);
        if (codec == OGG_CODEC_VORBIS) {
            dec->type = DECODER_VORBIS;
            dec->as.vorbis = vorbis_open_file(path);
            if (!dec->as.vorbis) return false;
            dec->channels = vorbis_get_channels(dec->as.vorbis);
            dec->sample_rate = vorbis_get_sample_rate(dec->as.vorbis);
            dec->source_sample_rate = dec->sample_rate;
            dec->total_frames = vorbis_get_total_pcm_frame_count(dec->as.vorbis);
            return true;
        }
        /* A file that could not be read is not a rejected format. */
        dec->open_io_error = codec == OGG_CODEC_IO_ERROR;
        dec->open_error = AUDIO_ERROR_UNSUPPORTED_FORMAT;
        return false;
    }

    dec->open_error = AUDIO_ERROR_UNSUPPORTED_FORMAT;
    return false;
}

const char * audio_error_description(audio_error_t error) {
    switch (error) {
        case AUDIO_ERROR_FILE_UNAVAILABLE: return N_("File unavailable");
        case AUDIO_ERROR_UNSUPPORTED_FORMAT: return N_("Unsupported audio format");
        case AUDIO_ERROR_UNSUPPORTED_CHANNELS: return N_("Too many audio channels");
        case AUDIO_ERROR_OUTPUT_FAILED: return N_("Audio output failed");
        case AUDIO_ERROR_DECODER_FAILED: return N_("Audio could not be decoded");
        default: return N_("Playback error");
    }
}

/* Every decoder feeds the same eight-channel playback buffers. Check the
 * decoded format before any playback/probe client can read into them. */
static bool decoder_format_is_safe(decoder_t * dec) {
    if (dec->channels > MAX_CHANNELS) {
        dec->open_error = AUDIO_ERROR_UNSUPPORTED_CHANNELS;
        return false;
    }
    if (dec->channels == 0 || dec->sample_rate == 0) {
        dec->open_error = AUDIO_ERROR_DECODER_FAILED;
        return false;
    }
    return true;
}

static bool cue_seconds_to_frame(double seconds, unsigned int sample_rate, uint64_t * frame_out) {
    if (!isfinite(seconds) || seconds < 0.0 || sample_rate == 0 || !frame_out) return false;
    double frames = seconds * (double) sample_rate;
    /* UINT64_MAX rounds to 2^64 as a double; reject that boundary before
     * the cast so out-of-range floating-to-integer conversion is avoided. */
    if (!isfinite(frames) || frames >= 0x1p64) return false;
    *frame_out = (uint64_t) floor(frames + 0.5);
    return true;
}

static bool decoder_open_policy(decoder_t * dec, const char * path, bool wait_for_remote,
                                uint64_t generation, bool generation_is_explicit) {
    if (!path || !path[0]) {
        memset(dec, 0, sizeof(*dec));
        dec->open_error = AUDIO_ERROR_FILE_UNAVAILABLE;
        return false;
    }
    const char * open_path = path;
    remote_track_meta_t remote_snapshot;
    remote_track_meta_t * snapshot = NULL;
    if (remote_track_path_is_remote(path)) {
        uint64_t remote_generation = generation;
        bool active = false;
        pthread_mutex_lock(&audio_mutex);
        if (!generation_is_explicit) remote_generation = playback_generation;
        active = playback_generation == remote_generation && active_path && strcmp(active_path, path) == 0;
        pthread_mutex_unlock(&audio_mutex);
        if (wait_for_remote) {
            if (!active) {
                memset(dec, 0, sizeof(*dec));
                dec->open_error = AUDIO_ERROR_FILE_UNAVAILABLE;
                return false;
            }
            if (!remote_track_stream_wait(path, remote_generation, &remote_snapshot)) {
                memset(dec, 0, sizeof(*dec));
                dec->open_error = AUDIO_ERROR_FILE_UNAVAILABLE;
                return false;
            }
            pthread_mutex_lock(&audio_mutex);
            active = playback_generation == remote_generation && active_path && strcmp(active_path, path) == 0;
            pthread_mutex_unlock(&audio_mutex);
            if (!active) {
                memset(dec, 0, sizeof(*dec));
                dec->open_error = AUDIO_ERROR_FILE_UNAVAILABLE;
                return false;
            }
        } else if (!remote_track_meta_copy_fresh_for_path(path, &remote_snapshot)) {
            memset(dec, 0, sizeof(*dec));
            dec->open_error = AUDIO_ERROR_FILE_UNAVAILABLE;
            return false;
        }
        snapshot = &remote_snapshot;
    }
    char cue_audio_path[600];
    bool cue_reference = cue_reference_is(path);
    uint64_t cue_start_frame = 0;
    uint64_t cue_end_frame = 0;
    double cue_start_seconds = 0.0;
    double cue_end_seconds = -1.0;
    if (cue_reference) {
        cue_sheet_t sheet = {0};
        unsigned ordinal = 0;
        if (!cue_reference_resolve(path, &sheet, &ordinal) ||
            ordinal == 0 || ordinal > (unsigned) sheet.track_count) {
            cue_sheet_free(&sheet);
            memset(dec, 0, sizeof(*dec));
            dec->open_error = AUDIO_ERROR_DECODER_FAILED;
            return false;
        }
        const cue_track_t * track = &sheet.tracks[ordinal - 1];
        memcpy(cue_audio_path, track->audio_path, sizeof(cue_audio_path));
        cue_audio_path[sizeof(cue_audio_path) - 1] = '\0';
        cue_start_seconds = track->start_seconds;
        cue_end_seconds = track->end_seconds;
        cue_sheet_free(&sheet);
        open_path = cue_audio_path;
        if (!isfinite(cue_start_seconds) || cue_start_seconds < 0.0 ||
            (cue_end_seconds != -1.0 &&
             (!isfinite(cue_end_seconds) || cue_end_seconds <= cue_start_seconds))) {
            memset(dec, 0, sizeof(*dec));
            dec->open_error = AUDIO_ERROR_DECODER_FAILED;
            return false;
        }
    }
    errno = 0;
    if (!decoder_open_internal(dec, open_path, snapshot)) {
        int saved_errno = errno;
        if (!is_stream_url(path) && !remote_track_path_is_remote(path) &&
            (saved_errno == ENOENT || saved_errno == ENOTDIR || saved_errno == EACCES))
            dec->open_error = AUDIO_ERROR_FILE_UNAVAILABLE;
        DBG_LOG("audio: open rejected (%s): %s\n",
                safe_path_tail(path), audio_error_description(dec->open_error));
        return false;
    }
    if (!decoder_format_is_safe(dec)) {
        DBG_LOG("audio: unsafe format rejected (%s): %u channels, %u Hz\n",
                safe_path_tail(path), dec->channels, dec->sample_rate);
        decoder_close(dec);
        return false;
    }
    if (!dec->net_stream)
        dec->seekable = true;
    if (cue_reference) {
        if (!dec->seekable || dec->total_frames == 0 ||
            !cue_seconds_to_frame(cue_start_seconds, dec->sample_rate, &cue_start_frame) ||
            (cue_end_seconds != -1.0 && !cue_seconds_to_frame(cue_end_seconds, dec->sample_rate, &cue_end_frame)) ||
            cue_start_frame >= dec->total_frames) {
            decoder_close(dec);
            dec->open_error = AUDIO_ERROR_DECODER_FAILED;
            return false;
        }
        if (cue_end_seconds == -1.0 || cue_end_frame > dec->total_frames)
            cue_end_frame = dec->total_frames;
        if (cue_end_frame <= cue_start_frame) {
            decoder_close(dec);
            dec->open_error = AUDIO_ERROR_DECODER_FAILED;
            return false;
        }
        dec->segmented = true;
        dec->segment_start_frame = cue_start_frame;
        dec->segment_frame_count = cue_end_frame - cue_start_frame;
        dec->segment_read_cursor = 0;
        dec->total_frames = dec->segment_frame_count;
        if (!decoder_seek_raw(dec, cue_start_frame)) {
            decoder_close(dec);
            dec->open_error = AUDIO_ERROR_DECODER_FAILED;
            return false;
        }
    }
    dec->open_error = AUDIO_ERROR_NONE;
    return true;
}

bool decoder_open(decoder_t * dec, const char * path) {
    return decoder_open_policy(dec, path, true, 0, false);
}

bool decoder_open_for_generation(decoder_t * dec, const char * path, uint64_t generation) {
    return decoder_open_policy(dec, path, true, generation, true);
}

/* NEXT decoders are opened on the output thread while the current track is
 * playing. They may only consume a fresh URL; resolving here would stall audio. */
bool decoder_open_next(decoder_t * dec, const char * path) {
    return decoder_open_policy(dec, path, false, 0, false);
}

audio_codec_t public_codec_for_decoder(const decoder_t * dec) {
    switch (dec->type) {
        case DECODER_WAVPACK: return AUDIO_CODEC_WAVPACK;
        case DECODER_CAF: return AUDIO_CODEC_PCM;
        case DECODER_FLAC: return AUDIO_CODEC_FLAC;
        case DECODER_MP3: return AUDIO_CODEC_MP3;
        case DECODER_WAV:
        case DECODER_AIFF: return AUDIO_CODEC_PCM;
        case DECODER_DSD: return AUDIO_CODEC_DSD;
        case DECODER_AAC: return AUDIO_CODEC_AAC;
        case DECODER_ALAC: return AUDIO_CODEC_ALAC;
        case DECODER_APE: return AUDIO_CODEC_APE;
        case DECODER_WMA: return AUDIO_CODEC_WMA;
        case DECODER_OPUS: return AUDIO_CODEC_OPUS;
        case DECODER_VORBIS: return AUDIO_CODEC_VORBIS;
    }
    return AUDIO_CODEC_UNKNOWN;
}

bool audio_probe_file_format(const char * path, audio_current_format_info_t * out) {
    return audio_probe_file_format_status(path, out, NULL);
}

static bool probe_path_is_mp4_family(const char * path) {
    const char * ext = path ? strrchr(path, '.') : NULL;
    return ext && (strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".m4b") == 0 || strcasecmp(ext, ".mp4") == 0);
}

bool audio_probe_file_format_status(const char * path, audio_current_format_info_t * out,
                                    audio_probe_status_t * status) {
    audio_probe_status_t local_status;
    if (!status) status = &local_status;
    *status = AUDIO_PROBE_ERROR;
    if (!path || !path[0] || !out || is_stream_url(path) || remote_track_path_is_remote(path)) return false;

    /* APE open decodes its first complete physical frame to validate decoder
     * state and allocates frame-sized working buffers. A format probe only
     * needs the already-validated container/header facts; avoid that heavy
     * first-frame path on the GUI/library-probe workers. */
    const char * ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".ape") == 0) {
        ape_demux_t * demux = ape_demux_open(path);
        if (!demux) return false;

        int compression = ape_demux_get_compression_level(demux);
        unsigned int channels = ape_demux_get_channels(demux);
        unsigned int sample_rate = ape_demux_get_sample_rate(demux);
        unsigned int bit_depth = ape_demux_get_bits_per_sample(demux);
        uint64_t total_samples = ape_demux_get_total_samples(demux);
        bool supported = compression >= 1000 && compression <= 5000 && compression % 1000 == 0 &&
                         channels >= 1 && channels <= 2 && sample_rate > 0 &&
                         (bit_depth == 8 || bit_depth == 16 || bit_depth == 24) && total_samples > 0;
        if (!supported) {
            ape_demux_close(demux);
            /* The header parsed and names a level or layout the decoder does not handle. */
            if (compression > 0 && sample_rate > 0) *status = AUDIO_PROBE_UNSUPPORTED;
            return false;
        }

        memset(out, 0, sizeof(*out));
        out->valid = true;
        *status = AUDIO_PROBE_OK;
        snprintf(out->path, sizeof(out->path), "%s", path);
        out->codec = AUDIO_CODEC_APE;
        out->source_sample_rate = sample_rate;
        out->source_bit_depth = bit_depth;
        out->channels = channels;
        out->duration_seconds = (double) total_samples / (double) sample_rate;
        ape_demux_close(demux);
        return true;
    }

    decoder_t dec;
    memset(&dec, 0, sizeof(dec));
    if (!decoder_open(&dec, path)) {
        /* Only a positive rejection counts: decoder_open() names an extension
         * or MP4 sample entry nothing handles, and a fragmented MP4 makes the
         * sample-table demuxer fail on a file that is intact. Anything else
         * (read error, no memory, truncated read) may be transient. The file
         * must still be readable, so a vanished card is not "unsupported". */
        bool rejected = !dec.open_io_error &&
                        (dec.open_error == AUDIO_ERROR_UNSUPPORTED_FORMAT ||
                         (probe_path_is_mp4_family(path) && mp4_demux_path_is_fragmented(path)));
        if (rejected && access(path, R_OK) == 0) *status = AUDIO_PROBE_UNSUPPORTED;
        return false;
    }
    *status = AUDIO_PROBE_OK;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    snprintf(out->path, sizeof(out->path), "%s", path);
    out->codec = public_codec_for_decoder(&dec);
    out->source_sample_rate = dec.source_sample_rate ? dec.source_sample_rate : dec.sample_rate;
    out->source_bit_depth = dec.source_bit_depth;
    out->channels = dec.channels;
    out->bitrate_kbps = dec.bitrate_kbps;
    out->duration_seconds = dec.sample_rate && dec.total_frames
        ? (double) dec.total_frames / (double) dec.sample_rate : 0.0;
    out->is_dsd = dec.type == DECODER_DSD;
    decoder_close(&dec);
    return true;
}
