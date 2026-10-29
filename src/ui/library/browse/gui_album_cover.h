#pragma once
/* A sharp album cover for the album header, decoded off the UI thread from the
 * player-size cover cache (or generated into it when missing) and scaled to
 * the header size. One small RGB565 buffer is held for the album on screen. */
#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>

/* UI thread. Starts decoding for `song_id`'s album unless already done, in
 * flight or failed. Drops a buffer that belongs to another album. */
void gui_album_cover_request(int64_t song_id, int px);
/* UI thread. Reaps a finished job. Returns true when a new cover just became
 * ready. */
bool gui_album_cover_poll(bool start_allowed);
/* The ready cover for `song_id`, or NULL. Valid until release or another
 * request. */
const lv_image_dsc_t * gui_album_cover_get(int64_t song_id);
/* Cancels any job and frees the buffer. The image using it must be gone.
 * Never blocks; the cancelled job is reaped by a later poll. */
void gui_album_cover_release(void);
/* True while a decode job is running. */
bool gui_album_cover_busy(void);
/* Blocking final-shutdown cleanup. */
void gui_album_cover_shutdown(void);
