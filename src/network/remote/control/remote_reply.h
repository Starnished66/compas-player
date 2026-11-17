#ifndef REMOTE_REPLY_H
#define REMOTE_REPLY_H

#include <stddef.h>

/* JSON error replies shared by every remote-control source (remote_http.c). */

/* {"error":"<error>","code":"<code>"} */
void send_api_error(int cfd, const char * status, const char * error, const char * code);
/* {"error":"<reason>","code":"<reason>"} */
void send_reason_error(int cfd, const char * status, const char * reason);

/* Most body bytes read and discarded after an error reply. */
#define REPLY_DRAIN_MAX_BYTES (8u * 1024u * 1024u)

/* Error reply for a request whose body was not fully read. Closing a TCP
 * socket with unread data can reset the connection and discard the reply, so
 * send it, half-close, and read what is left of the body (at most `remaining`
 * bytes, bounded in bytes and time) before the caller closes. */
void send_api_error_drain(int cfd, const char * status, const char * error, const char * code,
                          size_t remaining);

#endif /* REMOTE_REPLY_H */
