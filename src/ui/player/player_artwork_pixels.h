#ifndef PLAYER_ARTWORK_PIXELS_H
#define PLAYER_ARTWORK_PIXELS_H

#include <stdint.h>
#include <stdbool.h>
#include "frosted_glass.h"

/* Stateless pixel processing, also used by the cover worker. Buffers retain
 * their existing caller ownership; cancellation is forwarded to the blur. */
bool player_cover_accent_from_rgb565(const uint16_t * px, uint32_t * out_rgb);
uint8_t * compute_reflection_bytes(const uint8_t * cover_bytes, int blur_radius, int blur_passes, int darken_num, int darken_den);
uint8_t * player_compute_reflection_bytes_cancelable(const uint8_t * cover_bytes,
                                                    int blur_radius, int blur_passes,
                                                    int darken_num, int darken_den,
                                                    uint32_t tint_rgb, int tint_weight,
                                                    frosted_glass_cancel_fn cancel_cb,
                                                    void * user_data);

#endif /* PLAYER_ARTWORK_PIXELS_H */
