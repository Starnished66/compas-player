#include "http_url.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

bool http_conn_parse_url(const char * url, bool * out_https, char * host, size_t host_size,
                          char * port, size_t port_size, char * path, size_t path_size) {
    const char * p;
    if (strncasecmp(url, "https://", 8) == 0) { *out_https = true; p = url + 8; }
    else if (strncasecmp(url, "http://", 7) == 0) { *out_https = false; p = url + 7; }
    else return false;

    const char * host_start = p;
    const char * host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t host_len = (size_t) (host_end - host_start);
    if (host_len == 0 || host_len >= host_size) return false;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    p = host_end;
    if (*p == ':') {
        p++;
        const char * port_start = p;
        while (*p && *p != '/') p++;
        size_t port_len = (size_t) (p - port_start);
        if (port_len == 0 || port_len >= port_size) return false;
        memcpy(port, port_start, port_len);
        port[port_len] = '\0';
    } else {
        snprintf(port, port_size, "%s", *out_https ? "443" : "80");
    }

    /* A "#fragment" is purely client-side (RFC 3986) and must never be sent
     * to the server -- strip it before it becomes part of the request path.
     * (audio.c's decoder_open() deliberately relies on this: it reads a
     * "#.<ext>" suffix off a stream URL as a local-only format hint before
     * ever handing the URL to this function, so the fragment must not leak
     * into the actual HTTP request line.) */
    const char * frag = strchr(p, '#');
    size_t p_len = frag ? (size_t) (frag - p) : strlen(p);

    if (p_len == 0) {
        snprintf(path, path_size, "/");
    } else {
        snprintf(path, path_size, "%.*s", (int) p_len, p);
    }
    return true;
}
