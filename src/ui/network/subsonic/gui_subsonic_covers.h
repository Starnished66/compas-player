#pragma once
#include <lvgl/lvgl.h>
#include "../../../library/remote/subsonic/subsonic_cache.h"

/* Covers for the Subsonic Albums, Artists and Playlists rows. One worker
 * thread reads each thumbnail from the SD card, or downloads it when Wi-Fi is
 * connected; it never touches LVGL. The UI thread keeps the decoded ones in a
 * small LRU and shows the placeholder until a cover lands. All functions are
 * UI thread only. */

/* Installs the cover row decorator on a compact list. Its items carry the
 * cover id as artwork_name (borrowed) and gui_subsonic_covers_key(id) as artwork_key. */
void gui_subsonic_covers_bind(lv_obj_t * list);

/* The server whose covers are shown. A different server drops what is queued. */
void gui_subsonic_covers_set_server(const subsonic_server_t * server);

/* RAM key of a cover id on the current server; 0 for no cover or no server. */
uint64_t gui_subsonic_covers_key(const char * cover_id);

/* Drops queued requests and earlier failures: call when the lists' borrowed
 * ids change or the network does. Keeps decoded covers. */
void gui_subsonic_covers_reset(void);

/* Frees every decoded cover and retires the worker without waiting for it.
 * Rows show placeholders until gui_subsonic_covers_wake() or a new list is bound. */
void gui_subsonic_covers_shutdown(void);

/* Screen back on after a shutdown: the visible rows ask for their covers again. */
void gui_subsonic_covers_wake(void);

/* Main loop: shows covers the worker finished. */
void poll_subsonic_covers(void);

/* Copies a cached thumbnail, or queues it without blocking. */
bool gui_subsonic_covers_copy(const char * id, uint16_t * pixels, bool * no_art);
