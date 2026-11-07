#ifndef METADATA_PARSER_INTERNAL_H
#define METADATA_PARSER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "metadata.h"

#define METADATA_BLOB_MAX_BYTES (4U * 1024U * 1024U)
#define EMBEDDED_COVER_MAX_BYTES METADATA_BLOB_MAX_BYTES

/* One-way mode used only by the disposable artwork helper process. */
void metadata_parser_enable_artwork_only(void);

/* ---- Shared by the metadata_*.c readers (not for other modules) ---- */

extern bool artwork_only;
extern _Thread_local bool lyrics_only;

/* Mode predicates shared by every reader: pictures are skipped for the lyrics
 * worker, lyrics for the artwork helper. */
static inline bool wants_picture(bool include_blobs) { return include_blobs && !lyrics_only; }
static inline bool wants_lyrics(bool include_blobs) { return include_blobs && !artwork_only; }

#define SET_TAG_TEXT(out, f, v, n) set_tag_text((out)->f, sizeof((out)->f), &(out)->has_##f, (v), (n))

/* off_t appears in shared signatures: every including unit must be built with
 * 64-bit file offsets (#define _FILE_OFFSET_BITS 64 before any include). */
_Static_assert(sizeof(off_t) == 8, "define _FILE_OFFSET_BITS 64 before including system headers");

/* Field helpers (metadata.c). */
bool safe_file_advance(off_t chunk_start, uint64_t size, off_t pad, off_t * out_next);
bool store_picture_bytes(track_metadata_t * out, const uint8_t * src, uint32_t n);
void copy_bounded(char * dst, size_t dst_size, const char * src, size_t src_len);
void set_tag_text(char * dst, size_t dst_size, bool * has, const char * v, size_t n);
void set_sequence(track_metadata_t * out, bool disc, int number);
uint32_t read_be32(const uint8_t * b, bool synchsafe);
uint64_t read_u64be(const uint8_t * b);
void apply_sequence_text(track_metadata_t * out, bool disc, const char * value, size_t value_len);
void apply_original_year(track_metadata_t * out, const char * value, size_t value_len);
void apply_release_year(track_metadata_t * out, const char * value, size_t value_len, bool canonical_date);
void apply_replaygain_field(track_metadata_t * out, const char * key, size_t key_len, const char * value, size_t value_len);
void apply_vorbis_comment_field(track_metadata_t * out, const char * comment, size_t comment_len, bool include_blobs);
bool apply_vorbis_picture_comment(track_metadata_t * out, const char * comment, size_t comment_len, bool include_blobs);

/* ID3v2 and MP3/AAC readers (metadata_id3.c). */
void decode_id3v2_text_frame(const uint8_t * data, uint32_t size, char * out, size_t out_size);
bool read_id3v2(FILE * f, track_metadata_t * out, bool include_blobs);
void read_mp3_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_aac_metadata(const char * path, track_metadata_t * out, bool include_blobs);

/* FLAC, M4A, Ogg/Opus, WavPack and CAF readers (metadata_xiph_mp4.c). */
void read_flac_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_m4a_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_wavpack_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_caf_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_opus_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_ogg_vorbis_metadata(const char * path, track_metadata_t * out, bool include_blobs);

/* RIFF/AIFF/DSF/DFF/APE/WMA readers (metadata_iff.c). */
void read_aiff_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_dsf_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_ape_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_dff_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_wma_metadata(const char * path, track_metadata_t * out, bool include_blobs);
void read_wav_all(const char * path, track_metadata_t * out, bool include_blobs);

#endif /* METADATA_PARSER_INTERNAL_H */
