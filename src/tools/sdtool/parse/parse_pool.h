#ifndef SDTOOL_PARSE_POOL_H
#define SDTOOL_PARSE_POOL_H

#include "../../../library/metadata/parsing/metadata.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Tag parsing in a separate process, as the player does (metadata_isolated.c):
 * a file whose parser crashes or hangs is reported failed and costs only the
 * worker, never a placeholder row. The same code runs on every system:
 * compas-sdtool starts itself as `parse-worker`.
 *
 * Protocol (both sides are this build, so structs are the format): standard
 * input is a sequence of records, a uint32_t byte length followed by that many
 * bytes of path, so any file name is carried exactly. Standard output is, for
 * each file in order, a uint32_t file index followed by a track_metadata_t. The
 * parent checks the index; a record that is not the file expected counts that
 * file failed. */

typedef struct {
    uint32_t index;
    track_metadata_t meta;
} sdtool_parse_record_t;

typedef enum {
    SDTOOL_PARSE_OK,
    SDTOOL_PARSE_WORKER_FAILED, /* the worker cannot be started or never answers */
} sdtool_parse_result_t;

/* Parses paths[0..count) into out[], setting ok[i] = false for a file that
 * could not be parsed. *broken counts those whose worker left without a
 * word (exit 0, 126 or 127) rather than crashing or timing out: a hint that
 * the worker, not the file, is at fault. */
sdtool_parse_result_t sdtool_parse_files(const char *const *paths, size_t count,
                                         track_metadata_t *out, bool *ok, size_t *broken);

/* The `parse-worker` entry point; `artwork` is the `parse-worker art` mode. */
int sdtool_parse_worker(bool artwork);

/* Embedded cover art of one file, read in a worker of its own with the
 * player's size limit. The worker answers with the picture's size and bytes.
 * FAILED means the worker crashed, hung or could not run: no art for that file
 * (the caller counts it), and nothing of the parser touches this process. */
typedef enum { SDTOOL_ART_FOUND, SDTOOL_ART_NONE, SDTOOL_ART_FAILED } sdtool_art_result_t;
sdtool_art_result_t sdtool_parse_artwork(const char *path, uint8_t **data, uint32_t *size);

/* process.c: a child process of this program with a file on its standard input
 * and a pipe on its standard output. No shell is involved. */
typedef struct sdtool_worker sdtool_worker_t;
sdtool_worker_t *sdtool_worker_start(const char *input_file, bool artwork);
FILE *sdtool_worker_output(sdtool_worker_t *worker);
/* Closes the pipe, waits for the child and frees it. Returns its exit status,
 * or -1 if it did not exit normally. */
int sdtool_worker_finish(sdtool_worker_t *worker);

#endif
