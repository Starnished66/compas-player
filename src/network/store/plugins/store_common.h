#ifndef STORE_COMMON_H
#define STORE_COMMON_H

#include "cJSON.h"
#include "../../http/client/http_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Validation, hashing, download and worker helpers shared by the GitHub
 * catalog installers: the Plugin Store (plugin_store.c) and the emulator
 * store (emulator_store.c). Both fetch a release index.json, check every
 * field before trusting it, verify each downloaded file by size and
 * SHA-256, and run network/disk jobs on lightweight detached threads. */

#define STORE_TIMEOUT_CONNECT 10000
#define STORE_TIMEOUT_READ 20000

/* Copies text into out if out and size are non-zero, formatted with snprintf("%s"). */
void store_error_text(char * out, size_t size, const char * text);

/* Spawns fn(arg) on a detached thread with a 256 KiB stack. Returns true if created. */
bool store_start_thread(void * (*fn)(void *), void * arg);

bool store_ascii_digit(char c);
bool store_ascii_lower(char c);
bool store_ascii_alnum(char c);
/* Letters, digits, '_' and '-' (plus '.' when dot), 1..max bytes. */
bool store_token_valid(const char * s, size_t max, bool dot);
/* Up to four dot-separated digit groups, under 32 bytes. */
bool store_version_valid(const char * s);
/* 64 lowercase hex digits. */
bool store_sha_valid(const char * s);
/* No control characters, at most max bytes. */
bool store_safe_text(const char * s, size_t max, bool allow_empty);
/* A relative destination below an install root: at most 255 bytes of
 * letters, digits and . _ - space, '/'-separated, no empty, "." or ".."
 * segment, not absolute. */
bool store_dest_path_valid(const char * dest);
/* Two destinations that cannot both exist: the same path (case-insensitive,
 * the card is FAT), or one is a folder the other lives in. */
bool store_dests_collide(const char * a, const char * b);
/* Numeric dotted compare: negative, zero or positive like strcmp. */
int store_version_compare(const char * a, const char * b);

const cJSON * store_json_field(const cJSON * o, const char * name);
const char * store_json_string(const cJSON * o, const char * name);
bool store_json_uint(const cJSON * o, const char * name, uint32_t min, uint32_t max, uint32_t * out);

/* Size and SHA-256 (lowercase hex) of a file. */
bool store_hash_file(const char * path, uint64_t * size, char hex[65]);

/* GET url (up to 5 redirects, verified TLS) into dest, then check that the
 * file has exactly the given size and SHA-256. http_get_to_file_redirects
 * removes dest when the transfer fails. */
bool store_download_verified(const char * url, const char * dest, uint32_t size, const char * sha256,
                             size_t max_bytes, http_progress_cb_t progress, void * context);

/* Fetches a catalog index into *response (caller frees with
 * http_response_free). Returns false with *error set to a translated reason;
 * generic_error is used when the failure is not a connection problem. */
bool store_fetch_index(const char * url, size_t limit, const char * generic_error,
                       http_response_t * response, const char ** error);

#endif
