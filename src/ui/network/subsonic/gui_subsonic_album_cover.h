#pragma once
#include <stdbool.h>
#include <lvgl/lvgl.h>
#include "../../../library/remote/subsonic/subsonic_cache.h"

/* The large cover of the Subsonic album page on screen: one worker reads it
 * from the SD card (or downloads it when Wi-Fi is connected), decodes it at the
 * page's cover size and hands over one RGB565 buffer. The worker never touches
 * LVGL and the UI never waits for it. All functions are UI thread only. */

#define GUI_SUBSONIC_COVER_ID_MAX 256 /* the longest id a list holds (255) plus the NUL */

/* Asks for the cover of `cover_id` at px x px. Asking again for the cover
 * already ready or on its way costs nothing; another id replaces it. */
void gui_subsonic_album_cover_request(const subsonic_server_t * server, const char * cover_id, int px);

/* Main loop: reaps a finished job and retries a cover that failed offline once
 * Wi-Fi is back. True when a new cover just became ready. */
bool gui_subsonic_album_cover_poll(void);

/* The ready cover for `cover_id`, or NULL. Valid until release or another request. */
const lv_image_dsc_t * gui_subsonic_album_cover_get(const char * cover_id);

/* Cancels the job and frees the buffer; the image showing it must be gone. Never blocks. */
void gui_subsonic_album_cover_release(void);
