#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "icy_metadata.h"

typedef struct http_stream http_stream_t;

/* Opens an HTTP(S) stream. A byte-range probe is used to identify finite,
 * seekable responses; live and ordinary non-range responses remain readable. */
/* Generation 0 is untracked; other generations support playback cancellation. */
http_stream_t * http_stream_open(const char * url, bool verify_tls,
                                 bool request_icy, uint64_t generation);
void http_stream_cancel_generation(uint64_t generation);
size_t http_stream_read(http_stream_t * s, void * buf, size_t n);
const char * http_stream_content_type(http_stream_t * s);
/* Nonblocking snapshot; false if unavailable or a worker currently owns it. */
bool http_stream_metadata_for_url(const char * url, icy_metadata_t * out);

/* Finite HTTP byte-range streams support stdio SEEK_SET/CUR/END. Length and
 * position are byte offsets; length returns -1 when the entity is not known
 * to be finite. A failed seek leaves the current stream position unchanged. */
bool http_stream_can_seek(http_stream_t * s);
int64_t http_stream_tell(http_stream_t * s);
int64_t http_stream_length(http_stream_t * s);
bool http_stream_seek(http_stream_t * s, int64_t offset, int origin);

/* The caller owns and serializes read/seek/close operations. Close stops the
 * internal pump; it must not race a caller that is reading or seeking. */
void http_stream_close(http_stream_t * s);

#ifdef HTTP_STREAM_TESTING
/* Deterministically fail the next stream-pump pthread_create in host tests. */
void http_stream_test_fail_next_pump_create(void);
#endif

#endif
