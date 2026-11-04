#include "metadata.h"
#include "metadata_parser_internal.h"
#include "../../../core/text/utf8_util.h"
#include "../../artwork/support/library_endian.h"

#include "dr_wav.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Moves to the next 2-byte-aligned IFF-style chunk after data_start + size.
 * Fails (so the walker stops) on an overflowing size or when the next chunk
 * does not start after chunk_start, which prevents infinite loops on
 * corrupted files. */
static bool chunk_seek_next(FILE * f, long chunk_start, long data_start, uint64_t size) {
    long next;
    return safe_chunk_advance(data_start, size, (long) (size & 1), &next) && next > chunk_start &&
           fseek(f, next, SEEK_SET) == 0;
}

static uint64_t read_u64le(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}


/* Unmarked LIST/INFO text is not guaranteed to be UTF-8. Preserve valid text,
 * but record when a title needed sanitizing so an embedded ID3 title
 * can recover it. This does not try to guess a legacy code page. */
static bool is_valid_utf8(const char * text, size_t size) {
    return utf8_is_valid(text, text ? strnlen(text, size) : size);
}

/* AIFF/AIFC ("FORM", big-endian chunk sizes, "ID3 " chunk) and WAV
 * ("RIFF"/"WAVE", little-endian sizes, "id3 "/"ID3 " chunk, often placed after
 * the audio data) embedded ID3v2 reader: walks the chunks up to the first ID3
 * one. */
static void read_iff_id3(const char * path, track_metadata_t * out, bool include_blobs, bool aiff) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, aiff ? "FORM" : "RIFF", 4) != 0 ||
        (aiff ? (memcmp(header + 8, "AIFF", 4) != 0 && memcmp(header + 8, "AIFC", 4) != 0)
              : memcmp(header + 8, "WAVE", 4) != 0)) {
        fclose(f);
        return;
    }

    while (1) {
        long chunk_start = ftell(f); /* before this iteration's own header read -- see chunk_seek_next() */
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header)) break;

        char chunk_id[5] = { 0 };
        memcpy(chunk_id, chunk_header, 4);
        uint32_t chunk_size = aiff ? read_be32(chunk_header + 4, false) : library_read_u32le(chunk_header + 4);
        long chunk_data_start = ftell(f);

        if (aiff ? strcmp(chunk_id, "ID3 ") == 0 : strcasecmp(chunk_id, "id3 ") == 0) {
            read_id3v2(f, out, include_blobs);
            break;
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
    if (metadata_pointer != 0 && fseek(f, (long) metadata_pointer, SEEK_SET) == 0) {
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

/* Finds the APEv2 footer and positions the file at the first item. */
static bool ape_locate_items(FILE * f, uint32_t * item_count) {
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long file_end = ftell(f);
    if (file_end < 32) return false;

    /* Some files carry a legacy 128-byte ID3v1 tag appended AFTER the
     * APEv2 tag -- skip past it if present so the footer search below
     * looks at the actual end of the APEv2 tag, not the ID3v1 trailer. */
    long effective_end = file_end;
    if (file_end >= 128) {
        uint8_t id3v1_magic[3];
        if (fseek(f, file_end - 128, SEEK_SET) == 0 && fread(id3v1_magic, 1, 3, f) == 3 &&
            memcmp(id3v1_magic, "TAG", 3) == 0) {
            effective_end = file_end - 128;
        }
    }
    if (effective_end < 32) return false;

    uint8_t footer[32];
    if (fseek(f, effective_end - 32, SEEK_SET) != 0 || fread(footer, 1, sizeof(footer), f) != sizeof(footer) ||
        memcmp(footer, "APETAGEX", 8) != 0)
        return false;

    uint32_t tag_size = library_read_u32le(&footer[12]);
    *item_count = library_read_u32le(&footer[16]);
    if (tag_size < 32 || (long) tag_size > effective_end) return false;

    long items_start = effective_end - (long) tag_size;
    return fseek(f, items_start, SEEK_SET) == 0;
}

/* ---- APE (Monkey's Audio): APEv2 footer tag -- no existing helper to
 * reuse, ape_demux.c only ever parses the file-start descriptor/header for
 * decoding, never anything at the file's end. Spec layout: a 32-byte
 * footer (magic "APETAGEX" + version u32LE + tag_size u32LE, which counts
 * the items plus this footer itself but never the file's actual end if a
 * legacy trailing ID3v1 tag also follows + item_count u32LE + flags u32LE
 * + 8 reserved bytes), preceded by item_count items of
 * { value_length u32LE, item_flags u32LE, null-terminated key, raw value }.
 * Only text items (item_flags bits 1-2 == 0) are read as UTF-8 text. */
void metadata_ape_text_items(const char * path, metadata_text_item_fn item, void * ctx) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint32_t item_count;
    if (!ape_locate_items(f, &item_count)) {
        fclose(f);
        return;
    }

    for (uint32_t i = 0; i < item_count; i++) {
        uint8_t item_header[8];
        if (fread(item_header, 1, sizeof(item_header), f) != sizeof(item_header)) break;
        uint32_t value_length = library_read_u32le(&item_header[0]);
        uint32_t item_flags = library_read_u32le(&item_header[4]);

        char key[64];
        size_t key_len = 0;
        int c;
        while (key_len + 1 < sizeof(key) && (c = fgetc(f)) != EOF && c != '\0') key[key_len++] = (char) c;
        key[key_len] = '\0';
        if (c == EOF) break;
        /* A key longer than this buffer (no real-world tag uses one) still
         * needs its remaining bytes drained before the value, or every
         * subsequent item in the tag would be misaligned. */
        while (c != '\0') {
            c = fgetc(f);
            if (c == EOF) break;
        }

        if (value_length > 0 && value_length < (1u << 20)) { /* defensive cap -- no real tag value is anywhere near 1MB */
            char * value = malloc(value_length);
            if (value && fread(value, 1, value_length, f) == value_length) {
                if ((item_flags & 0x6) == 0) { /* bits 1-2 of item_flags: 0 = UTF-8 text */
                    item(ctx, key, key_len, value, value_length);
                }
            }
            free(value);
        } else if (fseek(f, (long) value_length, SEEK_CUR) != 0) {
            break;
        }
    }

    fclose(f);
}

static void ape_apply_item(void * ctx, const char * key, size_t key_len, const char * value, size_t value_len) {
    track_metadata_t * out = ctx;
    apply_title_artist_album_field(out, key, key_len, value, value_len);
    /* APEv2 stores ReplayGain keys using the same naming as Vorbis comments. */
    apply_replaygain_field(out, key, key_len, value, value_len);
}

void read_ape_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    (void) include_blobs; /* APE tags carry no artwork or lyrics here */
    metadata_ape_text_items(path, ape_apply_item, out);
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
static void dff_read_diin(FILE * f, long diin_data_start, long diin_end, track_metadata_t * out) {
    if (fseek(f, diin_data_start, SEEK_SET) != 0) return;
    while (ftell(f) < diin_end) {
        char sub_id[5];
        uint64_t sub_size;
        long sub_chunk_start = ftell(f); /* see chunk_seek_next() */
        if (!dff_read_chunk_header(f, sub_id, &sub_size)) break;
        long sub_data_start = ftell(f);

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

    long diin_data_start = -1;
    long diin_end = -1;

    while (1) {
        char id[5];
        uint64_t size;
        long chunk_start = ftell(f); /* position BEFORE this iteration's own header read -- next must clear this, or a corrupted size could seek back here and loop forever */
        if (!dff_read_chunk_header(f, id, &size)) break;
        long chunk_data_start = ftell(f);

        if (strcmp(id, "ID3 ") == 0) {
            read_id3v2(f, out, include_blobs);
            fclose(f);
            return;
        }
        if (strcmp(id, "DIIN") == 0 && diin_data_start < 0) {
            /* Safely compute diin_end without overflow. */
            long candidate_end;
            if (safe_chunk_advance(chunk_data_start, size, 0, &candidate_end)) {
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
    if (byte_len > read_len) fseek(f, (long) byte_len - read_len, SEEK_CUR);
}

/* Content Description object: title and author as UTF-16LE. data_start is the
 * file offset of the object's data. */
static void asf_read_content_description(FILE * f, long data_start, track_metadata_t * out) {
    uint8_t lens[10]; /* 5 x u16LE lengths: title, author, copyright, description, rating */
    if (fread(lens, 1, sizeof(lens), f) != sizeof(lens)) return;
    uint16_t title_len = library_read_u16le(&lens[0]);
    uint16_t author_len = library_read_u16le(&lens[2]);
    if (title_len > 0) {
        decode_asf_utf16le(f, title_len, out->title, sizeof(out->title));
        out->has_title = out->title[0] != '\0';
    }
    /* Absolute reposition to the author field. */
    if (fseek(f, data_start + 10 + title_len, SEEK_SET) == 0 && author_len > 0) {
        decode_asf_utf16le(f, author_len, out->artist, sizeof(out->artist));
        out->has_artist = out->artist[0] != '\0';
    }
}

/* Extended Content Description object: only WM/AlbumTitle is read. */
static void asf_read_ext_content_description(FILE * f, track_metadata_t * out) {
    uint8_t count_field[2];
    if (fread(count_field, 1, sizeof(count_field), f) != sizeof(count_field)) return;
    uint16_t desc_count = library_read_u16le(count_field);
    for (uint16_t d = 0; d < desc_count; d++) {
        uint8_t name_len_buf[2];
        if (fread(name_len_buf, 1, 2, f) != 2) break;
        uint16_t name_len = library_read_u16le(name_len_buf);
        char name[64];
        decode_asf_utf16le(f, name_len, name, sizeof(name));

        uint8_t type_len_buf[4];
        if (fread(type_len_buf, 1, 4, f) != 4) break;
        uint16_t value_type = library_read_u16le(&type_len_buf[0]);
        uint16_t value_len = library_read_u16le(&type_len_buf[2]);

        if (value_type == 0 && value_len > 0 && strcasecmp(name, "WM/AlbumTitle") == 0) {
            decode_asf_utf16le(f, value_len, out->album, sizeof(out->album));
            out->has_album = out->album[0] != '\0';
        } else if (fseek(f, value_len, SEEK_CUR) != 0) {
            break;
        }
    }
}

void read_wma_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    (void) include_blobs; /* ASF tags carry no artwork or lyrics here */
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t top_guid[16];
    uint8_t size_buf[8];
    if (fread(top_guid, 1, 16, f) != 16 || memcmp(top_guid, GUID_ASF_HEADER, 16) != 0 ||
        fread(size_buf, 1, 8, f) != 8) {
        fclose(f);
        return;
    }

    uint8_t count_buf[6]; /* num_objects u32LE + 2 reserved bytes */
    if (fread(count_buf, 1, sizeof(count_buf), f) != sizeof(count_buf)) {
        fclose(f);
        return;
    }
    uint32_t num_objects = library_read_u32le(count_buf);

    long pos = ftell(f);
    for (uint32_t i = 0; i < num_objects; i++) {
        if (fseek(f, pos, SEEK_SET) != 0) break;

        uint8_t sub_guid[16];
        uint8_t sub_size_buf[8];
        if (fread(sub_guid, 1, 16, f) != 16 || fread(sub_size_buf, 1, 8, f) != 8) break;
        uint64_t sub_size = read_u64le(sub_size_buf);
        long data_start = ftell(f);
        /* Ensure sub_size covers the 24-byte header before subtracting,
         * avoiding underflow and safely advancing to the next object. */
        if (sub_size < 24) break;
        long next;
        if (!safe_chunk_advance(data_start, sub_size - 24, 0, &next)) break;

        if (memcmp(sub_guid, GUID_CONTENT_DESCRIPTION, 16) == 0) {
            asf_read_content_description(f, data_start, out);
        } else if (memcmp(sub_guid, GUID_EXT_CONTENT_DESCRIPTION, 16) == 0) {
            asf_read_ext_content_description(f, out);
        }

        pos = next;
    }

    fclose(f);
}

/* ---- WAV: RIFF LIST/INFO chunk (dr_wav parses this natively) ---- */

static bool read_wav_metadata(const char * path, track_metadata_t * out) {
    drwav wav;
    if (!drwav_init_file_with_metadata(&wav, path, drwav_metadata_type_list_all_info_strings, NULL)) return false;
    bool invalid_title_utf8 = false;

    for (drwav_uint32 i = 0; i < wav.metadataCount; i++) {
        drwav_metadata * meta = &wav.pMetadata[i];
        switch (meta->type) {
            case drwav_metadata_type_list_info_title:
                {
                    const uint8_t * raw = (const uint8_t *) meta->data.infoText.pString;
                    size_t len = meta->data.infoText.stringLength;
                    invalid_title_utf8 = false;
                    if (len >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF))) {
                        uint8_t tagged[2 * sizeof(out->title) + 3];
                        /* dr_wav exposes stringLength as chunkSize - 1 and
                         * allocates/reads the final byte too, even when a
                         * non-terminated UTF-16 value ends in a code unit. */
                        size_t copy_len = len + 1;
                        if (copy_len > sizeof(tagged) - 1) copy_len = sizeof(tagged) - 1;
                        copy_len &= ~(size_t) 1;
                        tagged[0] = 1;
                        memcpy(tagged + 1, raw, copy_len);
                        decode_id3v2_text_frame(tagged, (uint32_t) (copy_len + 1), out->title, sizeof(out->title));
                        size_t decoded_len = strlen(out->title);
                        if (!is_valid_utf8(out->title, decoded_len)) {
                            invalid_title_utf8 = true;
                            utf8_sanitize(out->title);
                        }
                    } else {
                        if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) { raw += 3; len -= 3; }
                        invalid_title_utf8 = !is_valid_utf8((const char *) raw, len);
                        copy_bounded(out->title, sizeof(out->title), (const char *) raw, len);
                    }
                }
                out->has_title = out->title[0] != '\0';
                break;
            case drwav_metadata_type_list_info_artist:
                SET_TAG_TEXT(out, artist, meta->data.infoText.pString, meta->data.infoText.stringLength);
                break;
            case drwav_metadata_type_list_info_album:
                SET_TAG_TEXT(out, album, meta->data.infoText.pString, meta->data.infoText.stringLength);
                break;
            case drwav_metadata_type_list_info_tracknumber:
                apply_sequence_text(out, false, meta->data.infoText.pString, meta->data.infoText.stringLength);
                break;
            default:
                break;
        }
    }

    drwav_uninit(&wav);
    return invalid_title_utf8;
}

void read_wav_all(const char * path, track_metadata_t * out, bool include_blobs) {
    bool invalid_info_title = read_wav_metadata(path, out);
    if (!out->has_title) {
        read_iff_id3(path, out, include_blobs, false);
    } else if (include_blobs || invalid_info_title || !out->has_release_year) {
        /* LIST/INFO carries only text: a cover or lyrics can live solely in
         * the "id3 " chunk. Its text stays authoritative unless INFO bytes
         * were invalid UTF-8 and an ID3 title can recover the display text. */
        track_metadata_t id3;
        memset(&id3, 0, sizeof(id3));
        read_iff_id3(path, &id3, include_blobs, false);
        if (invalid_info_title && id3.has_title) {
            copy_bounded(out->title, sizeof(out->title), id3.title, strnlen(id3.title, sizeof(id3.title)));
            out->has_title = true;
        }
        if (!out->has_release_year && id3.has_release_year) {
            out->release_year = id3.release_year;
            out->has_release_year = true;
        }
        if (!include_blobs) {
            free(id3.picture_data);
            free(id3.lyrics);
            return;
        }
        out->picture_data = id3.picture_data;
        out->picture_size = id3.picture_size;
        out->picture_too_large = id3.picture_too_large;
        out->lyrics = id3.lyrics;
    }
}
