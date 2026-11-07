#define _FILE_OFFSET_BITS 64
#include "metadata.h"
#include "metadata_parser_internal.h"
#include "../../text/lyrics/lyrics.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Text and TXXX frames larger than this are skipped while streaming. Pictures
 * use METADATA_BLOB_MAX_BYTES (also the buffered ID3 tag limit and the base64
 * METADATA_BLOCK_PICTURE decode cap; larger tags are streamed), lyrics the
 * smaller LYRICS_MAX_FILE_BYTES. */
#define ID3_TEXT_FRAME_MAX_BYTES (64U * 1024U)
#define ID3_FRAME_COUNT_MAX 4096U
#define ID3_STREAM_WORK_BUDGET_BYTES (METADATA_BLOB_MAX_BYTES + ID3_TEXT_FRAME_MAX_BYTES + \
                                      ID3_FRAME_COUNT_MAX * 10U + 10U)

/* ---- MP3: ID3v2 (falling back to ID3v1) -- dr_mp3 has no tag support ---- */

/* ---- ID3v2 text decoding ---- */

/* Offset of the first UTF-16 unit after an optional BOM (encoding 0x01:
 * BOM selects the byte order, default little-endian; 0x02: big-endian, no
 * BOM). */
static size_t id3_utf16_start(uint8_t encoding, const uint8_t * text, size_t len, bool * big_endian) {
    *big_endian = (encoding == 0x02);
    if (encoding == 0x01 && len >= 2) {
        if (text[0] == 0xFF && text[1] == 0xFE) { *big_endian = false; return 2; }
        if (text[0] == 0xFE && text[1] == 0xFF) { *big_endian = true; return 2; }
    }
    return 0;
}

static uint16_t id3_utf16_unit(const uint8_t * t, bool big_endian) {
    return big_endian ? (uint16_t) ((t[0] << 8) | t[1]) : (uint16_t) ((t[1] << 8) | t[0]);
}

/* Reads the next code point, combining a surrogate pair; an unpaired high
 * surrogate is returned as-is. False at the end of the text or on a NUL unit. */
static bool id3_utf16_next(const uint8_t * text, size_t len, size_t * src, bool big_endian, uint32_t * cp) {
    if (*src + 1 >= len) return false;
    uint16_t unit = id3_utf16_unit(text + *src, big_endian);
    *src += 2;
    if (unit == 0) return false;

    *cp = unit;
    if (unit >= 0xD800 && unit <= 0xDBFF && *src + 1 < len) {
        uint16_t unit2 = id3_utf16_unit(text + *src, big_endian);
        if (unit2 >= 0xDC00 && unit2 <= 0xDFFF) {
            *cp = 0x10000 + (((uint32_t) unit - 0xD800) << 10) + (unit2 - 0xDC00);
            *src += 2;
        }
    }
    return true;
}

/* Writes cp as UTF-8 (1-4 bytes) and returns the length. */
static size_t utf8_encode(uint32_t cp, char * out) {
    if (cp < 0x80) {
        out[0] = (char) cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | (cp >> 18));
    out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

/* Decodes `len` bytes of ID3v2-encoded text into UTF-8. Handles ISO-8859-1/
 * UTF-8 (copied as-is) and UTF-16/UTF-16BE (converted, including surrogate
 * pairs) -- the encodings actually used in practice. Unsynchronization is not
 * undone; this covers the common case (tags written by mainstream taggers),
 * not every ID3v2 edge case. */
static void id3_decode_text(uint8_t encoding, const uint8_t * text, uint32_t len, char * out, size_t out_size) {
    out[0] = '\0';
    if (encoding == 0x00 || encoding == 0x03) {
        copy_bounded(out, out_size, (const char *) text, len);
        return;
    }

    bool big_endian;
    size_t src = id3_utf16_start(encoding, text, len, &big_endian);
    size_t out_pos = 0;
    uint32_t codepoint;
    while (out_pos + 4 < out_size && id3_utf16_next(text, len, &src, big_endian, &codepoint))
        out_pos += utf8_encode(codepoint, out + out_pos);
    out[out_pos] = '\0';
}

/* Same for a frame body whose first byte is the encoding byte. */
void decode_id3v2_text_frame(const uint8_t * data, uint32_t size, char * out, size_t out_size) {
    if (size == 0) {
        out[0] = '\0';
        return;
    }
    id3_decode_text(data[0], data + 1, size - 1, out, out_size);
}

/* Scans forward from `start` for the null terminator appropriate to an
 * ID3v2 text encoding byte (1-byte null for Latin-1/UTF-8, 2-byte null for
 * UTF-16/UTF-16BE) and returns the position right after it, or 0 if none
 * is found before `size` (0 is never a valid return otherwise, since
 * start is always >= 1 in every caller). */
static uint32_t id3v2_terminated_field_end(const uint8_t * data, uint32_t size, uint32_t start, uint8_t encoding) {
    bool wide_terminator = (encoding == 0x01 || encoding == 0x02);
    uint32_t pos = start;
    while (pos < size) {
        if (!wide_terminator && data[pos] == '\0') return pos + 1;
        if (wide_terminator && pos + 1 < size && data[pos] == '\0' && data[pos + 1] == '\0') return pos + 2;
        pos += wide_terminator ? 2 : 1;
    }
    return 0;
}

/* APIC frame body: 1 byte text encoding (applies to the description field
 * only -- MIME type is always Latin-1 regardless), null-terminated MIME
 * type string, 1 byte picture type, a description string terminated per
 * the encoding byte (1-byte null for Latin-1/UTF-8, 2-byte null for
 * UTF-16), then the raw picture bytes to the end of the frame. */
static void decode_id3v2_apic_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (out->picture_data != NULL || size < 2) return; /* keep the first picture found, same as FLAC */

    uint8_t encoding = data[0];
    uint32_t pos = 1;

    const uint8_t * mime_end = memchr(data + pos, '\0', size - pos);
    if (!mime_end) return;
    pos = (uint32_t) (mime_end - data) + 1;

    if (pos >= size) return;
    pos += 1; /* picture type byte */

    uint32_t desc_start = pos;
    pos = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (pos == 0 || pos <= desc_start || pos > size) return; /* malformed -- no terminator found */

    uint32_t picture_size = size - pos;
    if (picture_size == 0) return;

    store_picture_bytes(out, data + pos, picture_size);
}

/* TXXX frame body: 1 byte text encoding, a description string terminated
 * per the encoding byte, then a value string (encoded the same way) running
 * to the end of the frame with no terminator required. Taggers that write
 * ReplayGain into ID3v2 (rather than APEv2 or the binary RVA2 frame) use
 * this with descriptions "REPLAYGAIN_TRACK_GAIN"/"REPLAYGAIN_TRACK_PEAK", or
 * the ALBUM_ variants for whole-album gain/peak. */
static void decode_id3v2_txxx_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (size < 2) return;

    uint8_t encoding = data[0];
    uint32_t desc_start = 1;
    uint32_t value_start = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (value_start == 0 || value_start > size) return;

    char description[64];
    uint32_t desc_len = value_start - desc_start;
    if (desc_len > sizeof(description) - 1) desc_len = sizeof(description) - 1;
    id3_decode_text(encoding, data + desc_start, desc_len, description, sizeof(description));

    char value_str[40];
    uint32_t value_len = size - value_start;
    id3_decode_text(encoding, data + value_start, value_len, value_str, sizeof(value_str));

    if (strcasecmp(description, "ORIGINALYEAR") == 0 ||
        strcasecmp(description, "ORIGINALDATE") == 0)
        apply_original_year(out, value_str, strlen(value_str));

    apply_replaygain_field(out, description, strlen(description), value_str, strlen(value_str));
}

/* v2.2 PIC frame body: 1 byte text encoding, a FIXED 3-character image
 * format code (not a null-terminated MIME string like APIC's -- e.g.
 * "JPG"/"PNG", always exactly 3 bytes, no terminator), 1 byte picture
 * type, a description string terminated per the encoding byte, then the
 * raw picture bytes to the end of the frame. */
static void decode_id3v2_pic_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (out->picture_data != NULL || size < 5) return; /* keep the first picture found, same as APIC */

    uint8_t encoding = data[0];
    uint32_t pos = 1 + 3; /* encoding byte + fixed 3-char image format code */
    pos += 1; /* picture type byte */
    if (pos >= size) return;

    uint32_t desc_start = pos;
    pos = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (pos == 0 || pos <= desc_start || pos > size) return; /* malformed -- no terminator found */

    uint32_t picture_size = size - pos;
    if (picture_size == 0) return;

    store_picture_bytes(out, data + pos, picture_size);
}

/* USLT (Unsynchronised lyrics/text transcription) frame body: 1 byte text
 * encoding, a FIXED 3-byte language code (not null-terminated -- e.g. "eng",
 * always exactly 3 bytes), a short content-descriptor string terminated per
 * the encoding byte (almost always empty in practice), then the actual
 * lyrics text (encoded the same way as the description) running to the end
 * of the frame with no terminator required -- same overall shape as TXXX's
 * own description+value pair above, just with a fixed-width field first
 * instead of another terminated one. Despite the "Unsynchronised" name, some
 * taggers embed real [mm:ss.xx]-tagged LRC text here anyway -- the caller
 * (lyrics_load_thread_func(), gui_lyrics.c) tries lrc parsing first and falls back
 * to a plain unsynced display otherwise, same treatment an external .lrc
 * sidecar's own content would need if it turned out to have no timestamps. */
static void decode_id3v2_uslt_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (out->lyrics != NULL || size < 5) return; /* keep the first lyrics frame found, same as APIC's picture */

    uint8_t encoding = data[0];
    uint32_t desc_start = 1 + 3; /* encoding byte + fixed 3-byte language code */

    uint32_t text_start = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (text_start == 0 || text_start > size) return; /* malformed -- no terminator found */

    uint32_t text_len = size - text_start;
    if (text_len == 0 || text_len > LYRICS_MAX_FILE_BYTES) return;

    /* Lyrics are retained as complete text, so reject a frame whose decoded
     * form would exceed the sidecar limit instead of keeping a partial one. */
    size_t decoded_len = 0;
    if (encoding == 0x00 || encoding == 0x03) decoded_len = text_len;
    else if (encoding == 0x01 || encoding == 0x02) {
        const uint8_t * text = data + text_start;
        bool big_endian;
        size_t src = id3_utf16_start(encoding, text, text_len, &big_endian);
        uint32_t cp;
        char scratch[4];
        while (id3_utf16_next(text, text_len, &src, big_endian, &cp)) {
            decoded_len += utf8_encode(cp, scratch);
            if (decoded_len > LYRICS_MAX_FILE_BYTES) return;
        }
    } else return;
    if (decoded_len == 0 || decoded_len > LYRICS_MAX_FILE_BYTES) return;

    size_t out_size = decoded_len + 5U;
    char * lyrics = malloc(out_size);
    if (lyrics) {
        id3_decode_text(encoding, data + text_start, text_len, lyrics, out_size);
        if (lyrics[0] != '\0') out->lyrics = lyrics;
        else free(lyrics);
    }
}

static uint32_t id3_deunsync_in_place(uint8_t * data, uint32_t size) {
    uint32_t write_pos = 0;
    for (uint32_t read_pos = 0; read_pos < size; read_pos++) {
        uint8_t b = data[read_pos];
        data[write_pos++] = b;
        if (b == 0xFF && read_pos + 1 < size && data[read_pos + 1] == 0x00) read_pos++;
    }
    return write_pos;
}

typedef struct {
    FILE * f;
    uint32_t raw_remaining;
    uint32_t work_remaining;
    bool tag_unsync;
    bool previous_ff;
} id3_stream_t;

static bool id3_stream_read(id3_stream_t * s, uint8_t * out, uint32_t count) {
    uint32_t written = 0;
    while (written < count && s->raw_remaining > 0) {
        if (s->work_remaining == 0) return false;
        int c = fgetc(s->f);
        if (c == EOF) return false;
        s->raw_remaining--;
        s->work_remaining--;
        if (s->tag_unsync && s->previous_ff && c == 0x00) {
            s->previous_ff = false;
            continue;
        }
        out[written++] = (uint8_t) c;
        s->previous_ff = c == 0xFF;
    }
    return written == count;
}

static bool id3_stream_skip(id3_stream_t * s, uint32_t count) {
    if (!s->tag_unsync) {
        if (count > s->raw_remaining || fseeko(s->f, (off_t) count, SEEK_CUR) != 0) return false;
        s->raw_remaining -= count;
        s->previous_ff = false;
        return true;
    }
    uint8_t scratch[512];
    while (count > 0) {
        uint32_t n = count < sizeof(scratch) ? count : (uint32_t) sizeof(scratch);
        if (!id3_stream_read(s, scratch, n)) return false;
        count -= n;
    }
    return true;
}

/* ---- ID3v2 frame table: one id -> field mapping for every reader ---- */

typedef enum { ID3_KIND_TEXT, ID3_KIND_PICTURE, ID3_KIND_TXXX, ID3_KIND_LYRICS } id3_kind_t;
typedef enum {
    ID3_F_TITLE, ID3_F_ARTIST, ID3_F_ALBUM, ID3_F_ALBUM_ARTIST, ID3_F_GENRE,
    ID3_F_TRACK, ID3_F_DISC, ID3_F_YEAR, ID3_F_DATE, ID3_F_ORIGINAL_YEAR, ID3_F_NONE
} id3_field_t;

/* v22 is NULL for frames that do not exist in ID3v2.2. */
typedef struct {
    const char * v22;
    const char * v23;
    id3_kind_t kind;
    id3_field_t field;
} id3_frame_def_t;

static const id3_frame_def_t id3_frames[] = {
    { "TT2", "TIT2", ID3_KIND_TEXT, ID3_F_TITLE },
    { "TP1", "TPE1", ID3_KIND_TEXT, ID3_F_ARTIST },
    { "TAL", "TALB", ID3_KIND_TEXT, ID3_F_ALBUM },
    { "TP2", "TPE2", ID3_KIND_TEXT, ID3_F_ALBUM_ARTIST },
    { "TCO", "TCON", ID3_KIND_TEXT, ID3_F_GENRE },
    { "TRK", "TRCK", ID3_KIND_TEXT, ID3_F_TRACK },
    { "TPA", "TPOS", ID3_KIND_TEXT, ID3_F_DISC },
    { "TYE", "TYER", ID3_KIND_TEXT, ID3_F_YEAR },
    { NULL,  "TDRC", ID3_KIND_TEXT, ID3_F_DATE },
    { "TOR", "TORY", ID3_KIND_TEXT, ID3_F_ORIGINAL_YEAR },
    { NULL,  "TDOR", ID3_KIND_TEXT, ID3_F_ORIGINAL_YEAR },
    { "PIC", "APIC", ID3_KIND_PICTURE, ID3_F_NONE },
    { NULL,  "TXXX", ID3_KIND_TXXX, ID3_F_NONE },
    { NULL,  "USLT", ID3_KIND_LYRICS, ID3_F_NONE },
};

static const id3_frame_def_t * id3_frame_lookup(const char * id, bool v22) {
    for (size_t i = 0; i < sizeof(id3_frames) / sizeof(id3_frames[0]); i++) {
        const char * name = v22 ? id3_frames[i].v22 : id3_frames[i].v23;
        if (name && strcmp(id, name) == 0) return &id3_frames[i];
    }
    return NULL;
}

/* Decodes a text frame into a fixed-size field; presence follows the decoded
 * text being non-empty. */
static void id3_set_text(const uint8_t * payload, uint32_t n, char * dst, size_t dst_size, bool * has) {
    decode_id3v2_text_frame(payload, n, dst, dst_size);
    *has = dst[0] != '\0';
}
#define ID3_SET_TEXT(out, f, payload, n) id3_set_text((payload), (n), (out)->f, sizeof((out)->f), &(out)->has_##f)

#define ID3_NUMBER_FIELD_BYTES 32U
#define ID3_DATE_FIELD_BYTES 64U

static void id3_apply_text_field(id3_field_t field, const uint8_t * payload, uint32_t n, track_metadata_t * out) {
    char text[ID3_DATE_FIELD_BYTES];
    switch (field) {
    case ID3_F_TITLE: ID3_SET_TEXT(out, title, payload, n); break;
    case ID3_F_ARTIST: ID3_SET_TEXT(out, artist, payload, n); break;
    case ID3_F_ALBUM: ID3_SET_TEXT(out, album, payload, n); break;
    case ID3_F_ALBUM_ARTIST: ID3_SET_TEXT(out, album_artist, payload, n); break;
    case ID3_F_GENRE: ID3_SET_TEXT(out, genre, payload, n); break;
    case ID3_F_TRACK:
    case ID3_F_DISC:
        decode_id3v2_text_frame(payload, n, text, ID3_NUMBER_FIELD_BYTES);
        apply_sequence_text(out, field == ID3_F_DISC, text, strlen(text));
        break;
    case ID3_F_YEAR:
        decode_id3v2_text_frame(payload, n, text, ID3_NUMBER_FIELD_BYTES);
        apply_release_year(out, text, strlen(text), false);
        break;
    case ID3_F_DATE:
        decode_id3v2_text_frame(payload, n, text, ID3_DATE_FIELD_BYTES);
        apply_release_year(out, text, strlen(text), true);
        break;
    case ID3_F_ORIGINAL_YEAR:
        decode_id3v2_text_frame(payload, n, text, ID3_DATE_FIELD_BYTES);
        apply_original_year(out, text, strlen(text));
        break;
    case ID3_F_NONE:
        break;
    }
}

/* Applies one frame body (group/data-length prefixes already removed). */
static void id3_apply_frame(const id3_frame_def_t * def, bool v22, const uint8_t * payload, uint32_t n,
                            track_metadata_t * out) {
    switch (def->kind) {
    case ID3_KIND_TEXT: id3_apply_text_field(def->field, payload, n, out); break;
    case ID3_KIND_PICTURE:
        if (v22) decode_id3v2_pic_frame(payload, n, out);
        else decode_id3v2_apic_frame(payload, n, out);
        break;
    case ID3_KIND_TXXX: decode_id3v2_txxx_frame(payload, n, out); break;
    case ID3_KIND_LYRICS: decode_id3v2_uslt_frame(payload, n, out); break;
    }
}

/* Frame header: 3-char id + 24-bit size (v2.2) or 4-char id + 32-bit size
 * (synchsafe in v2.4) + 2 flag bytes. flags2 is the second flag byte (0 for
 * v2.2, which has none). */
static void id3_parse_frame_header(const uint8_t * hdr, uint8_t major_version, char id[5], uint32_t * size,
                                   uint8_t * flags2) {
    memset(id, 0, 5);
    if (major_version <= 2) {
        memcpy(id, hdr, 3);
        *size = ((uint32_t) hdr[3] << 16) | ((uint32_t) hdr[4] << 8) | hdr[5];
        *flags2 = 0;
    } else {
        memcpy(id, hdr, 4);
        *size = read_be32(hdr + 4, major_version >= 4);
        *flags2 = hdr[9];
    }
}

typedef struct {
    bool compressed, encrypted, grouped, unsync, data_len;
} id3_frame_flags_t;

/* Per-frame flag bits. v2.2 has none. In v2.3 a compressed frame carries a
 * data-length indicator and there is no per-frame unsynchronization. */
static id3_frame_flags_t id3_parse_frame_flags(uint8_t major_version, uint8_t flags2) {
    id3_frame_flags_t fl = {0};
    if (major_version >= 4) {
        fl.compressed = (flags2 & 0x08) != 0;
        fl.encrypted = (flags2 & 0x04) != 0;
        fl.grouped = (flags2 & 0x40) != 0;
        fl.unsync = (flags2 & 0x02) != 0;
        fl.data_len = (flags2 & 0x01) != 0;
    } else if (major_version == 3) {
        fl.compressed = (flags2 & 0x80) != 0;
        fl.encrypted = (flags2 & 0x40) != 0;
        fl.grouped = (flags2 & 0x20) != 0;
        fl.data_len = fl.compressed;
    }
    return fl;
}

/* Extended header total size (v2.4: includes the size field; v2.3: excludes
 * it), validated against the tag size. size4 is its leading size field. */
static bool id3_ext_header_total(const uint8_t size4[4], uint8_t major_version, uint32_t tag_size,
                                 uint32_t * total) {
    uint32_t ext_size = read_be32(size4, major_version >= 4);
    uint64_t ext_total = major_version >= 4 ? ext_size : (uint64_t) ext_size + 4U;
    uint64_t min_ext_total = major_version >= 4 ? 6U : 10U;
    if (ext_total < min_ext_total || ext_total > tag_size) return false;
    *total = (uint32_t) ext_total;
    return true;
}

/* Frame kinds the streaming reader retains: text and TXXX always, pictures and
 * lyrics only in the modes that want them. */
static bool id3_kind_wanted(id3_kind_t kind, bool include_blobs) {
    if (kind == ID3_KIND_PICTURE) return wants_picture(include_blobs);
    if (kind == ID3_KIND_LYRICS) return wants_lyrics(include_blobs);
    return true;
}

/* Largest raw frame body the streaming reader will allocate for a frame. */
static uint32_t id3_stream_frame_limit(const id3_frame_def_t * def) {
    if (def && (def->kind == ID3_KIND_TEXT || def->kind == ID3_KIND_TXXX)) return ID3_TEXT_FRAME_MAX_BYTES;
    if (def && def->kind == ID3_KIND_LYRICS) return LYRICS_MAX_FILE_BYTES;
    return METADATA_BLOB_MAX_BYTES;
}

/* Oversized tags are usually small text frames plus one very large APIC.
 * Walk frame headers directly from the file so that APIC can be skipped
 * without discarding title/artist/album. The file is bounded by tag_end on
 * every read and seek; no allocation is based on the overall tag size. */
static bool read_id3v2_streaming(FILE * f, track_metadata_t * out, uint8_t major_version,
                                 uint8_t tag_flags, uint32_t tag_size, bool include_blobs) {
    off_t body_start = ftello(f);
    if (body_start < 0 || body_start > INT64_MAX - (off_t) tag_size) return false;
    off_t tag_end = body_start + (off_t) tag_size;
    bool found_any = false;
    bool v22 = major_version <= 2;
    /* v2.2/v2.3 unsynchronization applies to the complete tag body, so keep
     * decoding bytes as the stream consumes them. In v2.4 frame headers,
     * extended-header sizes and frame sizes describe the stored bytes; parse
     * those boundaries raw and de-unsynchronize only each affected frame
     * body below. */
    bool legacy_tag_unsync = major_version < 4 && (tag_flags & 0x80) != 0;
    bool tag_unsync = (tag_flags & 0x80) != 0;
    id3_stream_t stream = { f, tag_size, ID3_STREAM_WORK_BUDGET_BYTES,
                            legacy_tag_unsync, false };

    if (major_version >= 3 && (tag_flags & 0x40)) {
        uint8_t ext[4];
        uint32_t ext_total;
        if (!id3_stream_read(&stream, ext, 4) || !id3_ext_header_total(ext, major_version, tag_size, &ext_total) ||
            !id3_stream_skip(&stream, ext_total - 4U)) return false;
    }

    for (uint32_t frame_count = 0; frame_count < ID3_FRAME_COUNT_MAX; frame_count++) {
        uint32_t header_size = v22 ? 6U : 10U;
        uint8_t hdr[10];
        if (!id3_stream_read(&stream, hdr, header_size)) break;
        if (hdr[0] == 0) break;

        char id[5];
        uint32_t frame_size;
        uint8_t flags2;
        id3_parse_frame_header(hdr, major_version, id, &frame_size, &flags2);
        const id3_frame_def_t * def = id3_frame_lookup(id, v22);
        id3_frame_flags_t fl = id3_parse_frame_flags(major_version, flags2);
        bool is_picture = def && def->kind == ID3_KIND_PICTURE;

        bool wanted = def && id3_kind_wanted(def->kind, include_blobs);
        if (wanted && !fl.compressed && !fl.encrypted && frame_size <= id3_stream_frame_limit(def) &&
            frame_size <= stream.raw_remaining) {
            uint8_t * data = malloc(frame_size ? frame_size : 1);
            bool read_ok = data && id3_stream_read(&stream, data, frame_size);
            if (read_ok) {
                uint32_t n = frame_size;
                /* v2.4's tag flag means every frame body is unsynchronized;
                 * a per-frame flag can also request it. The stored frame was
                 * read raw above, so apply the transform once even when both
                 * flags are set. Legacy v2.3 was already decoded by the
                 * stream reader and must not be decoded a second time. */
                if (major_version >= 4 && (tag_unsync || fl.unsync))
                    n = id3_deunsync_in_place(data, n);
                uint32_t prefix = (fl.grouped ? 1U : 0U) + (fl.data_len ? 4U : 0U);
                if (prefix <= n) {
                    id3_apply_frame(def, v22, data + prefix, n - prefix, out);
                    found_any = true;
                }
            }
            free(data);
            if (!read_ok) break;
        } else {
            /* A large APIC/PIC body is intentionally skipped instead of
             * allocated. Preserve that fact for artwork clients so an
             * oversized source is not reported as permanently missing. */
            if (wants_picture(include_blobs) && is_picture && frame_size > EMBEDDED_COVER_MAX_BYTES)
                out->picture_too_large = true;
            if (!id3_stream_skip(&stream, frame_size)) break;
        }
    }
    (void) fseeko(f, tag_end, SEEK_SET);
    return found_any;
}

/* ID3 is parsed inline for MP3/AAC scans, so reject a declared body that
 * extends past a regular, seekable file before walking frames. Non-seekable
 * streams retain the streaming reader's own byte bound. */
static bool id3_tag_fits_file(FILE * f, uint32_t tag_size) {
    off_t body_start = ftello(f);
    if (body_start < 0) return true;
    /* Regular files already expose their size through the open descriptor.
     * Avoid an EOF seek and a second seek back for every MP3/AAC tag; this
     * also keeps rejection independent of the card's end-of-file seek cost. */
    struct stat st;
    int fd = fileno(f);
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        return st.st_size >= 0 && (uint64_t) st.st_size >= (uint64_t) body_start &&
               (uint64_t) st.st_size - (uint64_t) body_start >= (uint64_t) tag_size;
    }
    if (fseeko(f, 0, SEEK_END) != 0) {
        clearerr(f);
        return fseeko(f, body_start, SEEK_SET) == 0;
    }
    off_t file_end = ftello(f);
    bool restored = fseeko(f, body_start, SEEK_SET) == 0;
    return restored && file_end >= body_start &&
           (uint64_t) (file_end - body_start) >= (uint64_t) tag_size;
}

/* Walks the frames of a fully buffered (and, for v2.2/v2.3, already
 * de-unsynchronized) tag from `pos`, the end of the optional extended header.
 * `flags` are the ID3v2 header flags. */
static bool id3_walk_buffered_frames(uint8_t * tag_data, uint32_t tag_size, uint32_t pos, uint8_t major_version,
                                     uint8_t flags, track_metadata_t * out) {
    bool found_any = false;
    bool v22 = major_version <= 2;
    uint32_t header_size = v22 ? 6U : 10U;

    for (uint32_t frame_count = 0;
         frame_count < ID3_FRAME_COUNT_MAX && pos + header_size <= tag_size; frame_count++) {
        if (tag_data[pos] == '\0') break; /* start of padding */

        char frame_id[5];
        uint32_t frame_size;
        uint8_t frame_flags2;
        id3_parse_frame_header(tag_data + pos, major_version, frame_id, &frame_size, &frame_flags2);
        pos += header_size;
        /* Bound frame_size against remaining tag size using subtraction to avoid 32-bit overflow. */
        if (frame_size > tag_size - pos) break;
        uint32_t next_pos = pos + frame_size; /* safe now -- frame_size <= tag_size - pos, just proven above */

        /* Unsupported compressed or encrypted frames are skipped. */
        id3_frame_flags_t fl = id3_parse_frame_flags(major_version, frame_flags2);
        if (!fl.compressed && !fl.encrypted) {
            uint8_t * frame_data = tag_data + pos;
            uint32_t frame_data_size = frame_size;

            /* Unsynchronization covers the complete stored frame body,
             * including group identity and DLI prefixes. Decode it before
             * removing those prefixes so an escaped 0xFF group byte cannot
             * shift the following DLI. */
            if (major_version >= 4 && ((flags & 0x80) || fl.unsync))
                frame_data_size = id3_deunsync_in_place(frame_data, frame_data_size);

            if (fl.grouped && frame_data_size >= 1) {
                frame_data += 1;
                frame_data_size -= 1;
            }
            if (fl.data_len && frame_data_size >= 4) {
                frame_data += 4;
                frame_data_size -= 4;
            }

            const id3_frame_def_t * def = id3_frame_lookup(frame_id, v22);
            if (def && !(def->kind == ID3_KIND_LYRICS && frame_data_size > LYRICS_MAX_FILE_BYTES)) {
                id3_apply_frame(def, v22, frame_data, frame_data_size, out);
                found_any = true;
            }
        }

        pos = next_pos;
    }

    return found_any;
}

/* Reads a whole tag of at most METADATA_BLOB_MAX_BYTES into memory and walks
 * it. */
static bool id3_read_buffered(FILE * f, track_metadata_t * out, uint8_t major_version, uint8_t flags,
                              uint32_t tag_size) {
    uint8_t * tag_data = malloc(tag_size);
    if (!tag_data) return false;
    if (fread(tag_data, 1, tag_size, f) != tag_size) {
        free(tag_data);
        return false;
    }

    /* v2.2/v2.3 unsynchronize the complete tag body. In v2.4, frame headers,
     * extended-header sizes and frame sizes stay in stored-byte coordinates;
     * each affected frame body is de-unsynchronized after it is bounded. */
    if (major_version < 4 && (flags & 0x80)) {
        tag_size = id3_deunsync_in_place(tag_data, tag_size);
    }

    uint32_t pos = 0;

    /* Skip the optional extended header if present (flags bit 0x40). */
    if (major_version >= 3 && (flags & 0x40)) {
        if (tag_size < 4 || !id3_ext_header_total(tag_data, major_version, tag_size, &pos)) {
            free(tag_data);
            return false;
        }
    }

    bool found_any = id3_walk_buffered_frames(tag_data, tag_size, pos, major_version, flags, out);
    free(tag_data);
    return found_any;
}

bool read_id3v2(FILE * f, track_metadata_t * out, bool include_blobs) {
    uint8_t header[10];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) return false;
    if (memcmp(header, "ID3", 3) != 0) return false;

    uint8_t major_version = header[3];
    uint8_t flags = header[5];
    if (major_version < 2 || major_version > 4) return false;
    uint32_t tag_size = read_be32(&header[6], true); /* overall tag size is always synchsafe */
    if (tag_size == 0) return false;
    if (!id3_tag_fits_file(f, tag_size)) return false;
    /* A database scan only needs bounded text frames. Always stream in that
     * mode so a large APIC/USLT body is skipped without allocating the
     * complete ID3 tag first. */
    if (artwork_only || lyrics_only || !include_blobs || tag_size > METADATA_BLOB_MAX_BYTES)
        return read_id3v2_streaming(f, out, major_version, flags, tag_size, include_blobs);
    return id3_read_buffered(f, out, major_version, flags, tag_size);
}

static void copy_id3v1_field(char * dst, size_t dst_size, const char * src, size_t src_len) {
    while (src_len > 0 && (src[src_len - 1] == ' ' || src[src_len - 1] == '\0')) src_len--;
    copy_bounded(dst, dst_size, src, src_len);
}

static bool read_id3v1(FILE * f, track_metadata_t * out) {
    if (fseek(f, -128, SEEK_END) != 0) return false;

    uint8_t tag[128];
    if (fread(tag, 1, sizeof(tag), f) != sizeof(tag)) return false;
    if (memcmp(tag, "TAG", 3) != 0) return false;

    copy_id3v1_field(out->title, sizeof(out->title), (const char *) tag + 3, 30);
    copy_id3v1_field(out->artist, sizeof(out->artist), (const char *) tag + 33, 30);
    copy_id3v1_field(out->album, sizeof(out->album), (const char *) tag + 63, 30);
    out->has_title = out->title[0] != '\0';
    out->has_artist = out->artist[0] != '\0';
    out->has_album = out->album[0] != '\0';
    if (tag[125] == 0 && tag[126] != 0) set_sequence(out, false, tag[126]);
    apply_release_year(out, (const char *) tag + 93, 4, false);
    return true;
}

/* MP3 (v1_year_fallback) and raw AAC (.aac, ADTS/ADIF) tags: a prepended
 * ID3v2 tag, else a trailing ID3v1 one. MP3 additionally takes the ID3v1
 * year when the ID3v2 tag has none; AAC does not. */
static void read_id3_file(const char * path, track_metadata_t * out, bool include_blobs, bool v1_year_fallback) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    if (!read_id3v2(f, out, include_blobs)) read_id3v1(f, out);
    else if (v1_year_fallback && !out->has_release_year) {
        /* ID3v1 remains a low-priority year fallback even when an ID3v2 tag
         * supplied the other display metadata. */
        track_metadata_t legacy = {0};
        if (read_id3v1(f, &legacy) && legacy.has_release_year) {
            out->release_year = legacy.release_year;
            out->has_release_year = true;
        }
    }

    fclose(f);
}

void read_mp3_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    read_id3_file(path, out, include_blobs, true);
}

void read_aac_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    read_id3_file(path, out, include_blobs, false);
}
