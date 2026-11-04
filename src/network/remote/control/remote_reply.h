#ifndef REMOTE_REPLY_H
#define REMOTE_REPLY_H

/* JSON error replies shared by every remote-control source (remote_http.c). */

/* {"error":"<error>","code":"<code>"} */
void send_api_error(int cfd, const char * status, const char * error, const char * code);
/* {"error":"<reason>","code":"<reason>"} */
void send_reason_error(int cfd, const char * status, const char * reason);

#endif /* REMOTE_REPLY_H */
