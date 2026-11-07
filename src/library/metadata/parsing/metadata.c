#define _FILE_OFFSET_BITS 64
#include "metadata.h"
#include "metadata_parser_internal.h"
#include "../../../core/text/utf8_util.h"
#include "../../parsers/cue/cue_parser.h"
#include "../../sacd/sacd_iso.h"
#include "../../../audio/decoders/ogg/ogg_probe.h"
#include "mbedtls/base64.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

/* Validates that adding size and pad to chunk_start will not overflow a
 * signed 64-bit file offset. Returns true and writes the next seek offset to
 * *out_next on success, or false if size exceeds safe bounds. */
bool safe_file_advance(off_t chunk_start, uint64_t size, off_t pad, off_t * out_next) {
    if (chunk_start < 0 || pad < 0) return false;
    if (size > (uint64_t) (INT64_MAX - pad)) return false;
    off_t size_o = (off_t) size; /* provably in-range now -- no implementation-defined truncation */
    if (chunk_start > INT64_MAX - size_o - pad) return false; /* would overflow the addition below */
    *out_next = chunk_start + size_o + pad;
    return true;
}

/* Enabled only in the disposable artwork helper, never in playback readers. */
bool artwork_only;

void metadata_parser_enable_artwork_only(void) {
    artwork_only = true;
}
/* Per-thread mode used by the lyrics worker: retain lyrics while skipping
 * caller-visible artwork extraction.  TLS keeps concurrent readers isolated. */
_Thread_local bool lyrics_only;

bool store_picture_bytes(track_metadata_t * out, const uint8_t * src, uint32_t n) {
    if (!out || out->picture_data != NULL || !src || n == 0) return false;
    if (n > EMBEDDED_COVER_MAX_BYTES) {
        out->picture_too_large = true;
        return false;
    }
    uint8_t * copy = malloc(n);
    if (!copy) return false;
    memcpy(copy, src, n);
    out->picture_data = copy;
    out->picture_size = n;
    return true;
}

void copy_bounded(char * dst, size_t dst_size, const char * src, size_t src_len) {
    utf8_truncate_safe_bounded(dst, dst_size, src, src_len);
}

/* Stores a text tag and marks it present (even when the copy is empty). */
void set_tag_text(char * dst, size_t dst_size, bool * has, const char * v, size_t n) {
    copy_bounded(dst, dst_size, v, n);
    *has = true;
}

void set_sequence(track_metadata_t * out, bool disc, int number) {
    if (disc) {
        out->disc_number = number;
        out->has_disc_number = true;
    } else {
        out->track_number = number;
        out->has_track_number = true;
    }
}

uint32_t read_be32(const uint8_t * b, bool synchsafe) {
    if (synchsafe) {
        return ((uint32_t) (b[0] & 0x7F) << 21) | ((uint32_t) (b[1] & 0x7F) << 14) |
               ((uint32_t) (b[2] & 0x7F) << 7) | (uint32_t) (b[3] & 0x7F);
    }
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | (uint32_t) b[3];
}

/* DSDIFF chunk sizes are big-endian, unlike DSF/ASF. */
uint64_t read_u64be(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

static bool parse_sequence_number(const char * text, size_t len, int * out_value) {
    size_t i = 0;
    while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
    unsigned value = 0;
    bool have_digit = false;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        have_digit = true;
        unsigned digit = (unsigned) (text[i++] - '0');
        if (value > 9999U) return false;
        value = value * 10U + digit;
    }
    if (!have_digit || value == 0) return false;
    *out_value = (int) value;
    return true;
}

void apply_sequence_text(track_metadata_t * out, bool disc, const char * value, size_t value_len) {
    int number;
    if (!parse_sequence_number(value, value_len, &number)) return;
    set_sequence(out, disc, number);
}

static bool parse_two_digits(const char * text, size_t len, size_t * pos, int * out) {
    if (*pos + 2 > len || text[*pos] < '0' || text[*pos] > '9' ||
        text[*pos + 1] < '0' || text[*pos + 1] > '9') return false;
    *out = (text[*pos] - '0') * 10 + text[*pos + 1] - '0';
    *pos += 2;
    return true;
}

/* Accept a year by itself, YYYY-MM, or a valid ISO calendar date optionally
 * followed by an ISO time and zone. Refuse a valid-looking year embedded in
 * arbitrary text (for example "1234 remaster"). */
static bool parse_release_year(const char * value, size_t len, int * out_year) {
    if (!value || !out_year) return false;
    size_t begin = 0, end = len;
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r' || value[begin] == '\n')) begin++;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) end--;
    if (end - begin < 4) return false;
    int year = 0;
    for (size_t i = 0; i < 4; i++) {
        char c = value[begin + i];
        if (c < '0' || c > '9') return false;
        year = year * 10 + c - '0';
    }
    if (year < 1000 || year > 9999) return false;
    size_t p = begin + 4;
    if (p == end) { *out_year = year; return true; }

    int month = 0, day = 1;
    if (value[p++] != '-' || !parse_two_digits(value, end, &p, &month) || month < 1 || month > 12) return false;
    if (p < end) {
        if (value[p++] != '-' || !parse_two_digits(value, end, &p, &day)) return false;
        static const int days_in_month[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        int max_day = days_in_month[month];
        if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) max_day++;
        if (day < 1 || day > max_day) return false;
    }
    if (p == end) { *out_year = year; return true; }

    /* ISO time requires a complete day and hh:mm. Seconds and fractional
     * seconds are optional; a zone is allowed with or without seconds. */
    if (value[p] != 'T' && value[p] != 't' && value[p] != ' ') return false;
    p++;
    int hour, minute, second = 0;
    if (!parse_two_digits(value, end, &p, &hour) || p >= end || value[p++] != ':' ||
        !parse_two_digits(value, end, &p, &minute) || hour > 23 || minute > 59) return false;
    if (p < end && value[p] == ':') {
        p++;
        if (!parse_two_digits(value, end, &p, &second) || second > 60) return false;
        if (p < end && value[p] == '.') {
            p++;
            size_t digits = p;
            while (p < end && value[p] >= '0' && value[p] <= '9') p++;
            if (p == digits) return false;
        }
    }
    if (p < end && (value[p] == 'Z' || value[p] == 'z')) p++;
    else if (p < end && (value[p] == '+' || value[p] == '-')) {
        p++;
        int zone_hour, zone_minute;
        if (!parse_two_digits(value, end, &p, &zone_hour) || p >= end || value[p++] != ':' ||
            !parse_two_digits(value, end, &p, &zone_minute) || zone_hour > 14 || zone_minute > 59 ||
            (zone_hour == 14 && zone_minute != 0)) return false;
    }
    if (p != end) return false;
    *out_year = year;
    return true;
}

void apply_release_year(track_metadata_t * out, const char * value, size_t value_len,
                               bool canonical_date) {
    int year;
    if (!parse_release_year(value, value_len, &year)) return;
    /* Canonical date/recording-time fields override year-only aliases even
     * when the alias appears later in the file. */
    if (!out->has_original_year && (canonical_date || !out->has_release_year)) {
        out->release_year = year;
        out->has_release_year = true;
    }
}

void apply_original_year(track_metadata_t * out, const char * value, size_t value_len) {
    int year;
    if (!parse_release_year(value, value_len, &year)) return;
    out->release_year = year;
    out->has_release_year = true;
    out->has_original_year = true;
}

/* strtod() wrapper for REPLAYGAIN_* fields. Reports success only when at least
 * one digit was consumed and the parsed float is finite (rejecting NaN and Infinity). */
static bool parse_finite_double(const char * str, double * out) {
    char * end = NULL;
    double v = strtod(str, &end);
    if (end == str || !isfinite(v)) return false;
    *out = v;
    return true;
}

/* Bounds ReplayGain values to sane limits (+-100 dB gain, 0..100 peak ratio)
 * to prevent floating-point overflow during linear gain conversion. */
#define REPLAYGAIN_GAIN_DB_LIMIT 100.0
#define REPLAYGAIN_PEAK_LIMIT 100.0

static bool parse_replaygain_gain(const char * str, double * out) {
    double v;
    if (!parse_finite_double(str, &v) || v < -REPLAYGAIN_GAIN_DB_LIMIT || v > REPLAYGAIN_GAIN_DB_LIMIT) return false;
    *out = v;
    return true;
}

static bool parse_replaygain_peak(const char * str, double * out) {
    double v;
    if (!parse_finite_double(str, &v) || v <= 0.0 || v > REPLAYGAIN_PEAK_LIMIT) return false;
    *out = v;
    return true;
}

/* REPLAYGAIN_TRACK_GAIN/_PEAK/_ALBUM_GAIN/_ALBUM_PEAK key matching, shared
 * by every format that hands over an already-split key/value pair (Vorbis
 * comments, APEv2 items, ID3 TXXX). value is bounded-copied into a small
 * stack buffer first since no caller's value buffer is guaranteed
 * NUL-terminated. */
void apply_replaygain_field(track_metadata_t * out, const char * key, size_t key_len, const char * value,
                                    size_t value_len) {
    if (key_len != 21) return; /* every one of the four keys below is exactly 21 characters */
    char value_buf[32];
    copy_bounded(value_buf, sizeof(value_buf), value, value_len);
    double v;
    if (strncasecmp(key, "REPLAYGAIN_TRACK_GAIN", 21) == 0) {
        if (parse_replaygain_gain(value_buf, &v)) {
            out->replaygain_gain_db = v;
            out->has_replaygain = true;
        }
    } else if (strncasecmp(key, "REPLAYGAIN_TRACK_PEAK", 21) == 0) {
        if (parse_replaygain_peak(value_buf, &v)) {
            out->replaygain_peak = v;
            out->has_replaygain_peak = true;
        }
    } else if (strncasecmp(key, "REPLAYGAIN_ALBUM_GAIN", 21) == 0) {
        if (parse_replaygain_gain(value_buf, &v)) {
            out->replaygain_album_gain_db = v;
            out->has_replaygain_album = true;
        }
    } else if (strncasecmp(key, "REPLAYGAIN_ALBUM_PEAK", 21) == 0) {
        if (parse_replaygain_peak(value_buf, &v)) {
            out->replaygain_album_peak = v;
            out->has_replaygain_album_peak = true;
        }
    }
}

/* Generic Vorbis-Comment "KEY=VALUE" field matching -- shared by FLAC's
 * VORBIS_COMMENT block (below, via dr_flac's own iterator) and Opus's
 * OpusTags (read_opus_metadata(), same comment-list layout per RFC 7845
 * 5.2). Only the iteration mechanism differs between formats; this matching
 * logic has no dr_flac-specific coupling. */
void apply_vorbis_comment_field(track_metadata_t * out, const char * comment, size_t comment_len,
                                       bool include_blobs) {
    const char * eq = memchr(comment, '=', comment_len);
    if (!eq) return;

    size_t key_len = (size_t) (eq - comment);
    const char * value = eq + 1;
    size_t value_len = comment_len - key_len - 1;

    if (key_len == 5 && strncasecmp(comment, "TITLE", 5) == 0) {
        SET_TAG_TEXT(out, title, value, value_len);
    } else if (key_len == 6 && strncasecmp(comment, "ARTIST", 6) == 0) {
        SET_TAG_TEXT(out, artist, value, value_len);
    } else if (key_len == 5 && strncasecmp(comment, "ALBUM", 5) == 0) {
        SET_TAG_TEXT(out, album, value, value_len);
    } else if (key_len == 11 && strncasecmp(comment, "ALBUMARTIST", 11) == 0) {
        SET_TAG_TEXT(out, album_artist, value, value_len);
    } else if (key_len == 12 && !out->album_artist[0] &&
               (strncasecmp(comment, "ALBUM ARTIST", 12) == 0 ||
                strncasecmp(comment, "ALBUM_ARTIST", 12) == 0)) {
        /* Accept common Vorbis aliases, but let canonical ALBUMARTIST win
         * regardless of comment order when both forms are present. */
        SET_TAG_TEXT(out, album_artist, value, value_len);
    } else if (key_len == 5 && strncasecmp(comment, "GENRE", 5) == 0) {
        SET_TAG_TEXT(out, genre, value, value_len);
    } else if (key_len == 12 &&
               (strncasecmp(comment, "ORIGINALDATE", 12) == 0 ||
                strncasecmp(comment, "ORIGINALYEAR", 12) == 0)) {
        apply_original_year(out, value, value_len);
    } else if (key_len == 4 && strncasecmp(comment, "DATE", 4) == 0) {
        apply_release_year(out, value, value_len, true);
    } else if (key_len == 4 && strncasecmp(comment, "YEAR", 4) == 0) {
        apply_release_year(out, value, value_len, false);
    } else if (key_len == 11 && strncasecmp(comment, "TRACKNUMBER", 11) == 0) {
        apply_sequence_text(out, false, value, value_len);
    } else if ((key_len == 10 && strncasecmp(comment, "DISCNUMBER", 10) == 0) ||
               (key_len == 10 && strncasecmp(comment, "DISKNUMBER", 10) == 0)) {
        apply_sequence_text(out, true, value, value_len);
    } else if (key_len == 21 && strncasecmp(comment, "REPLAYGAIN_", 11) == 0) {
        /* strtod() stops at a trailing " dB" on gain fields, no need to
         * strip it -- apply_replaygain_field() re-checks the full key
         * against each of the four exact REPLAYGAIN_* names itself. */
        apply_replaygain_field(out, comment, key_len, value, value_len);
    } else if (wants_lyrics(include_blobs) && out->lyrics == NULL && value_len > 0 &&
               ((key_len == 6 && strncasecmp(comment, "LYRICS", 6) == 0) ||
                (key_len == 14 && strncasecmp(comment, "UNSYNCEDLYRICS", 14) == 0))) {
        /* Plain UTF-8 text, no ID3v2-style encoding byte to strip -- unlike
         * decode_id3v2_uslt_frame()'s own malloc'd buffer, this is just a
         * straight copy. Keeps the first of either key found, same
         * first-wins precedent as picture_data. May or may not contain
         * [mm:ss.xx] LRC timestamps -- see track_metadata_t's own lyrics
         * field comment. */
        if (value_len > METADATA_BLOB_MAX_BYTES) return;
        char * lyrics = malloc(value_len + 1);
        if (lyrics) {
            memcpy(lyrics, value, value_len);
            lyrics[value_len] = '\0';
            out->lyrics = lyrics;
        }
    }
}

/* Parses a METADATA_BLOCK_PICTURE structure (https://xiph.org/flac/format.html#metadata_block_picture,
 * also used verbatim -- just base64-wrapped as an Ogg/Opus comment value --
 * by Xiph's own "PICTURE" tag convention for Vorbis-Comment-based formats):
 * a fixed run of big-endian fields (type, then length-prefixed MIME string,
 * length-prefixed description, width/height/depth/color-count) followed by
 * the length-prefixed image bytes themselves. dr_flac parses this same
 * layout internally for native FLAC picture blocks (exposed directly via
 * meta->data.picture in flac_meta_cb() in metadata_xiph_mp4.c), but that parsing isn't
 * reusable source code -- this is a from-scratch equivalent for the
 * base64-decoded bytes the Opus path hands it, keeping only the final
 * image bytes since track_metadata_t has no fields for the others. */
static void parse_flac_picture_block(const uint8_t * data, size_t size, track_metadata_t * out) {
    if (out->picture_data != NULL) return; /* keep the first picture found, same as FLAC/MP3/M4A */

    size_t pos = 0;
    if (pos + 4 > size) return;
    pos += 4; /* picture type -- not discriminated on, matching FLAC/MP3/M4A's "first picture wins" */

    if (pos + 4 > size) return;
    uint32_t mime_len = read_be32(data + pos, false);
    pos += 4;
    if (pos + mime_len > size) return;
    pos += mime_len;

    if (pos + 4 > size) return;
    uint32_t desc_len = read_be32(data + pos, false);
    pos += 4;
    if (pos + desc_len > size) return;
    pos += desc_len;

    if (pos + 16 > size) return; /* width, height, depth, color-count -- 4 bytes each */
    pos += 16;

    if (pos + 4 > size) return;
    uint32_t pic_data_len = read_be32(data + pos, false);
    pos += 4;
    if (pic_data_len == 0 || pos + pic_data_len > size) return;

    store_picture_bytes(out, data + pos, pic_data_len);
}

/* METADATA_BLOCK_PICTURE=<base64> comment (Opus/Ogg Vorbis): decoded here
 * rather than in apply_vorbis_comment_field(), which stays a pure text-field
 * matcher. Returns true when the comment was a picture comment and is
 * consumed; otherwise (including when pictures are not wanted) the caller
 * runs the text-field matcher as for any other comment. FLAC does not route
 * through this: its pictures arrive as native PICTURE blocks. */
bool apply_vorbis_picture_comment(track_metadata_t * out, const char * comment, size_t comment_len,
                                         bool include_blobs) {
    static const char picture_key[] = "METADATA_BLOCK_PICTURE=";
    size_t picture_key_len = sizeof(picture_key) - 1;
    if (!wants_picture(include_blobs) || comment_len <= picture_key_len ||
        strncasecmp(comment, picture_key, picture_key_len) != 0)
        return false;

    const char * b64 = comment + picture_key_len;
    size_t b64_len = comment_len - picture_key_len;

    size_t decoded_len = 0;
    int size_err = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *) b64, b64_len);
    if ((size_err == 0 || size_err == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) &&
        decoded_len > METADATA_BLOB_MAX_BYTES)
        out->picture_too_large = true;
    if ((size_err == 0 || size_err == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) && decoded_len > 0 &&
        decoded_len <= METADATA_BLOB_MAX_BYTES) {
        uint8_t * decoded = malloc(decoded_len);
        if (decoded) {
            size_t actual_len = 0;
            if (mbedtls_base64_decode(decoded, decoded_len, &actual_len, (const unsigned char *) b64, b64_len) == 0) {
                parse_flac_picture_block(decoded, actual_len, out);
            }
            free(decoded);
        }
    }
    return true;
}

/* One display line per tag: control characters (line breaks from multi-line
 * tags included, CRLF as one) become spaces, so a label never grows into the
 * row below it. A tag with no visible characters counts as absent, so callers
 * apply the same fallback as for a missing tag. Other spacing is kept: it is
 * part of the value the library groups by. */
static void normalize_tag_text(char * text, bool * present) {
    if (!*present) return;
    bool visible = false;
    char * w = text;
    for (const char * r = text; *r; r++) {
        unsigned char c = (unsigned char) *r;
        if (c == '\r' && r[1] == '\n') continue;
        if (c < 0x20 || c == 0x7F) c = ' ';
        if (c != ' ') visible = true;
        *w++ = (char) c;
    }
    *w = '\0';
    if (!visible) {
        text[0] = '\0';
        *present = false;
    }
}

void metadata_normalize_text_tags(track_metadata_t * out) {
    normalize_tag_text(out->title, &out->has_title);
    normalize_tag_text(out->artist, &out->has_artist);
    normalize_tag_text(out->album, &out->has_album);
    normalize_tag_text(out->album_artist, &out->has_album_artist);
    normalize_tag_text(out->genre, &out->has_genre);
}

static void metadata_read_dispatch(const char * path, track_metadata_t * out, bool include_blobs);

/* CUE virtual tracks keep the physical source as their storage identity.
 * Read its tags and artwork once, then replace only the fields supplied by
 * the selected CUE entry/sheet. */
static void read_cue_track_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    unsigned ordinal = 0;
    cue_sheet_t sheet = {0};
    if (!cue_reference_resolve(path, &sheet, &ordinal) || ordinal == 0 ||
        ordinal > (unsigned) sheet.track_count) {
        cue_sheet_free(&sheet);
        return;
    }
    const cue_track_t * track = &sheet.tracks[ordinal - 1];
    metadata_read_dispatch(track->audio_path, out, include_blobs);
    if (track->title[0]) {
        SET_TAG_TEXT(out, title, track->title, strlen(track->title));
    } else {
        snprintf(out->title, sizeof(out->title), "Track %d", track->number);
        out->has_title = true;
    }
    const char * performer = track->performer[0] ? track->performer : sheet.performer;
    if (performer[0]) SET_TAG_TEXT(out, artist, performer, strlen(performer));
    if (sheet.title[0]) SET_TAG_TEXT(out, album, sheet.title, strlen(sheet.title));
    if (track->number > 0) set_sequence(out, false, track->number);
    cue_sheet_free(&sheet);
}

/* The extension names the codec; CAF carries a distinct signature. */
static void read_alac_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * file = fopen(path, "rb");
    char magic[4];
    bool is_caf = file && fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
                  memcmp(magic, "caff", sizeof(magic)) == 0;
    if (file) fclose(file);
    if (is_caf) read_caf_metadata(path, out, include_blobs);
    else read_m4a_metadata(path, out, include_blobs);
}

static void read_ogg_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    ogg_codec_t codec = ogg_detect_codec(path);
    if (codec == OGG_CODEC_OPUS) read_opus_metadata(path, out, include_blobs);
    else if (codec == OGG_CODEC_VORBIS) read_ogg_vorbis_metadata(path, out, include_blobs);
}

static void read_sacd_iso_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    (void) include_blobs; /* the SACD TOC carries no artwork or lyrics */
    FILE *file = fopen(path, "rb");
    sacd_toc_t *toc = calloc(1, sizeof(*toc));
    bool ok = file && toc && sacd_read_toc(file, toc);
    if (file) fclose(file);
    if (ok) {
        if (toc->title[0]) {
            SET_TAG_TEXT(out, title, toc->title, strlen(toc->title));
            SET_TAG_TEXT(out, album, toc->title, strlen(toc->title));
        }
        if (toc->artist[0]) {
            SET_TAG_TEXT(out, artist, toc->artist, strlen(toc->artist));
            SET_TAG_TEXT(out, album_artist, toc->artist, strlen(toc->artist));
        }
    }
    free(toc);
}

typedef void (*metadata_reader_fn)(const char * path, track_metadata_t * out, bool include_blobs);

/* Reader per file extension (matched case-insensitively). */
static const struct {
    const char * ext;
    metadata_reader_fn read;
} metadata_readers[] = {
    { ".flac", read_flac_metadata },
    { ".mp3", read_mp3_metadata },
    { ".wav", read_wav_all },
    { ".rf64", read_wav_all },
    { ".w64", read_wav_all },
    { ".aac", read_aac_metadata },
    { ".wv", read_wavpack_metadata },
    { ".caf", read_caf_metadata },
    { ".alac", read_alac_metadata },
    { ".m4a", read_m4a_metadata },
    { ".m4b", read_m4a_metadata },
    { ".mp4", read_m4a_metadata },
    { ".opus", read_opus_metadata },
    { ".ogg", read_ogg_metadata },
    { ".oga", read_ogg_metadata },
    { ".aiff", read_aiff_metadata },
    { ".aif", read_aiff_metadata },
    { ".aifc", read_aiff_metadata },
    { ".dsf", read_dsf_metadata },
    { ".ape", read_ape_metadata },
    { ".dff", read_dff_metadata },
    { ".iso", read_sacd_iso_metadata },
    { ".wma", read_wma_metadata },
};

static void metadata_read_dispatch(const char * path, track_metadata_t * out, bool include_blobs) {
    memset(out, 0, sizeof(*out));

    if (cue_reference_is(path)) {
        read_cue_track_metadata(path, out, include_blobs);
        return;
    }

    const char * ext = strrchr(path, '.');
    if (!ext) return;
    for (size_t i = 0; i < sizeof(metadata_readers) / sizeof(metadata_readers[0]); i++) {
        if (strcasecmp(ext, metadata_readers[i].ext) == 0) {
            metadata_readers[i].read(path, out, include_blobs);
            return;
        }
    }
}

static void metadata_read_internal(const char * path, track_metadata_t * out, bool include_blobs) {
    metadata_read_dispatch(path, out, include_blobs);
    metadata_normalize_text_tags(out);
}

void metadata_read(const char * path, track_metadata_t * out) {
    metadata_read_internal(path, out, true);
}

void metadata_read_without_artwork(const char * path, track_metadata_t * out) {
    /* The no-blob mode preserves textual tags and ReplayGain while avoiding
     * embedded artwork (lyrics are intentionally omitted as well). */
    metadata_read_internal(path, out, false);
}

void metadata_read_lyrics_without_artwork(const char * path, track_metadata_t * out) {
    bool previous = lyrics_only;
    lyrics_only = true;
    metadata_read_internal(path, out, true);
    lyrics_only = previous;
}
