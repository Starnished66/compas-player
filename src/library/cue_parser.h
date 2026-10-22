#ifndef CUE_PARSER_H
#define CUE_PARSER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int number;
    char title[128];
    char performer[128];
    char audio_path[600];
    double start_seconds;
    double end_seconds; /* -1 when no explicit end is available */
    bool has_index_01;
} cue_track_t;

typedef struct {
    char audio_path[600]; /* first track's resolved source, retained for
                             compatibility */
    char title[128];
    char performer[128];
    cue_track_t *tracks;
    int track_count;
} cue_sheet_t;

bool cue_parse_file(const char *cue_path, cue_sheet_t *out);
bool cue_parse_embedded(const char *audio_path, cue_sheet_t *out);
void cue_sheet_free(cue_sheet_t *out);

/* Virtual track references are serialized as source#compas-cue=N (N is
 * 1-based). */
bool cue_reference_format(const char *source_path, unsigned ordinal_1based, char *out,
                          size_t out_size);
bool cue_reference_is(const char *reference);
bool cue_reference_parse(const char *reference, char *source_path_out, size_t source_out_size,
                         unsigned *ordinal_1based_out);
bool cue_reference_resolve(const char *reference, cue_sheet_t *out, unsigned *ordinal_1based_out);
bool cue_reference_audio_path(const char *reference, char *audio_path_out, size_t out_size);

#endif
