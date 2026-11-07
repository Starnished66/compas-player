#define _FILE_OFFSET_BITS 64
#include "metadata.h"
#include "metadata_parser_internal.h"
#include "../../../core/text/utf8_util.h"
#include "../../artwork/support/library_endian.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

/* Moves to the next 2-byte-aligned IFF-style chunk after data_start + size.
 * Fails (so the walker stops) on an overflowing size or when the next chunk
 * does not start after chunk_start, which prevents infinite loops on
 * corrupted files. */
static bool chunk_seek_next(FILE * f, off_t chunk_start, off_t data_start, uint64_t size) {
    off_t next;
    return safe_file_advance(data_start, size, (off_t) (size & 1), &next) && next > chunk_start &&
           fseeko(f, next, SEEK_SET) == 0;
}

static uint64_t read_u64le(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}


/* True for a FORM AIFF/AIFC header, or a RIFF/RF64 WAVE one when aiff is
 * false. Sets *rf64 for RF64, whose chunks match RIFF but whose "data" size
 * may be 0xFFFFFFFF with the real size in the "ds64" chunk. */
static bool iff_header_ok(const uint8_t * header, bool aiff, bool * rf64) {
    if (aiff) return memcmp(header, "FORM", 4) == 0 && (memcmp(header + 8, "AIFF", 4) == 0 || memcmp(header + 8, "AIFC", 4) == 0);
    *rf64 = memcmp(header, "RF64", 4) == 0;
    return (*rf64 || memcmp(header, "RIFF", 4) == 0) && memcmp(header + 8, "WAVE", 4) == 0;
}

/* AIFF/AIFC ("FORM", big-endian chunk sizes, "ID3 " chunk), WAV
 * ("RIFF"/"WAVE", little-endian sizes, "id3 "/"ID3 " chunk, often placed after
 * the audio data) and RF64 embedded ID3v2 reader: walks the chunks up to the
 * first ID3 one. */
static void read_iff_id3(const char * path, track_metadata_t * out, bool include_blobs, bool aiff) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t header[12];
    bool rf64 = false;
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || !iff_header_ok(header, aiff, &rf64)) {
        fclose(f);
        return;
    }

    uint64_t rf64_data_size = 0;
    while (1) {
        off_t chunk_start = ftello(f); /* before this iteration's own header read -- see chunk_seek_next() */
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header)) break;

        char chunk_id[5] = { 0 };
        memcpy(chunk_id, chunk_header, 4);
        uint64_t chunk_size = aiff ? read_be32(chunk_header + 4, false) : library_read_u32le(chunk_header + 4);
        off_t chunk_data_start = ftello(f);

        if (aiff ? strcmp(chunk_id, "ID3 ") == 0 : strcasecmp(chunk_id, "id3 ") == 0) {
            read_id3v2(f, out, include_blobs);
            break;
        }
        if (rf64 && strcmp(chunk_id, "ds64") == 0) {
            uint8_t ds64[16]; /* riff size, data size */
            if (fread(ds64, 1, sizeof(ds64), f) == sizeof(ds64)) rf64_data_size = read_u64le(ds64 + 8);
        } else if (rf64 && chunk_size == 0xFFFFFFFFu && strcmp(chunk_id, "data") == 0) {
            chunk_size = rf64_data_size;
        }

        if (!chunk_seek_next(f, chunk_start, chunk_data_start, chunk_size)) break;
    }

    fclose(f);
}

void read_aiff_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    read_iff_id3(path, out, include_blobs, true);
}

/* DSF metadata parser. Reads the ID3v2 metadataPointer offset from the "DSD "
 * master chunk and parses the ID3v2 tag. */
void read_dsf_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t dsd_chunk[28];
    if (fread(dsd_chunk, 1, sizeof(dsd_chunk), f) != sizeof(dsd_chunk) || memcmp(dsd_chunk, "DSD ", 4) != 0) {
        fclose(f);
        return;
    }

    uint64_t metadata_pointer = read_u64le(&dsd_chunk[20]);
    if (metadata_pointer != 0 && fseeko(f, (off_t) metadata_pointer, SEEK_SET) == 0) {
        read_id3v2(f, out, include_blobs);
    }

    fclose(f);
}

/* APEv2 key -> field matching (case-insensitive Title/Artist/Album/Year/
 * Track/Disc text items). */
static void apply_title_artist_album_field(track_metadata_t * out, const char * key, size_t key_len,
                                            const char * value, size_t value_len) {
    if (key_len == 5 && strncasecmp(key, "Title", 5) == 0) {
        SET_TAG_TEXT(out, title, value, value_len);
    } else if (key_len == 6 && strncasecmp(key, "Artist", 6) == 0) {
        SET_TAG_TEXT(out, artist, value, value_len);
    } else if (key_len == 5 && strncasecmp(key, "Album", 5) == 0) {
        SET_TAG_TEXT(out, album, value, value_len);
    } else if (key_len == 4 && strncasecmp(key, "Year", 4) == 0) {
        apply_release_year(out, value, value_len, false);
    } else if ((key_len == 5 && strncasecmp(key, "Track", 5) == 0) ||
               (key_len == 11 && strncasecmp(key, "TrackNumber", 11) == 0)) {
        apply_sequence_text(out, false, value, value_len);
    } else if ((key_len == 4 && strncasecmp(key, "Disc", 4) == 0) ||
               (key_len == 10 && strncasecmp(key, "DiscNumber", 10) == 0)) {
        apply_sequence_text(out, true, value, value_len);
    }
}

/* Finds a trailing APEv2 footer and positions the file at the first item. */
static bool ape_locate_trailing(FILE * f, uint32_t * item_count) {
    if (fseeko(f, 0, SEEK_END) != 0) return false;
    off_t file_end = ftello(f);
    if (file_end < 32) return false;

    /* Some files carry a legacy 128-byte ID3v1 tag appended AFTER the
     * APEv2 tag -- skip past it if present so the footer search below
     * looks at the actual end of the APEv2 tag, not the ID3v1 trailer. */
    off_t effective_end = file_end;
    if (file_end >= 128) {
        uint8_t id3v1_magic[3];
        if (fseeko(f, file_end - 128, SEEK_SET) == 0 && fread(id3v1_magic, 1, 3, f) == 3 &&
            memcmp(id3v1_magic, "TAG", 3) == 0) {
            effective_end = file_end - 128;
        }
    }
    if (effective_end < 32) return false;

    uint8_t footer[32];
    if (fseeko(f, effective_end - 32, SEEK_SET) != 0 || fread(footer, 1, sizeof(footer), f) != sizeof(footer) ||
        memcmp(footer, "APETAGEX", 8) != 0)
        return false;

    uint32_t tag_size = library_read_u32le(&footer[12]);
    *item_count = library_read_u32le(&footer[16]);
    if (tag_size < 32 || (off_t) tag_size > effective_end) return false;

    off_t items_start = effective_end - (off_t) tag_size;
    return fseeko(f, items_start, SEEK_SET) == 0;
}

/* Finds an APEv2 tag at the start of the file (header flag set); the items
 * follow its 32-byte header. */
static bool ape_locate_leading(FILE * f, uint32_t * item_count) {
    uint8_t header[32];
    if (fseeko(f, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "APETAGEX", 8) != 0 || !(library_read_u32le(&header[20]) & (1u << 29)))
        return false;
    *item_count = library_read_u32le(&header[16]);
    return true;
}

static bool ape_locate_items(FILE * f, uint32_t * item_count) {
    return ape_locate_trailing(f, item_count) || ape_locate_leading(f, item_count);
}

/* Reads one item's NUL-terminated key, draining any part longer than the buffer
 * so the value that follows stays aligned. Returns false at end of file. */
static bool ape_read_key(FILE * f, char * key, size_t cap, size_t * key_len) {
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\0') {
        if (n + 1 < cap) key[n++] = (char) c;
    }
    key[n] = '\0';
    *key_len = n;
    return c != EOF;
}

/* ---- APE (Monkey's Audio): APEv2 footer tag -- no existing helper to
 * reuse, ape_demux.c only ever parses the file-start descriptor/header for
 * decoding, never anything at the file's end. Spec layout: a 32-byte
 * footer (magic "APETAGEX" + version u32LE + tag_size u32LE, which counts
 * the items plus this footer itself but never the file's actual end if a
 * legacy trailing ID3v1 tag also follows + item_count u32LE + flags u32LE
 * + 8 reserved bytes), preceded by item_count items of
 * { value_length u32LE, item_flags u32LE, null-terminated key, raw value }.
 * A walk reads one kind of item, UTF-8 text (item_flags bits 1-2 == 0) or
 * binary (bits 1-2 == 1), and skips the rest without reading them. */
static void ape_walk_items(const char * path, bool binary, metadata_text_item_fn item, void * ctx) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint32_t item_count;
    if (!ape_locate_items(f, &item_count)) {
        fclose(f);
        return;
    }

    /* Defensive caps: no real text value is near 1MB; covers use the artwork cap. */
    uint32_t cap = binary ? EMBEDDED_COVER_MAX_BYTES : (1u << 20);
    for (uint32_t i = 0; i < item_count; i++) {
        uint8_t item_header[8];
        if (fread(item_header, 1, sizeof(item_header), f) != sizeof(item_header)) break;
        uint32_t value_length = library_read_u32le(&item_header[0]);
        uint32_t kind = library_read_u32le(&item_header[4]) & 0x6; /* 0 = UTF-8 text, 2 = binary */

        char key[64];
        size_t key_len;
        if (!ape_read_key(f, key, sizeof(key), &key_len)) break;

        bool wanted = value_length > 0 && value_length < cap && kind == (binary ? 2u : 0u);
        if (!wanted) {
            if (fseeko(f, (off_t) value_length, SEEK_CUR) != 0) break;
            continue;
        }
        char * value = malloc(value_length);
        bool ok = value && fread(value, 1, value_length, f) == value_length;
        if (ok) item(ctx, key, key_len, value, value_length);
        free(value);
        if (!ok) break;
    }

    fclose(f);
}

void metadata_ape_text_items(const char * path, metadata_text_item_fn item, void * ctx) {
    ape_walk_items(path, false, item, ctx);
}

static void ape_apply_item(void * ctx, const char * key, size_t key_len, const char * value, size_t value_len) {
    track_metadata_t * out = ctx;
    apply_title_artist_album_field(out, key, key_len, value, value_len);
    /* APEv2 stores ReplayGain keys using the same naming as Vorbis comments. */
    apply_replaygain_field(out, key, key_len, value, value_len);
}

/* "Cover Art (Front)" binary item: file name, NUL, then the image bytes. */
static void ape_cover_item(void * ctx, const char * key, size_t key_len, const char * value, size_t value_len) {
    if (key_len != 17 || strncasecmp(key, "Cover Art (Front)", 17) != 0) return;
    const char * name_end = memchr(value, '\0', value_len);
    if (!name_end) return;
    size_t offset = (size_t) (name_end - value) + 1;
    store_picture_bytes(ctx, (const uint8_t *) value + offset, (uint32_t) (value_len - offset));
}

void read_ape_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    metadata_ape_text_items(path, ape_apply_item, out);
    if (wants_picture(include_blobs)) ape_walk_items(path, true, ape_cover_item, out);
}

/* ---- DFF (DSDIFF): real-world files use two different, incompatible
 * tagging conventions depending on which tool wrote them -- many modern
 * taggers (dBpoweramp, foobar2000, ...) embed a non-standard top-level
 * "ID3 " chunk, exactly AIFF's own convention above, even though it isn't
 * part of the official Philips DSDIFF spec; the spec's own native
 * mechanism is a "DIIN" chunk containing "DIAR" (artist)/"DITI" (title)
 * sub-chunks (plain strings, no ID3-style text-encoding byte). Tries the
 * ID3 chunk first (richer -- covers title/artist/album in one read_id3v2()
 * call), falling back to DIIN/DIAR/DITI only if no "ID3 " chunk was found
 * anywhere in the file. Independent chunk walk from dsd_decoder.c's own
 * open_dff() (big-endian 64-bit chunk sizes, unlike AIFF's 32-bit) --
 * that one stops once channels/sample_rate/data are all found, which
 * happens before any trailing "ID3 "/"DIIN" chunk would be reached. */
static bool dff_read_chunk_header(FILE * f, char id_out[5], uint64_t * size_out) {
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) return false;
    memcpy(id_out, header, 4);
    id_out[4] = '\0';
    *size_out = read_u64be(header + 4); /* DSDIFF chunk sizes are big-endian */
    return true;
}

/* Reads the spec-native DIIN sub-chunks (DIAR artist, DITI title). */
static void dff_read_diin(FILE * f, off_t diin_data_start, off_t diin_end, track_metadata_t * out) {
    if (fseeko(f, diin_data_start, SEEK_SET) != 0) return;
    while (ftello(f) < diin_end) {
        char sub_id[5];
        uint64_t sub_size;
        off_t sub_chunk_start = ftello(f); /* see chunk_seek_next() */
        if (!dff_read_chunk_header(f, sub_id, &sub_size)) break;
        off_t sub_data_start = ftello(f);

        if ((strcmp(sub_id, "DIAR") == 0 || strcmp(sub_id, "DITI") == 0) && sub_size > 0 &&
            sub_size < sizeof(out->title)) {
            char text[128];
            if (fread(text, 1, (size_t) sub_size, f) == sub_size) {
                if (strcmp(sub_id, "DIAR") == 0) SET_TAG_TEXT(out, artist, text, (size_t) sub_size);
                else SET_TAG_TEXT(out, title, text, (size_t) sub_size);
            }
        }

        if (!chunk_seek_next(f, sub_chunk_start, sub_data_start, sub_size)) break;
    }
}

void read_dff_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    char frm8_id[5];
    uint64_t frm8_size;
    uint8_t form_type[4];
    if (!dff_read_chunk_header(f, frm8_id, &frm8_size) || strcmp(frm8_id, "FRM8") != 0 ||
        fread(form_type, 1, sizeof(form_type), f) != sizeof(form_type) || memcmp(form_type, "DSD ", 4) != 0) {
        fclose(f);
        return;
    }

    off_t diin_data_start = -1;
    off_t diin_end = -1;

    while (1) {
        char id[5];
        uint64_t size;
        off_t chunk_start = ftello(f); /* position BEFORE this iteration's own header read -- next must clear this, or a corrupted size could seek back here and loop forever */
        if (!dff_read_chunk_header(f, id, &size)) break;
        off_t chunk_data_start = ftello(f);

        if (strcmp(id, "ID3 ") == 0) {
            read_id3v2(f, out, include_blobs);
            fclose(f);
            return;
        }
        if (strcmp(id, "DIIN") == 0 && diin_data_start < 0) {
            /* Safely compute diin_end without overflow. */
            off_t candidate_end;
            if (safe_file_advance(chunk_data_start, size, 0, &candidate_end)) {
                diin_data_start = chunk_data_start;
                diin_end = candidate_end;
            }
        }

        if (!chunk_seek_next(f, chunk_start, chunk_data_start, size)) break;
    }

    /* No "ID3 " chunk anywhere -- fall back to the spec-native DIIN
     * sub-chunks, if a DIIN chunk was seen during the walk above. */
    if (diin_data_start >= 0) dff_read_diin(f, diin_data_start, diin_end, out);

    fclose(f);
}

/* ---- WMA: ASF Content Description + Extended Content Description
 * objects. asf_demux.c already has exactly this shape for decoding
 * (GUID_HEADER et al., asf_object_header_t, read_object_header(), and
 * asf_demux_open()'s own top-level object walk) but everything there is
 * static to that file, so this is an independent walk in the same shape --
 * same reasoning as DFF/AIFF above. GUIDs are stored in ASF on-disk byte
 * order: the first three fields (uint32, uint16, uint16) little-endian, the
 * final 8 bytes as-is -- the same layout asf_demux.c uses for GUID_HEADER et
 * al. and what fread() delivers here. */
static const uint8_t GUID_CONTENT_DESCRIPTION[16] = {
    /* {75B22633-668E-11CF-A6D9-00AA0062CE6C} */
    0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};
static const uint8_t GUID_EXT_CONTENT_DESCRIPTION[16] = {
    /* {D2D0A440-E307-11D2-97F0-00A0C95EA850} */
    0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50
};
static const uint8_t GUID_HEADER_EXTENSION[16] = {
    /* {5FBF03B5-A92E-11CF-8EE3-00C00C205365} */
    0xB5, 0x03, 0xBF, 0x5F, 0x2E, 0xA9, 0xCF, 0x11, 0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65
};
static const uint8_t GUID_METADATA[16] = {
    /* {C5F8CBEA-5BAF-4877-8467-AA8C44FA4CCA} */
    0xEA, 0xCB, 0xF8, 0xC5, 0xAF, 0x5B, 0x77, 0x48, 0x84, 0x67, 0xAA, 0x8C, 0x44, 0xFA, 0x4C, 0xCA
};
static const uint8_t GUID_METADATA_LIBRARY[16] = {
    /* {44231C94-9498-49D1-A141-1D134E457054} */
    0x94, 0x1C, 0x23, 0x44, 0x98, 0x94, 0xD1, 0x49, 0xA1, 0x41, 0x1D, 0x13, 0x4E, 0x45, 0x70, 0x54
};
static const uint8_t GUID_ASF_HEADER[16] = {
    /* {75B22630-668E-11CF-A6D9-00AA0062CE6C} */
    0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};

/* Reuses decode_id3v2_text_frame() for the UTF-16LE -> UTF-8 conversion
 * rather than a second implementation: encoding byte 0x01 ("UTF-16 with
 * optional BOM") already treats absent-BOM input as little-endian by
 * default -- its big_endian flag only flips true on an actual 0xFEFF BOM
 * -- which is exactly ASF's raw no-BOM UTF-16LE convention. Decodes at most
 * a small stack buffer (no real WMA tag field is anywhere near this long)
 * and, after a full read, skips the rest of the field so the stream ends up
 * just past all byte_len bytes. */
static void decode_asf_utf16le(FILE * f, uint16_t byte_len, char * out, size_t out_size) {
    uint8_t buf[257];
    buf[0] = 0x01;
    uint16_t read_len = byte_len > sizeof(buf) - 1 ? (uint16_t) (sizeof(buf) - 1) : byte_len;
    if (fread(buf + 1, 1, read_len, f) != read_len) {
        out[0] = '\0';
        return;
    }
    decode_id3v2_text_frame(buf, (uint32_t) read_len + 1, out, out_size);
    if (byte_len > read_len) fseeko(f, (off_t) byte_len - read_len, SEEK_CUR);
}

/* True when n more bytes fit before the object's end. */
static bool asf_fits(FILE * f, off_t end, uint64_t n) {
    off_t pos = ftello(f);
    return pos >= 0 && pos <= end && n <= (uint64_t) (end - pos);
}

static bool asf_read(FILE * f, off_t end, void * buf, size_t n) {
    return asf_fits(f, end, n) && fread(buf, 1, n, f) == n;
}

/* Content Description object: title and author as UTF-16LE. data_start and
 * end are the file offsets of the object's data. */
static void asf_read_content_description(FILE * f, off_t data_start, off_t end, track_metadata_t * out) {
    uint8_t lens[10]; /* 5 x u16LE lengths: title, author, copyright, description, rating */
    if (!asf_read(f, end, lens, sizeof(lens))) return;
    uint16_t title_len = library_read_u16le(&lens[0]);
    uint16_t author_len = library_read_u16le(&lens[2]);
    if (title_len > 0 && asf_fits(f, end, title_len)) {
        decode_asf_utf16le(f, title_len, out->title, sizeof(out->title));
        out->has_title = out->title[0] != '\0';
    }
    /* Absolute reposition to the author field. */
    if (fseeko(f, data_start + 10 + title_len, SEEK_SET) == 0 && author_len > 0 && asf_fits(f, end, author_len)) {
        decode_asf_utf16le(f, author_len, out->artist, sizeof(out->artist));
        out->has_artist = out->artist[0] != '\0';
    }
}

/* State shared by the ASF object handlers. */
typedef struct {
    track_metadata_t * out;
    bool want_picture;
    bool front; /* the stored picture is a front cover */
} asf_ctx_t;

/* Skips a UTF-16LE string including its 2-byte terminator; NULL if unterminated. */
static const uint8_t * asf_skip_utf16z(const uint8_t * p, const uint8_t * end) {
    for (; end - p >= 2; p += 2) {
        if (p[0] == 0 && p[1] == 0) return p + 2;
    }
    return NULL;
}

/* WM/Picture value: type byte, u32LE data length, UTF-16LE MIME and
 * description (both NUL-terminated), then the image. Keeps the front cover
 * (type 3), else the first picture. */
static void asf_store_picture(asf_ctx_t * c, const uint8_t * v, size_t n) {
    if (n < 5) return;
    bool front = v[0] == 3;
    if (c->out->picture_data && (!front || c->front)) return;
    const uint8_t * p = asf_skip_utf16z(v + 5, v + n);
    p = p ? asf_skip_utf16z(p, v + n) : NULL;
    uint32_t size = library_read_u32le(v + 1);
    if (!p || size > (size_t) (v + n - p)) return;
    uint8_t * previous = c->out->picture_data;
    c->out->picture_data = NULL;
    if (store_picture_bytes(c->out, p, size)) {
        free(previous);
        c->front = front;
    } else {
        c->out->picture_data = previous;
    }
}

/* Reads a WM/Picture byte-array value of len bytes. Returns false, with the
 * value unread, when the attribute is something else. */
static bool asf_read_picture_value(FILE * f, asf_ctx_t * c, const char * name, uint16_t type, uint32_t len) {
    if (!c->want_picture || type != 1 || len == 0 || len > EMBEDDED_COVER_MAX_BYTES + 4096u /* image plus MIME/description */ ||
        strcasecmp(name, "WM/Picture") != 0)
        return false;
    uint8_t * value = malloc(len);
    if (!value) return false; /* the caller skips the unread value */
    if (fread(value, 1, len, f) == len) asf_store_picture(c, value, len);
    free(value);
    return true;
}

/* Extended Content Description object: WM/AlbumTitle and WM/Picture. */
static void asf_read_ext_content_description(FILE * f, asf_ctx_t * c, off_t end) {
    uint8_t count_field[2];
    if (!asf_read(f, end, count_field, sizeof(count_field))) return;
    uint16_t desc_count = library_read_u16le(count_field);
    for (uint16_t d = 0; d < desc_count; d++) {
        uint8_t name_len_buf[2];
        if (!asf_read(f, end, name_len_buf, 2)) break;
        uint16_t name_len = library_read_u16le(name_len_buf);
        if (!asf_fits(f, end, name_len)) break;
        char name[64];
        decode_asf_utf16le(f, name_len, name, sizeof(name));

        uint8_t type_len_buf[4];
        if (!asf_read(f, end, type_len_buf, 4)) break;
        uint16_t value_type = library_read_u16le(&type_len_buf[0]);
        uint16_t value_len = library_read_u16le(&type_len_buf[2]);
        if (!asf_fits(f, end, value_len)) break;

        if (asf_read_picture_value(f, c, name, value_type, value_len)) continue;
        if (value_type == 0 && value_len > 0 && strcasecmp(name, "WM/AlbumTitle") == 0) {
            decode_asf_utf16le(f, value_len, c->out->album, sizeof(c->out->album));
            c->out->has_album = c->out->album[0] != '\0';
        } else if (fseeko(f, value_len, SEEK_CUR) != 0) {
            break;
        }
    }
}

/* Metadata or Metadata Library object: records of reserved/language u16,
 * stream u16, name length u16, type u16, value length u32, name, value. Large
 * pictures live here because the extended description caps values at 64KB. */
static void asf_read_metadata_records(FILE * f, asf_ctx_t * c, off_t end) {
    uint8_t count_field[2];
    if (!c->want_picture || !asf_read(f, end, count_field, sizeof(count_field))) return;
    uint16_t count = library_read_u16le(count_field);
    for (uint16_t i = 0; i < count; i++) {
        uint8_t rec[12];
        if (!asf_read(f, end, rec, sizeof(rec))) break;
        uint16_t name_len = library_read_u16le(&rec[4]);
        if (!asf_fits(f, end, name_len)) break;
        char name[64];
        decode_asf_utf16le(f, name_len, name, sizeof(name));
        uint32_t len = library_read_u32le(&rec[8]);
        if (!asf_fits(f, end, len)) break;
        if (asf_read_picture_value(f, c, name, library_read_u16le(&rec[6]), len)) continue;
        if (fseeko(f, (off_t) len, SEEK_CUR) != 0) break;
    }
}

typedef void (*asf_object_fn)(FILE * f, asf_ctx_t * c, const uint8_t * guid, off_t data_start, off_t end);

/* Walks consecutive objects (GUID, u64LE size including this 24-byte header)
 * from pos, at most max_objects of them and none starting at or past limit. */
static void asf_walk_objects(FILE * f, asf_ctx_t * c, off_t pos, off_t limit, uint32_t max_objects, asf_object_fn fn) {
    for (uint32_t i = 0; i < max_objects && limit - pos >= 24; i++) {
        uint8_t guid[16];
        uint8_t size_buf[8];
        if (fseeko(f, pos, SEEK_SET) != 0 || fread(guid, 1, 16, f) != 16 || fread(size_buf, 1, 8, f) != 8) break;
        uint64_t size = read_u64le(size_buf);
        off_t data_start = ftello(f);
        off_t next;
        if (size < 24 || !safe_file_advance(data_start, size - 24, 0, &next) || next > limit) break;
        fn(f, c, guid, data_start, next);
        pos = next;
    }
}

static void asf_on_extension_object(FILE * f, asf_ctx_t * c, const uint8_t * guid, off_t data_start, off_t end) {
    (void) data_start;
    if (memcmp(guid, GUID_METADATA, 16) == 0 || memcmp(guid, GUID_METADATA_LIBRARY, 16) == 0)
        asf_read_metadata_records(f, c, end);
}

/* Header Extension object: 16 reserved GUID bytes, u16, u32LE data size,
 * then nested objects. */
static void asf_read_header_extension(FILE * f, asf_ctx_t * c, off_t data_start, off_t end) {
    uint8_t head[22];
    if (!c->want_picture || fseeko(f, data_start, SEEK_SET) != 0 || !asf_read(f, end, head, sizeof(head))) return;
    off_t limit = data_start + (off_t) sizeof(head) + (off_t) library_read_u32le(&head[18]);
    asf_walk_objects(f, c, data_start + (off_t) sizeof(head), limit < end ? limit : end, UINT32_MAX, asf_on_extension_object);
}

static void asf_on_object(FILE * f, asf_ctx_t * c, const uint8_t * guid, off_t data_start, off_t end) {
    if (memcmp(guid, GUID_CONTENT_DESCRIPTION, 16) == 0) {
        asf_read_content_description(f, data_start, end, c->out);
    } else if (memcmp(guid, GUID_EXT_CONTENT_DESCRIPTION, 16) == 0) {
        asf_read_ext_content_description(f, c, end);
    } else if (memcmp(guid, GUID_HEADER_EXTENSION, 16) == 0) {
        asf_read_header_extension(f, c, data_start, end);
    }
}

void read_wma_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t top[30]; /* GUID, u64LE size, num_objects u32LE, 2 reserved bytes */
    if (fread(top, 1, sizeof(top), f) == sizeof(top) && memcmp(top, GUID_ASF_HEADER, 16) == 0) {
        asf_ctx_t ctx = { out, wants_picture(include_blobs), false };
        uint64_t header_size = read_u64le(&top[16]);
        off_t limit = header_size > INT64_MAX ? INT64_MAX : (off_t) header_size;
        asf_walk_objects(f, &ctx, (off_t) sizeof(top), limit, library_read_u32le(&top[24]), asf_on_object);
    }

    fclose(f);
}

/* ---- WAV: one RIFF walk for INFO and embedded ID3 ---- */

static void wav_info_title(track_metadata_t * out, const uint8_t * raw, size_t len) {
    if (len >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF))) {
        uint8_t tagged[2 * sizeof(out->title) + 3];
        size_t n = len < sizeof(tagged) - 1 ? len : sizeof(tagged) - 1;
        n &= ~(size_t) 1;
        tagged[0] = 1;
        memcpy(tagged + 1, raw, n);
        decode_id3v2_text_frame(tagged, (uint32_t) n + 1, out->title, sizeof(out->title));
        if (!utf8_is_valid(out->title, strlen(out->title))) out->title[0] = '\0';
    } else {
        if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) { raw += 3; len -= 3; }
        if (!utf8_is_valid((const char *) raw, strnlen((const char *) raw, len))) {
            out->title[0] = '\0';
            out->has_title = false;
            return;
        }
        copy_bounded(out->title, sizeof(out->title), (const char *) raw, len);
    }
    out->has_title = out->title[0] != '\0';
}

static void wav_read_info(FILE * f, uint64_t size, track_metadata_t * info) {
    off_t end;
    uint8_t kind[4];
    if (size < 4 || !safe_file_advance(ftello(f), size, 0, &end) ||
        fread(kind, 1, sizeof(kind), f) != sizeof(kind) || memcmp(kind, "INFO", 4) != 0) return;

    while (ftello(f) >= 0 && end - ftello(f) >= 8) {
        off_t start = ftello(f);
        uint8_t header[8];
        if (fread(header, 1, sizeof(header), f) != sizeof(header)) break;
        uint32_t len = library_read_u32le(header + 4);
        off_t data = ftello(f);
        if ((uint64_t) (end - data) < len) break;
        bool wanted = memcmp(header, "INAM", 4) == 0 || memcmp(header, "IART", 4) == 0 ||
                      memcmp(header, "IPRD", 4) == 0 || memcmp(header, "ITRK", 4) == 0;
        /* INFO strings are small; ignore unreasonable fields without allocating them. */
        if (wanted && len > 0 && len <= (1u << 20)) {
            uint8_t * text = malloc((size_t) len + 1);
            if (text && fread(text, 1, len, f) == len) {
                text[len] = 0;
                if (memcmp(header, "INAM", 4) == 0) wav_info_title(info, text, len);
                else if (memcmp(header, "IART", 4) == 0) SET_TAG_TEXT(info, artist, (char *) text, len);
                else if (memcmp(header, "IPRD", 4) == 0) SET_TAG_TEXT(info, album, (char *) text, len);
                else apply_sequence_text(info, false, (char *) text, len);
            }
            free(text);
        }
        if (!chunk_seek_next(f, start, data, len)) break;
    }
}

void read_wav_all(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;
    uint8_t header[12];
    bool rf64 = false;
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || !iff_header_ok(header, false, &rf64)) {
        fclose(f);
        return;
    }
    track_metadata_t info = { 0 };
    uint64_t rf64_data_size = 0;
    bool read_id3 = false;
    while (1) {
        off_t start = ftello(f);
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), f) != sizeof(chunk)) break;
        uint64_t size = library_read_u32le(chunk + 4);
        off_t data = ftello(f);
        if (strncasecmp((const char *) chunk, "LIST", 4) == 0) {
            wav_read_info(f, size, &info);
        } else if (!read_id3 && strncasecmp((const char *) chunk, "id3 ", 4) == 0) {
            read_id3v2(f, out, include_blobs);
            read_id3 = true;
        } else if (rf64 && memcmp(chunk, "ds64", 4) == 0 && size >= 16) {
            uint8_t ds64[16];
            if (fread(ds64, 1, sizeof(ds64), f) == sizeof(ds64)) rf64_data_size = read_u64le(ds64 + 8);
        } else if (rf64 && size == 0xFFFFFFFFu && memcmp(chunk, "data", 4) == 0) {
            size = rf64_data_size;
        }
        if (!chunk_seek_next(f, start, data, size)) break;
    }
    fclose(f);

    /* ID3 text is Unicode; INFO can contain already-lost legacy characters. */
    if (!out->has_title && info.has_title) SET_TAG_TEXT(out, title, info.title, strlen(info.title));
    if (!out->has_artist && info.has_artist) SET_TAG_TEXT(out, artist, info.artist, strlen(info.artist));
    if (!out->has_album && info.has_album) SET_TAG_TEXT(out, album, info.album, strlen(info.album));
    if (!out->has_track_number && info.has_track_number) set_sequence(out, false, info.track_number);
}
