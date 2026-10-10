#ifndef HTTP_STREAM_FILE_H
#define HTTP_STREAM_FILE_H
#include <stdio.h>
#include "http_stream.h"
/* Seekable stdio view for existing container parsers. The FILE owns its
 * adapter, but borrows stream: fclose FILE before http_stream_close. */
FILE * http_stream_open_file(http_stream_t * stream);

/* Like http_stream_open_file(), and also serves [span_offset, span_offset +
 * span_length) from span. Takes ownership of span (frees it on failure and
 * on fclose) so the demuxer can reread that range without seeking the
 * stream backwards. span may be NULL. A backward seek past the HTTP ring's
 * history opens a new connection, and the pump keeps that ring full. */
FILE * http_stream_open_span_file(http_stream_t * stream, uint8_t * span,
                                  uint64_t span_offset, uint64_t span_length);
#endif
