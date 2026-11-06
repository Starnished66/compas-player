#ifndef SUBSONIC_CLIENT_H
#define SUBSONIC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include "../http/client/http_client.h"

/* Connection details for one configured server. verify_tls controls HTTPS
 * certificate verification. */
typedef struct {
    char base_url[256];  /* e.g. "https://music.example.com:4040", no trailing slash, no "/rest" */
    char username[128];
    char password[128];
    bool verify_tls;
} subsonic_server_t;

/* Safe diagnostic for the most recent API request on the calling thread.
 * Copy before leaving a worker; never includes URLs or credentials. */
const char * subsonic_last_error(void);

/* The server's own error code from the same request, 0 when it sent none
 * (network failure, bad JSON). 70 means the item no longer exists. */
#define SUBSONIC_ERROR_NOT_FOUND 70
int subsonic_last_error_code(void);

/* Artist, album and playlist lists keep their text in the same allocation as
 * the array: free() the array and nothing else. The text is only valid while
 * that array lives, so never copy rows to a longer-lived owner. Fields the
 * server omits are "". */
#define SUBSONIC_ARTIST_TEXT_FIELDS 3
#define SUBSONIC_ALBUM_TEXT_FIELDS 6
#define SUBSONIC_GENRE_TEXT_FIELDS 1
#define SUBSONIC_PLAYLIST_TEXT_FIELDS 4 /* the text members come first, the ints after them */

typedef struct {
    const char * id;
    const char * name;
    const char * cover_art; /* getCoverArt.view id parameter */
} subsonic_artist_t;

typedef struct {
    const char * id;
    const char * name;
    const char * artist;
    const char * artist_id;
    const char * cover_art; /* getCoverArt.view id parameter */
    const char * genre;
    int year;
    int song_count;
    int duration_seconds;
} subsonic_album_t;

typedef struct {
    char id[64];
    char title[128];
    char artist[128];
    char album[128];
    char album_artist[128]; /* album grouping artist; falls back to track artist */
    char suffix[16];    /* File extension without leading dot (e.g. "mp3", "flac") */
    char cover_art[64]; /* getCoverArt.view id parameter, empty if omitted */
    int track;
    int disc;
    int duration_seconds;
    unsigned int sample_rate;
    unsigned int bit_depth;
    unsigned int channels;
    unsigned int bitrate_kbps;
} subsonic_song_t;

typedef struct {
    const char * name;
    int album_count;
    int song_count;
} subsonic_genre_t;

typedef struct {
    const char * id;
    const char * name;
    const char * changed; /* last-change timestamp as the server wrote it */
    const char * cover_art; /* getCoverArt.view id parameter */
    int song_count;
} subsonic_playlist_t;

/* Cheap "has the library changed" markers. Each is 0 / "" when the server does
 * not report it. */
typedef struct {
    long long last_modified; /* getIndexes lastModified, ms since the epoch */
    char last_scan[48];      /* getScanStatus lastScan */
} subsonic_library_stamp_t;

/* Fixed-width rows whose text lives in one trailing block (see above). The
 * first text_fields members of a row must be const char *; subsonic_pack_add()
 * copies their text, so a caller may point them at transient buffers. */
typedef struct subsonic_pack subsonic_pack_t;
subsonic_pack_t * subsonic_pack_new(size_t row_size, int text_fields);
bool subsonic_pack_add(subsonic_pack_t * pack, const void * row);
/* Frees the builder. *out is NULL for zero rows; false only when memory ran out. */
bool subsonic_pack_finish(subsonic_pack_t * pack, void ** out, int * out_count);
void subsonic_pack_discard(subsonic_pack_t * pack);

/* Trims the URL the way every request does. */
void subsonic_normalize_base_url(const char * input, char * out, size_t out_size);

/* Checks server reachability and credentials via ping.view. */
bool subsonic_ping(const subsonic_server_t * server, http_cancel_token_t * cancel);

/* Fetches all artists via getArtists.view. Caller owns *out_artists. */
bool subsonic_get_artists(const subsonic_server_t * server, subsonic_artist_t ** out_artists, int * out_count,
                           http_cancel_token_t * cancel);

/* Fetches albums for an artist via getArtist.view. Caller owns *out_albums. */
bool subsonic_get_artist_albums(const subsonic_server_t * server, const char * artist_id,
                                 subsonic_album_t ** out_albums, int * out_count, http_cancel_token_t * cancel);

/* Fetches songs for an album via getAlbum.view. Caller owns *out_songs. */
bool subsonic_get_album_songs(const subsonic_server_t * server, const char * album_id,
                               subsonic_song_t ** out_songs, int * out_count, http_cancel_token_t * cancel);

/* Fetches albums alphabetically via getAlbumList2.view, 500 per request, up to
 * 20000 in total. An empty library succeeds with zero items. Caller owns *out_albums. */
bool subsonic_get_all_albums(const subsonic_server_t * server, subsonic_album_t ** out_albums, int * out_count,
                              http_cancel_token_t * cancel);

/* Fetches one page (up to 500) of getAlbumList2.view for a list type such as
 * "newest", "recent", "frequent", "highest", "random" or "alphabeticalByName".
 * genre is only used with "byGenre" (pass NULL otherwise); false if it is too
 * long to send. An empty result succeeds with zero items. Caller owns *out_albums. */
bool subsonic_get_album_list(const subsonic_server_t * server, const char * type, const char * genre,
                             subsonic_album_t ** out_albums, int * out_count, http_cancel_token_t * cancel);

/* Fetches the genres via getGenres.view. Caller owns *out_genres. */
bool subsonic_get_genres(const subsonic_server_t * server, subsonic_genre_t ** out_genres, int * out_count,
                         http_cancel_token_t * cancel);

/* Fetches random songs via getRandomSongs.view; size <= 0 means 100, at most 500.
 * Caller owns *out_songs. */
bool subsonic_get_random_songs(const subsonic_server_t * server, int size, subsonic_song_t ** out_songs,
                               int * out_count, http_cancel_token_t * cancel);

/* The starred items of getStarred2.view. Free each list with free(); a list
 * with no items is NULL with count 0. */
typedef struct {
    subsonic_artist_t * artists;
    int artist_count;
    subsonic_album_t * albums;
    int album_count;
    subsonic_song_t * songs;
    int song_count;
} subsonic_starred_t;

bool subsonic_get_starred(const subsonic_server_t * server, subsonic_starred_t * out, http_cancel_token_t * cancel);

/* Reads the library change markers (getIndexes and getScanStatus). False only
 * when neither request got an answer. */
bool subsonic_get_library_stamp(const subsonic_server_t * server, subsonic_library_stamp_t * out,
                                 http_cancel_token_t * cancel);

/* True when both stamps carry a comparable marker and none differs. */
bool subsonic_stamp_unchanged(const subsonic_library_stamp_t * known, const subsonic_library_stamp_t * now);

/* A playlist's stamp: song count (stored plus one, so 0 means "no stamp") and
 * changed time. Unlike the library stamp, a count of zero is a real value. */
void subsonic_playlist_stamp(const subsonic_playlist_t * playlist, subsonic_library_stamp_t * out);

/* True when both playlist stamps exist and are equal. */
bool subsonic_playlist_stamp_unchanged(const subsonic_library_stamp_t * known, const subsonic_library_stamp_t * now);

/* Fetches playlists via getPlaylists.view. Caller owns *out_playlists. */
bool subsonic_get_playlists(const subsonic_server_t * server, subsonic_playlist_t ** out_playlists, int * out_count,
                             http_cancel_token_t * cancel);

/* Fetches songs for a playlist via getPlaylist.view. Caller owns *out_songs. */
bool subsonic_get_playlist_songs(const subsonic_server_t * server, const char * playlist_id,
                                  subsonic_song_t ** out_songs, int * out_count, http_cancel_token_t * cancel);

/* Builds the stream.view URL including auth parameters for song_id. */
void subsonic_build_stream_url(const subsonic_server_t * server, const char * song_id, char * out_url, size_t out_url_size);

/* Builds a stream URL for a new playback queue. max_bitrate_kbps <= 0 keeps
 * the server's original stream representation; positive values request MP3
 * transcoding at that maximum bitrate. Downloads should use the original
 * builder above, independent of the playback preference. */
void subsonic_build_stream_url_quality(const subsonic_server_t * server, const char * song_id,
                                       int max_bitrate_kbps, char * out_url, size_t out_url_size);

/* Builds the getCoverArt.view URL including auth parameters for cover_art_id.
 * size_px > 0 asks the server for a cover scaled to that size; 0 asks for the original. */
void subsonic_build_cover_art_url(const subsonic_server_t * server, const char * cover_art_id, int size_px,
                                  char * out_url, size_t out_url_size);

#endif /* SUBSONIC_CLIENT_H */
