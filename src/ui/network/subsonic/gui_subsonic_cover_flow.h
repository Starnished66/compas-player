#pragma once
#include <stdbool.h>
#include "../../../library/remote/subsonic/subsonic_cache.h"

/* Cover Flow over the Subsonic albums. UI thread. The albums are copied by the
 * carousel, so the caller's array may be freed or replaced right after. A tap on
 * the centre cover calls open_album(id, title). The covers come from the card,
 * and from the server only for the centre cover and only if Wi-Fi was connected
 * when this was called. */
typedef void (*gui_subsonic_cover_flow_open_album_fn)(const char * id, const char * title);

/* UI thread, every tick: the current Wi-Fi state for the covers still to fetch, and
 * on a lost network or once the carousel is gone, cuts the downloads running. */
void gui_subsonic_cover_flow_poll(bool wifi_connected);

void gui_subsonic_cover_flow_open(const subsonic_server_t * server, const subsonic_album_t * albums, int count,
                                  gui_subsonic_cover_flow_open_album_fn open_album);
