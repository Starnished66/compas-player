#ifndef HTTP_URL_H
#define HTTP_URL_H

#include <stdbool.h>
#include <stddef.h>

/* Splits a non-NULL http(s):// URL into caller-owned host/port/path buffers.
 * Rejects unrecognized schemes and empty/oversized hosts or explicit ports.
 * Default ports (80/443) and paths retain the existing snprintf truncation
 * behavior. Fragments are excluded from the request path. */
bool http_conn_parse_url(const char * url, bool * out_https, char * host, size_t host_size,
                         char * port, size_t port_size, char * path, size_t path_size);

#endif
