#ifndef SDTOOL_PARSE_POOL_H
#define SDTOOL_PARSE_POOL_H

#include "metadata.h"
#include <stdbool.h>
#include <stdio.h>

/* Tag parsing in a separate process, as the player does (metadata_isolated.c):
 * a file whose parser crashes or hangs is reported failed and costs only the
 * worker, never a placeholder row. The same code runs on every system:
 * compas-sdtool starts itself as `parse-worker`, which reads file names from
 * standard input and writes one track_metadata_t per file to standard output
 * (the child's inputs and outputs are binary and in the same build, so the
 * struct is the protocol). */

/* Parses paths[0..count) into out[], setting ok[i] = false for a file that
 * could not be parsed. Returns false only if no worker could be started. */
bool sdtool_parse_files(const char *const *paths, size_t count, track_metadata_t *out, bool *ok);

/* The `parse-worker` entry point. */
int sdtool_parse_worker(void);

/* process.c */
FILE *sdtool_worker_open(const char *input_file);
void sdtool_worker_close(FILE *worker);

#endif
