#ifndef OPUSFILE_ALLOC_H
#define OPUSFILE_ALLOC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <opusfile.h>

/* libogg/libopusfile allocations share a global live-memory cap, including
 * concurrent opens/decoders and the overlap during realloc. Metadata opens
 * avoid the PCM seek index; playback opens require seekable input so all
 * chained-link headers are available before playback starts. */
OggOpusFile * compas_opusfile_open(const char * path, bool metadata_only);

/* Allocators used when compiling vendored libogg/libopusfile sources with
 * -Dmalloc/-Dcalloc/-Drealloc/-Dfree mapped to these helpers. */
void * compas_opusfile_malloc(size_t size);
void * compas_opusfile_calloc(size_t count, size_t size);
void * compas_opusfile_realloc(void * ptr, size_t size);
void compas_opusfile_free(void * ptr);

#endif /* OPUSFILE_ALLOC_H */
