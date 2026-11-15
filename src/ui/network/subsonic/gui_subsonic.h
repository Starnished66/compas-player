#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <lvgl/lvgl.h>
#include "../../../library/remote/subsonic/subsonic_cache.h"
#include "../../../network/services/subsonic_client.h"

typedef struct {
    char song_id[64];
    char artist_id[64], album_id[64];
    char url[1536]; /* the exact playlist[i] this entry describes */
    char title[128];
    char artist[128];
    char album[128];
    char cover_art[256]; /* coverArt id; the cover is fetched by id, never by URL */
    char server_key[SUBSONIC_CACHE_KEY_SIZE]; /* which server the id belongs to */
    char suffix[16];
    int track;
    int disc;
    int duration_seconds;
    unsigned int sample_rate;
    unsigned int bit_depth;
    unsigned int channels;
    unsigned int bitrate_kbps;
} subsonic_stream_song_meta_t;


subsonic_server_t subsonic_server_from_settings(void);
bool gui_subsonic_session_active(void);
void gui_subsonic_session_set_active(bool active);
void gui_subsonic_init(void);
/* Deletes every screen this module owns so gui_reload.c's in-process UI
 * reload can call gui_subsonic_init() again from a clean slate. */
void gui_subsonic_teardown(void);
/* UI-only lookup and source-aware navigation for player and song menus. */
const subsonic_stream_song_meta_t * gui_subsonic_song_for_path(const char * path);
bool gui_subsonic_open_song_group(const char * path, bool album);

extern subsonic_stream_song_meta_t * subsonic_stream_meta;
extern int subsonic_stream_meta_count;
extern void subsonic_tile_cb(lv_event_t * e);

void poll_subsonic_download(void);
void poll_subsonic_library_download(void);
void poll_subsonic_connect(void);
void poll_subsonic_browse(void);
void poll_subsonic_refresh(void);

bool gui_subsonic_has_background_work(void);
void gui_subsonic_handle_wifi_disabled(void);
void gui_subsonic_cancel_background_work(void);

/* Moves the now-playing marker on Subsonic song lists; call when the track changes. */
void gui_subsonic_refresh_now_playing(void);

/* Prepare one directly streamable song for the shared queue context menu. */
void gui_subsonic_open_song_context(int index);

/* Open an artist's albums or an album's songs from global search results. */
void gui_subsonic_global_open_artist(const char *id, const char *name);
void gui_subsonic_global_open_album(const char *id, const char *name);
/* The song value must own its strings; it remains valid only for this call. */
void gui_subsonic_global_song(const subsonic_song_t *song, bool long_press);
