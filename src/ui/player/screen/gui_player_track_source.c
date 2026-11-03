#include "gui_player.h"
#include "gui_player_internal.h"
#include "../queue/player_queue_model.h"
#include "../../application/gui.h"
#include "../../settings/gui_settings.h"
#include "../../shell/gui_notifications.h"
#include "../../text/rendering/fallback_font.h"
#include "../../network/subsonic/gui_subsonic.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../library/parsers/cue/cue_parser.h"
#include "../../../library/remote/remote_track.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static audio_current_format_info_t player_cached_format;

audio_codec_t info_codec_from_hint(const char * hint) {
    if (!hint || !hint[0]) return AUDIO_CODEC_UNKNOWN;
    if (*hint == '.') hint++;
    if (strcasecmp(hint, "flac") == 0) return AUDIO_CODEC_FLAC;
    if (strcasecmp(hint, "mp3") == 0) return AUDIO_CODEC_MP3;
    if (strcasecmp(hint, "wav") == 0 || strcasecmp(hint, "rf64") == 0 ||
        strcasecmp(hint, "w64") == 0 || strcasecmp(hint, "aif") == 0 ||
        strcasecmp(hint, "aiff") == 0 || strcasecmp(hint, "aifc") == 0) return AUDIO_CODEC_PCM;
    if (strcasecmp(hint, "dsf") == 0 || strcasecmp(hint, "dff") == 0)
        return AUDIO_CODEC_DSD;
    if (strcasecmp(hint, "aac") == 0 || strcasecmp(hint, "aacp") == 0)
        return AUDIO_CODEC_AAC;
    if (strcasecmp(hint, "wv") == 0 || strcasecmp(hint, "wavpack") == 0) return AUDIO_CODEC_WAVPACK;
    if (strcasecmp(hint, "ape") == 0) return AUDIO_CODEC_APE;
    if (strcasecmp(hint, "wma") == 0 || strcasecmp(hint, "asf") == 0)
        return AUDIO_CODEC_WMA;
    if (strcasecmp(hint, "opus") == 0) return AUDIO_CODEC_OPUS;
    return AUDIO_CODEC_UNKNOWN; /* M4A/Ogg need the actual decoder to disambiguate. */
}

void info_container_from_hint(const char * hint, char out[16]) {
    out[0] = '\0';
    if (!hint || !hint[0]) return;
    if (*hint == '.') hint++;
    if (strcasecmp(hint, "aif") == 0 || strcasecmp(hint, "aiff") == 0 ||
        strcasecmp(hint, "aifc") == 0)
        snprintf(out, 16, "AIFF");
    else if (strcasecmp(hint, "aac") == 0 || strcasecmp(hint, "aacp") == 0)
        snprintf(out, 16, "ADTS");
    else if (strcasecmp(hint, "wma") == 0 || strcasecmp(hint, "asf") == 0)
        snprintf(out, 16, "ASF");
    else if (strcasecmp(hint, "m4a") == 0 || strcasecmp(hint, "m4b") == 0 ||
             strcasecmp(hint, "mp4") == 0)
        snprintf(out, 16, "M4A");
    else if (strcasecmp(hint, "ogg") == 0 || strcasecmp(hint, "oga") == 0 ||
             strcasecmp(hint, "opus") == 0)
        snprintf(out, 16, "Ogg");
    else {
        size_t i = 0;
        while (hint[i] && i + 1 < 16) {
            out[i] = (char) toupper((unsigned char) hint[i]);
            i++;
        }
        out[i] = '\0';
    }
}

bool is_http_url(const char * path) {
    if (!path) return false;
    return strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0;
}

const char * info_path_hint(const char * path) {
    if (!path) return NULL;
    if (is_http_url(path)) {
        /* Only this app's local-only #.<ext> hint is safe to inspect on a
         * stream. Never treat a URL path/query suffix as a container name:
         * besides being unreliable, a query can contain an auth token and
         * must not reach the Information UI even in truncated form. */
        const char * fragment = strrchr(path, '#');
        return fragment && fragment[1] == '.' ? fragment + 2 : NULL;
    }
    const char * base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char * dot = strrchr(base, '.');
    return dot ? dot + 1 : NULL;
}

/* The stream catalog is initially parallel to the queue created by
 * Subsonic. Queue edits can move playlist slots without moving that array,
 * so retain the fast indexed lookup and fall back to the stable stream URL.
 * Both callers and the live playlist must agree on the exact path before a
 * metadata record is trusted. */
const subsonic_stream_song_meta_t * subsonic_meta_for_path(int index, const char * path) {
    if (!path || !is_http_url(path) || index < 0 || index >= player_queue_model_count() ||
        !subsonic_stream_meta || subsonic_stream_meta_count <= 0) return NULL;
    const char * live_path = playlist_path_at(index);
    if (!live_path || strcmp(path, live_path) != 0) return NULL;
    if (index < subsonic_stream_meta_count &&
        strcmp(path, subsonic_stream_meta[index].url) == 0)
        return &subsonic_stream_meta[index];
    for (int i = 0; i < subsonic_stream_meta_count; i++) {
        if (strcmp(path, subsonic_stream_meta[i].url) == 0)
            return &subsonic_stream_meta[i];
    }
    return NULL;
}

void track_source_resolve(int index, const char * path, track_source_t * out) {
    out->subsonic = subsonic_meta_for_path(index, path);
    out->is_subsonic = out->subsonic != NULL;
    out->is_remote = remote_track_meta_copy_for_path(path, &out->remote);
}

/* Resolves which of a track's own ReplayGain fields to actually hand to
 * audio.c, per Settings -> Playback -> ReplayGain's mode: Off (no gain at
 * all), Per Track (the default -- normalizes every track to the same
 * perceived loudness), or Per Album (preserves intentional relative
 * loudness differences between tracks on the same album). Falls back to
 * track gain when Per Album is selected but this particular file has no
 * album-level tag -- most taggers only write one or the other depending on
 * whether the rip was tagged as a whole album or track-at-a-time, and
 * silently playing unnormalized instead of falling back to whatever IS
 * available would be a worse outcome than just using track gain here. */
void replaygain_for_mode(int mode, const track_metadata_t * meta, bool * out_has_gain,
                                double * out_gain_db, bool * out_has_peak, double * out_peak) {
    if (mode == 2 && meta->has_replaygain_album) {
        *out_has_gain = true;
        *out_gain_db = meta->replaygain_album_gain_db;
        if (meta->has_replaygain_album_peak) {
            *out_has_peak = true;
            *out_peak = meta->replaygain_album_peak;
        } else {
            *out_has_peak = meta->has_replaygain_peak;
            *out_peak = meta->replaygain_peak;
        }
        return;
    }
    bool use_track = mode != 0;
    *out_has_gain = use_track && meta->has_replaygain;
    *out_gain_db = meta->replaygain_gain_db;
    *out_has_peak = use_track && meta->has_replaygain_peak;
    *out_peak = meta->replaygain_peak;
}

void resolve_replaygain(const track_metadata_t * meta, bool * out_has_gain, double * out_gain_db,
                                bool * out_has_peak, double * out_peak) {
    replaygain_for_mode(current_settings.replaygain_mode, meta, out_has_gain, out_gain_db, out_has_peak, out_peak);
}

/* A stream's URL is not a format label. Only known catalog/local hint
 * values may supply a fallback while its decoder is still opening. */
static const char * stream_format_hint_name(const char * hint) {
    const char * name = audio_codec_name(info_codec_from_hint(hint));
    if (name[0] || !hint) return name;
    if (*hint == '.') hint++;
    if (strcasecmp(hint, "alac") == 0) return "ALAC";
    if (strcasecmp(hint, "vorbis") == 0) return "Vorbis";
    if (strcasecmp(hint, "m4a") == 0 || strcasecmp(hint, "m4b") == 0 ||
        strcasecmp(hint, "mp4") == 0) return "M4A";
    if (strcasecmp(hint, "ogg") == 0 || strcasecmp(hint, "oga") == 0) return "Ogg";
    return "";
}

void refresh_format_badge(void) {
    const char * path = player_queue_model_current() >= 0 ? gui_player_get_current_track_path() : NULL;
    if (!path || !path[0]) {
        player_set_format_badge_text("");
        gui_shell_update_quick_drawer_format("");
        return;
    }

    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    unsigned int sample_rate = is_remote_track ? remote_meta.sample_rate : 0;
    unsigned int bit_depth = is_remote_track ? remote_meta.bit_depth : 0;
    audio_codec_t codec = AUDIO_CODEC_UNKNOWN;
    if (player_cached_format.valid && strcmp(player_cached_format.path, path) == 0) {
        sample_rate = player_cached_format.source_sample_rate;
        bit_depth = player_cached_format.source_bit_depth;
        codec = player_cached_format.codec;
    }
    audio_current_format_info_t fmt_info;
    if (audio_get_current_format_info(&fmt_info) && fmt_info.valid &&
        strcmp(fmt_info.path, path) == 0) {
        sample_rate = fmt_info.source_sample_rate;
        bit_depth = fmt_info.source_bit_depth;
        codec = fmt_info.codec;
    }

    char ext[16] = "";
    const char * hint = info_path_hint(path);
    if (cue_reference_is(path)) {
        /* The reference identifies a CUE entry, not its audio container. */
        snprintf(ext, sizeof(ext), "%s", audio_codec_name(codec));
    } else if (is_http_url(path) || is_remote_track) {
        const char * name = audio_codec_name(codec);
        if (!name[0] && is_remote_track) name = stream_format_hint_name(remote_meta.codec);
        if (!name[0]) name = stream_format_hint_name(hint);
        snprintf(ext, sizeof(ext), "%s", name);
    } else if (hint) {
        /* Local badges retain container extensions such as DSF and M4A. */
        size_t i = 0;
        for (; hint[i] && i < sizeof(ext) - 1; i++)
            ext[i] = (char) toupper((unsigned char) hint[i]);
        ext[i] = '\0';
    }

    char text[96];
    const char * separator = ext[0] ? " " : "";
    if (sample_rate > 0 && bit_depth > 0) {
        snprintf(text, sizeof(text), "%s%s%u-bit / %.1fkHz", ext, separator,
                 bit_depth, sample_rate / 1000.0);
    } else if (sample_rate > 0) {
        snprintf(text, sizeof(text), "%s%s%.1fkHz", ext, separator, sample_rate / 1000.0);
    } else if (bit_depth > 0) {
        snprintf(text, sizeof(text), "%s%s%u-bit", ext, separator, bit_depth);
    } else {
        snprintf(text, sizeof(text), "%s", ext);
    }
    /* Polling unchanged text must not restart its marquee every tick. */
    player_set_format_badge_text(text);
    gui_shell_update_quick_drawer_format(text);
}

void player_track_source_set_cached_format(const audio_current_format_info_t * format) {
    player_cached_format = *format;
}

bool gui_player_get_subsonic_track_identity(int track_index, const char * path,
                                            char * title, size_t title_size,
                                            char * artist, size_t artist_size,
                                            char * album, size_t album_size) {
    if (title && title_size) title[0] = '\0';
    if (artist && artist_size) artist[0] = '\0';
    if (album && album_size) album[0] = '\0';
    /* Snapshot rows use physical playlist slots; metadata can have moved
     * relative to those slots after a queue edit, so resolve by exact URL. */
    const subsonic_stream_song_meta_t * meta = subsonic_meta_for_path(track_index, path);
    if (!meta) return false;

    if (title && title_size) utf8_truncate_safe(title, meta->title, title_size);
    if (artist && artist_size) utf8_truncate_safe(artist, meta->artist, artist_size);
    if (album && album_size) utf8_truncate_safe(album, meta->album, album_size);
    return true;
}
