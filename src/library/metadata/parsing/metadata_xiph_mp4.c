#define _FILE_OFFSET_BITS 64
#include "metadata.h"
#include "metadata_parser_internal.h"
#include "../../artwork/support/library_endian.h"

#include "../../../audio/engine/audio_dr_config.h" /* before dr_flac.h: same struct drflac layout as the engine */
#include "dr_flac.h"
#include "../../../audio/decoders/ogg/opusfile_alloc.h"
#include "../../../audio/decoders/wavpack/wavpack_decoder.h"
#include "../../../audio/decoders/caf/caf_decoder.h"
#include "stb_vorbis.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- FLAC: VORBIS_COMMENT block ---- */

/* Full dr_flac parse: only reached when pictures are wanted (see
 * read_flac_metadata()), so the picture block is always taken and lyrics
 * are not pre-filtered. */
static void flac_meta_cb(void * user_data, drflac_metadata * meta) {
    track_metadata_t * out = (track_metadata_t *) user_data;

    if (meta->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
        /* dr_flac frees its own copy of pPictureData right after this
         * callback returns, so it has to be copied here, not just
         * pointed to. */
        if (out->picture_data == NULL && meta->data.picture.pictureDataSize > 0 &&
            meta->data.picture.pPictureData != NULL) {
            store_picture_bytes(out, meta->data.picture.pPictureData, meta->data.picture.pictureDataSize);
        }
        return;
    }

    if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;

    drflac_vorbis_comment_iterator iter;
    drflac_init_vorbis_comment_iterator(&iter, meta->data.vorbis_comment.commentCount, meta->data.vorbis_comment.pComments);

    drflac_uint32 comment_len;
    const char * comment;
    while ((comment = drflac_next_vorbis_comment(&iter, &comment_len)) != NULL) {
        apply_vorbis_comment_field(out, comment, comment_len, true);
    }
}

/* Reads the 4-byte stream marker, skipping any ID3v2 tags in front of it.
 * Some taggers prepend a plain ID3v2 tag before "fLaC"; dr_flac's own
 * drflac_init() skips it too, so this lightweight walker must as well or an
 * ID3-prefixed FLAC loses all its text tags in no-artwork/lyrics-only mode. */
static bool flac_text_stream_start(FILE * f) {
    uint8_t sig[4];
    for (;;) {
        if (fread(sig, 1, 4, f) != 4) return false;
        if (memcmp(sig, "ID3", 3) != 0) break;
        uint8_t rest[6];
        if (fread(rest, 1, sizeof(rest), f) != sizeof(rest)) return false;
        uint32_t tag_size = read_be32(&rest[2], true);
        if (rest[1] & 0x10) tag_size += 10; /* footer present */
        if (fseeko(f, (off_t) tag_size, SEEK_CUR) != 0) return false;
    }
    return memcmp(sig, "fLaC", 4) == 0;
}

/* Applies every comment of one VORBIS_COMMENT block body. */
static void flac_apply_comment_block(const uint8_t * block, uint32_t n, track_metadata_t * out, bool include_blobs) {
    if (n < 8) return;
    uint32_t pos = 4;
    uint32_t vendor = library_read_u32le(block);
    if (vendor > n - pos - 4) return;
    pos += vendor;
    uint32_t count = library_read_u32le(&block[pos]);
    pos += 4;
    for (uint32_t i = 0; i < count && pos + 4 <= n; i++) {
        uint32_t len = library_read_u32le(&block[pos]);
        pos += 4;
        if (len > n - pos) break;
        apply_vorbis_comment_field(out, (const char *) &block[pos], len, include_blobs);
        pos += len;
    }
}

static void read_flac_text_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;
    if (!flac_text_stream_start(f)) { fclose(f); return; }

    bool last = false;
    while (!last) {
        uint8_t h[4];
        if (fread(h, 1, 4, f) != 4) break;
        last = (h[0] & 0x80) != 0;
        uint32_t n = ((uint32_t) (h[1] << 16) | ((uint32_t) h[2] << 8) | h[3]);
        if ((h[0] & 0x7f) != 4) {
            if (fseeko(f, (off_t) n, SEEK_CUR) != 0) break;
            continue;
        }
        if (n > METADATA_BLOB_MAX_BYTES) break;
        uint8_t * block = malloc(n ? n : 1);
        if (!block || fread(block, 1, n, f) != n) { free(block); break; }
        flac_apply_comment_block(block, n, out, include_blobs);
        free(block);
    }
    fclose(f);
}

void read_flac_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    if (lyrics_only || !include_blobs) {
        read_flac_text_metadata(path, out, include_blobs);
        return;
    }
    drflac * flac = drflac_open_file_with_metadata(path, flac_meta_cb, out, NULL);
    if (flac) drflac_close(flac);
}

/* ---- M4A (ALAC or AAC container): moov/udta/meta/ilst iTunes-style atoms ----
 *
 * This is a separate, minimal box walker rather than a reuse of
 * mp4_demux.c's (audio.c's demuxer only tracks the moov/trak/.../stbl path
 * needed to locate compressed audio samples, has no reason to know about
 * udta/meta/ilst, and its helpers are file-local statics anyway). */

typedef struct {
    char type[5];
    uint64_t size;
    int header_size;
    off_t data_start;
} m4a_box_t;

static bool m4a_read_box_header(FILE * f, m4a_box_t * out) {
    off_t start = ftello(f);
    if (start < 0) return false;
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;

    uint32_t size32 = read_be32(hdr, false);
    memcpy(out->type, hdr + 4, 4);
    out->type[4] = '\0';

    if (size32 == 1) {
        uint8_t ext[8];
        if (fread(ext, 1, sizeof(ext), f) != sizeof(ext)) return false;
        out->size = read_u64be(ext);
        out->header_size = 16;
    } else if (size32 == 0) {
        off_t cur = ftello(f);
        if (cur < 0 || fseeko(f, 0, SEEK_END) != 0) return false;
        off_t end = ftello(f);
        if (end < start || fseeko(f, cur, SEEK_SET) != 0) return false;
        out->size = (uint64_t) (end - start);
        out->header_size = 8;
    } else {
        out->size = size32;
        out->header_size = 8;
    }

    /* Reject malformed boxes smaller than their own header. */
    if (out->size < (uint64_t) out->header_size) return false;

    /* Validate the still-unsigned box size before casting or adding it,
     * otherwise a corrupt extended-size atom can wrap the next seek
     * backwards and keep the scanner walking the same bytes forever. */
    off_t box_end;
    if (!safe_file_advance(start, out->size, 0, &box_end)) return false;

    out->data_start = start + out->header_size;
    return true;
}

static bool m4a_box_end_within(const m4a_box_t * box, off_t container_end, off_t * out_end) {
    if (!box || box->data_start < box->header_size || container_end < 0) return false;
    off_t box_start = box->data_start - box->header_size;
    off_t box_end;
    if (!safe_file_advance(box_start, box->size, 0, &box_end)) return false;
    if (box_end < box->data_start || box_end > container_end) return false;
    if (out_end) *out_end = box_end;
    return true;
}

static bool m4a_find_child(FILE * f, off_t container_start, uint64_t container_size, const char * type, m4a_box_t * out) {
    off_t end;
    if (!safe_file_advance(container_start, container_size, 0, &end) ||
        fseeko(f, container_start, SEEK_SET) != 0)
        return false;

    for (;;) {
        off_t pos = ftello(f);
        if (pos < 0 || pos >= end) break;
        m4a_box_t box;
        if (!m4a_read_box_header(f, &box)) return false;
        off_t next;
        if (!m4a_box_end_within(&box, end, &next)) return false;
        if (strcmp(box.type, type) == 0) {
            *out = box;
            return true;
        }
        if (fseeko(f, next, SEEK_SET) != 0) return false;
    }
    return false;
}

/* Every ilst item (©nam/©ART/©alb/covr/...) wraps its actual value in a
 * nested "data" box: a FullBox-shaped header (type indicator(4) + locale(4)
 * -- always seen as zeroed/type 1 or 13/14 in practice, not otherwise
 * inspected here) followed immediately by the raw value bytes. */
static bool m4a_find_data_payload(FILE * f, m4a_box_t parent, off_t * out_offset, uint32_t * out_size) {
    if (parent.size < (uint64_t) parent.header_size) return false;
    m4a_box_t data_box;
    if (!m4a_find_child(f, parent.data_start, parent.size - (uint64_t) parent.header_size, "data", &data_box)) return false;
    if (data_box.size < (uint64_t) data_box.header_size + 8) return false;
    uint64_t payload_size = data_box.size - (uint64_t) data_box.header_size - 8;
    if (payload_size > UINT32_MAX || data_box.data_start > INT64_MAX - 8) return false;
    *out_offset = data_box.data_start + 8;
    *out_size = (uint32_t) payload_size;
    return true;
}

static void m4a_read_text_tag(FILE * f, m4a_box_t item, char * dst, size_t dst_size, bool * has_flag) {
    off_t payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size == 0) return;

    uint32_t n = (payload_size > dst_size - 1) ? (uint32_t) (dst_size - 1) : payload_size;
    fseeko(f, payload_offset, SEEK_SET);
    if (fread(dst, 1, n, f) != n) return;
    dst[n] = '\0';
    *has_flag = true;
}

static void m4a_read_sequence_tag(FILE * f, m4a_box_t item, bool disc, track_metadata_t * out) {
    off_t payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size < 4) return;
    uint8_t data[8] = {0};
    uint32_t n = payload_size < sizeof(data) ? payload_size : (uint32_t) sizeof(data);
    if (fseeko(f, payload_offset, SEEK_SET) != 0 || fread(data, 1, n, f) != n) return;
    /* iTunes trkn/disk payload: two reserved bytes followed by the
     * big-endian current track/disc number (then total count). */
    int value = ((int) data[2] << 8) | data[3];
    if (value <= 0) return;
    set_sequence(out, disc, value);
}

static void m4a_read_cover_art(FILE * f, m4a_box_t item, track_metadata_t * out) {
    if (out->picture_data != NULL) return; /* keep the first picture found, same as FLAC/MP3 */

    off_t payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size == 0) return;
    /* Audiobook/M4B covers are often multi-megabyte JPEGs. Unbounded
     * malloc here races the decoder's own RAM on the 56 MiB target. */
    if (payload_size > EMBEDDED_COVER_MAX_BYTES) {
        out->picture_too_large = true;
        return;
    }

    uint8_t * data = malloc(payload_size);
    if (!data) return;
    fseeko(f, payload_offset, SEEK_SET);
    if (fread(data, 1, payload_size, f) == payload_size) {
        out->picture_data = data;
        out->picture_size = payload_size;
    } else {
        free(data);
    }
}

/* "\xA9lyr" atom -- same nested "data" box shape as every other ilst item,
 * plain UTF-8 text (no encoding byte, unlike ID3v2's USLT) -- same malloc'd-
 * buffer-plus-NUL-terminator shape as m4a_read_cover_art() just above, text
 * instead of binary. May or may not contain [mm:ss.xx] LRC timestamps -- see
 * track_metadata_t's own lyrics field comment. */
static void m4a_read_lyrics_tag(FILE * f, m4a_box_t item, track_metadata_t * out) {
    if (out->lyrics != NULL) return; /* keep the first found, same as cover art */

    off_t payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size == 0) return;
    if (payload_size > METADATA_BLOB_MAX_BYTES) return;

    char * data = malloc((size_t) payload_size + 1);
    if (!data) return;
    fseeko(f, payload_offset, SEEK_SET);
    if (fread(data, 1, payload_size, f) == payload_size) {
        data[payload_size] = '\0';
        out->lyrics = data;
    } else {
        free(data);
    }
}

/* Locates moov/udta/meta/ilst, leaving the file position unspecified. */
static bool m4a_find_ilst(FILE * f, m4a_box_t * ilst) {
    if (fseeko(f, 0, SEEK_END) != 0) return false;
    off_t file_size = ftello(f);
    if (file_size < 0 || fseeko(f, 0, SEEK_SET) != 0) return false;

    m4a_box_t moov;
    bool found_moov = false;
    for (;;) {
        off_t pos = ftello(f);
        if (pos < 0 || pos >= file_size) break;
        m4a_box_t box;
        if (!m4a_read_box_header(f, &box)) break;
        off_t next;
        if (!m4a_box_end_within(&box, file_size, &next)) break;
        if (strcmp(box.type, "moov") == 0) { moov = box; found_moov = true; break; }
        if (fseeko(f, next, SEEK_SET) != 0) break;
    }
    if (!found_moov) return false;

    m4a_box_t udta, meta;
    if (!m4a_find_child(f, moov.data_start, moov.size - (uint64_t) moov.header_size, "udta", &udta)) return false;
    if (!m4a_find_child(f, udta.data_start, udta.size - (uint64_t) udta.header_size, "meta", &meta)) return false;

    /* Unlike every other container walked here, top-level "meta" is itself a
     * FullBox -- its children start 4 bytes (version+flags) further in
     * than a plain container's would. */
    if (meta.size < (uint64_t) meta.header_size + 4 || meta.data_start > INT64_MAX - 4) return false;
    off_t meta_children_start = meta.data_start + 4;
    uint64_t meta_children_size = meta.size - (uint64_t) meta.header_size - 4;
    return m4a_find_child(f, meta_children_start, meta_children_size, "ilst", ilst);
}

static void m4a_apply_ilst_item(FILE * f, m4a_box_t item, track_metadata_t * out, bool include_blobs) {
    if (strcmp(item.type, "\xA9" "nam") == 0) {
        m4a_read_text_tag(f, item, out->title, sizeof(out->title), &out->has_title);
    } else if (strcmp(item.type, "\xA9" "ART") == 0) {
        m4a_read_text_tag(f, item, out->artist, sizeof(out->artist), &out->has_artist);
    } else if (strcmp(item.type, "\xA9" "alb") == 0) {
        m4a_read_text_tag(f, item, out->album, sizeof(out->album), &out->has_album);
    } else if (strcmp(item.type, "aART") == 0) {
        m4a_read_text_tag(f, item, out->album_artist, sizeof(out->album_artist), &out->has_album_artist);
    } else if (strcmp(item.type, "\xA9" "gen") == 0) {
        m4a_read_text_tag(f, item, out->genre, sizeof(out->genre), &out->has_genre);
    } else if (strcmp(item.type, "\xA9" "day") == 0) {
        char date[64] = {0};
        bool have_date = false;
        m4a_read_text_tag(f, item, date, sizeof(date), &have_date);
        if (have_date) apply_release_year(out, date, strlen(date), true);
    } else if (strcmp(item.type, "trkn") == 0) {
        m4a_read_sequence_tag(f, item, false, out);
    } else if (strcmp(item.type, "disk") == 0) {
        m4a_read_sequence_tag(f, item, true, out);
    } else if (wants_picture(include_blobs) && strcmp(item.type, "covr") == 0) {
        m4a_read_cover_art(f, item, out);
    } else if (wants_lyrics(include_blobs) && strcmp(item.type, "\xA9" "lyr") == 0) {
        m4a_read_lyrics_tag(f, item, out);
    }
}

void read_m4a_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    m4a_box_t ilst;
    if (!m4a_find_ilst(f, &ilst)) { fclose(f); return; }

    off_t ilst_end;
    if (ilst.size < (uint64_t) ilst.header_size ||
        !safe_file_advance(ilst.data_start, ilst.size - (uint64_t) ilst.header_size, 0, &ilst_end) ||
        fseeko(f, ilst.data_start, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    for (;;) {
        off_t pos = ftello(f);
        if (pos < 0 || pos >= ilst_end) break;
        m4a_box_t item;
        if (!m4a_read_box_header(f, &item)) break;
        off_t next;
        if (!m4a_box_end_within(&item, ilst_end, &next)) break;

        m4a_apply_ilst_item(f, item, out, include_blobs);

        if (fseeko(f, next, SEEK_SET) != 0) break;
    }

    fclose(f);
}

/* ---- Opus: OpusTags (Ogg comment header, RFC 7845 5.2) ---- */

typedef struct {
    track_metadata_t * metadata;
    bool include_blobs;
} library_tags_context_t;

/* Library tag APIs expose separate keys/values; reuse the existing bounded
 * field matcher so sequencing, ReplayGain and lyrics follow the same rules. */
static void apply_library_tag(void * user, const char * key, const char * value) {
    library_tags_context_t * ctx = user;
    if (!key || !value) return;
    if (strcasecmp(key, "Track") == 0) key = "TRACKNUMBER";
    else if (strcasecmp(key, "Disc") == 0 || strcasecmp(key, "Disc Number") == 0) key = "DISCNUMBER";
    else if (strcasecmp(key, "Album Artist") == 0) key = "ALBUMARTIST";
    size_t key_len = strnlen(key, 64);
    size_t value_len = strnlen(value, METADATA_BLOB_MAX_BYTES + 1U);
    if (key_len == 64 || value_len > METADATA_BLOB_MAX_BYTES) return;
    char * comment = malloc(key_len + 1 + value_len);
    if (!comment) return;
    memcpy(comment, key, key_len);
    comment[key_len] = '=';
    memcpy(comment + key_len + 1, value, value_len);
    apply_vorbis_comment_field(ctx->metadata, comment, key_len + 1 + value_len, ctx->include_blobs);
    free(comment);
}

void read_wavpack_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    library_tags_context_t ctx = { out, include_blobs };
    wavpack_get_tags(path, apply_library_tag, &ctx);
    if (wants_picture(include_blobs)) {
        size_t image_size = 0;
        uint8_t * image = wavpack_get_cover(path, METADATA_BLOB_MAX_BYTES, &image_size);
        if (image && image_size > 0 && image_size <= METADATA_BLOB_MAX_BYTES) {
            out->picture_data = image;
            out->picture_size = (uint32_t) image_size;
        } else {
            free(image);
            if (image_size > METADATA_BLOB_MAX_BYTES) out->picture_too_large = true;
        }
    }
}

void read_caf_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    library_tags_context_t ctx = { out, include_blobs };
    caf_get_tags(path, apply_library_tag, &ctx);
}

void read_opus_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    /* Partial open parses OpusHead/OpusTags but skips the full-file seek
     * index and decoder setup used for playback. libopusfile owns the tag
     * strings, so consumers can inspect their authoritative lengths without
     * making another copy. */
    OggOpusFile *file = compas_opusfile_open(path, true);
    if (!file) return;

    const OpusTags *tags = op_tags(file, 0);
    int count = tags && tags->comments > 0 && tags->user_comments &&
                tags->comment_lengths ? tags->comments : 0;
    for (int i = 0; i < count; i++) {
        int raw_len = tags->comment_lengths[i];
        if (raw_len < 0 || !tags->user_comments[i]) continue;
        size_t comment_len = (size_t)raw_len;
        const char *comment = tags->user_comments[i];

        if (!apply_vorbis_picture_comment(out, comment, comment_len, include_blobs))
            apply_vorbis_comment_field(out, comment, comment_len, include_blobs);
    }

    op_free(file);
}

/* ---- Ogg Vorbis (.ogg): comment header, same RFC-ish "vendor string +
 * KEY=VALUE list" layout as Opus's own OpusTags above (Vorbis comments are
 * in fact where that layout originated -- RFC 7845 5.2 explicitly reuses
 * the Vorbis I spec's own comment format). Reuses vorbis_decoder.h's stb_
 * vorbis wrapper's underlying library directly (stb_vorbis_get_comment()).
 * stb_vorbis already performs Ogg demux internally, so there is no need to
 * duplicate that work here. This opens the decoder for metadata, as with
 * other formats in this file that reuse a full decoder-open. */
void read_ogg_vorbis_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    int error = 0;
    stb_vorbis * f = stb_vorbis_open_filename(path, &error, NULL);
    if (!f) return;

    stb_vorbis_comment comment = stb_vorbis_get_comment(f);
    for (int i = 0; i < comment.comment_list_length; i++) {
        const char * field = comment.comment_list[i];
        if (!field) continue;
        size_t field_len = strlen(field);

        if (!apply_vorbis_picture_comment(out, field, field_len, include_blobs))
            apply_vorbis_comment_field(out, field, field_len, include_blobs);
    }

    stb_vorbis_close(f);
}
