#ifndef REMOTE_CONTROL_INTERNAL_H
#define REMOTE_CONTROL_INTERNAL_H

/* Internal interface shared by the remote_*.c files that make up the Remote
 * Control HTTP server. The public API is remote_control.h. Only
 * remote_control_state.c touches the status mutex and the request/queue
 * snapshot; everything else reaches that state through the remote_state_*
 * accessors below. */

#include "remote_control.h"
#include "remote_reply.h"
#include "../../../library/metadata/db/metadata_db.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char * data;
    size_t capacity;
    size_t length;
    bool truncated;
} json_builder_t;

#define PLAYLIST_NAME_MAX_BYTES 127

/* FNV-1a. Phones cache cover keys and playlist revisions derived from these
 * hashes, so the byte sequences fed in must never change. */
#define FNV_OFFSET_BASIS UINT64_C(14695981039346656037)

static inline uint64_t fnv1a_byte(uint64_t hash, uint8_t byte) {
    return (hash ^ byte) * UINT64_C(1099511628211);
}

#define REQUEST_LINE_MAX 4096

/* JSON building, URL and query parsing, HTTP replies (remote_http.c) */
void json_escape_append(char * out, size_t out_size, const char * in);
void json_builder_init(json_builder_t * b, char * data, size_t capacity);
bool json_builder_appendf(json_builder_t * b, const char * format, ...);
bool url_decode(const char * in, char * out, size_t out_size);
bool url_decode_checked(const char * in, char * out, size_t out_size);
bool query_param_present(const char * path, const char * key);
bool query_param_str(const char * path, const char * key, char * out, size_t out_size);
int query_loose(const char * path, const char * key, char * out, size_t out_size);
void send_all(int fd, const char * data, size_t len);
void send_header(int fd, const char * status_line, const char * content_type, size_t len);
void send_response_binary(int fd, const char * status_line, const char * content_type, const uint8_t * data,
                          size_t len);
void send_response(int fd, const char * status_line, const char * content_type, const char * body);
char * json_alloc_or_500(int cfd, size_t capacity);
void send_json_free(int cfd, char * json);
bool query_param_int(const char * path, const char * key, int * out_value);
bool query_param_int_saturating(const char * path, const char * key, int * out_value);
bool query_param_int64(const char * path, const char * key, int64_t * out_value);
const char * sniff_image_content_type(const uint8_t * data, uint32_t size);
void catalog_send_error(int cfd, const char * status, const char * code);
bool catalog_query_number(const char * path, const char * key, uint64_t default_value, uint64_t max_value,
                          uint64_t * out_value);
bool query_paging(const char * path, uint64_t max_limit, int * out_offset, int * out_limit);
bool catalog_query_revision_named(const char * path, const char * key, bool required,
                                  char out[METADATA_DB_CATALOG_REVISION_SIZE]);
uint64_t catalog_hash_text(uint64_t hash, const char * text);
bool json_append_row(json_builder_t * json, const char * row, size_t reserve);
int query_decoded(const char * path, const char * key, char * out, size_t out_size);
bool query_required_number(const char * path, const char * key, uint64_t max_value, uint64_t * out);

/* Status and request state (remote_control_state.c) */
bool remote_state_now_playing_path(char * out, size_t size);
void remote_state_post_folder_play(const char * abs_path);
void remote_state_favorite_written(const char * song_path, bool is_favorite);
void handle_queue_edit_request(int cfd, const char * path, bool move);
void handle_library_sorting_route(int fd, const char *method, const char *path);
void handle_scoped_albums_route(int fd, const char *method, const char *path);
void handle_collection_play_route(int fd, const char *method, const char *path);
bool handle_import_lifecycle_route(int cfd, const char * method, const char * path_only);
void handle_status_get(int cfd);
void handle_import_get(int cfd);
void handle_queue_get(int cfd, const char * path);
void handle_legacy_playback_post(int cfd, const char * path_only, const char * path);

/* Catalog routes (remote_catalog.c) */
void handle_catalog_page_request(int cfd, const char * path, bool covers);
void handle_catalog_art_request(int cfd, const char * path);

/* Library, art, folders, recent and favorites routes (remote_library.c) */
void handle_art_request(int cfd, const char * path);
void handle_folder_roots_request(int cfd);
void handle_folder_list_request(int cfd, const char * path);
void handle_folder_play_request(int cfd, const char * path);
void handle_recent_request(int cfd, const char * path);
void handle_favorite_state_request(int cfd, const char * path);
void handle_favorite_edit_request(int cfd, const char * path, bool is_favorite);
bool handle_group_list_request(int cfd, const char * path_only);
void handle_library_get(int cfd, const char * path);
void handle_albums_get(int cfd, const char * path);

/* Playlist routes (remote_playlists.c) */
bool playlist_name_is_safe(const char * name);
void handle_playlist_songs_request(int cfd, const char * path);
void handle_playlist_create_request(int cfd, const char * path);
void handle_playlist_rename_request(int cfd, const char * path);
void handle_playlist_delete_request(int cfd, const char * path);
void handle_playlist_entry_request(int cfd, const char * path, bool move);
void handle_playlist_import_request(int cfd, const char * path, long long content_length);
void handle_playlists_get(int cfd);
void handle_legacy_playlist_post(int cfd, const char * path_only, const char * path);

#endif /* REMOTE_CONTROL_INTERNAL_H */
