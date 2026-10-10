#ifndef HTTP_STREAM_FILE_H
#define HTTP_STREAM_FILE_H
#include <stdint.h>
#include <stdio.h>
#include "http_stream.h"
/* Seekable stdio view for existing container parsers. The FILE owns its
 * adapter, but borrows stream: fclose FILE before http_stream_close. */
FILE * http_stream_open_file(http_stream_t * stream);

/* A file range served from memory; spans are sorted and do not overlap. */
typedef struct { uint64_t offset; uint64_t length; } http_span_t;

/* Like http_stream_open_file(), and also serves each span from bytes (the
 * spans' copies concatenated in order), so a demuxer can reread container
 * metadata without seeking the stream backwards. Takes ownership of bytes
 * and spans (frees them on failure and on fclose); both may be NULL with
 * span_count 0. Reads outside the spans go to the stream; a backward seek
 * past the HTTP ring's history opens a new connection. */
FILE * http_stream_open_spans_file(http_stream_t * stream, uint8_t * bytes,
                                   http_span_t * spans, size_t span_count);
#endif
