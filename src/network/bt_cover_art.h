#ifndef BT_COVER_ART_H
#define BT_COVER_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Starts (or re-observes) the current track. The serial changes only when
 * track_key differs from the prior key; NULL represents a distinct no-art
 * track state. A changed key clears the published URL. */
uint64_t bt_cover_art_begin_track(const char * track_key);

/* Encodes a row-major RGB565 image to a 200x200 baseline JPEG and publishes
 * its file URL if token still identifies the current track. Intended for the
 * cover worker thread, never the GUI thread. */
bool bt_cover_art_publish_rgb565(uint64_t token, const uint16_t * pixels,
                                 int width, int height, uint64_t image_key);

void bt_cover_art_clear(uint64_t token);

/* Reuses a retained JPEG only when the worker confirms the same source file
 * identity. image_key=0 is reserved for uncached sources. */
bool bt_cover_art_reuse(uint64_t token, uint64_t image_key);

/* Copies the currently published file URL; returns false if none is set or
 * the output buffer is too small. */
bool bt_cover_art_get_url(char * buffer, size_t size);

#endif /* BT_COVER_ART_H */
