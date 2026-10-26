#include "player_artwork_pixels.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../application/gui.h"
#include "lvgl/lvgl.h"

/* Generates a "frosted mirror" look behind the transport controls
 * (player_overlay_panel, built in build_player_screen()): a heavily
 * blurred, darkened, vertically-mirrored copy of the album art,
 * replacing the flat buttom.png placeholder. Generated fresh from the
 * per-track RGB565 buffer decoded by cover_decode_to_rgb565(). */

/* One accent pass: samples a grid, weights each pixel at least min_s
 * saturated and min_v bright by saturation x value into 15-degree hue
 * buckets, takes the strongest three-bucket window, and lifts its average
 * so it still reads as an accent on dark and light surfaces. False unless
 * that window averages min_weight per sample. */
static bool cover_accent_pass(const uint16_t * px, uint8_t min_s, uint8_t min_v, uint32_t min_weight,
                              uint32_t * out_rgb) {
    enum { BUCKETS = 24, BUCKET_DEG = 15, STEP = 7 };
    uint64_t weight[BUCKETS] = {0}, hue_off[BUCKETS] = {0}, sat[BUCKETS] = {0}, val[BUCKETS] = {0};
    uint32_t samples = 0;
    for (int y = STEP / 2; y < COVER_ART_HEIGHT; y += STEP) {
        for (int x = STEP / 2; x < COVER_ART_WIDTH; x += STEP) {
            uint16_t c = px[y * COVER_ART_WIDTH + x];
            uint8_t r = (uint8_t) (((c >> 11) << 3) | (c >> 13));
            uint8_t g = (uint8_t) ((((c >> 5) & 0x3F) << 2) | ((c >> 9) & 0x03));
            uint8_t b = (uint8_t) (((c & 0x1F) << 3) | ((c >> 2) & 0x07));
            lv_color_hsv_t hsv = lv_color_rgb_to_hsv(r, g, b);
            samples++;
            if (hsv.s < min_s || hsv.v < min_v) continue;
            uint32_t w = (uint32_t) hsv.s * hsv.v;
            int k = (hsv.h % 360) / BUCKET_DEG;
            weight[k] += w;
            hue_off[k] += (uint64_t) w * (hsv.h % 360 - k * BUCKET_DEG);
            sat[k] += (uint64_t) w * hsv.s;
            val[k] += (uint64_t) w * hsv.v;
        }
    }
    int best = 0;
    uint64_t best_w = 0;
    for (int k = 0; k < BUCKETS; k++) {
        uint64_t w = weight[(k + BUCKETS - 1) % BUCKETS] + weight[k] + weight[(k + 1) % BUCKETS];
        if (w > best_w) { best_w = w; best = k; }
    }
    if (best_w == 0 || best_w < (uint64_t) samples * min_weight) return false;

    uint64_t hue_acc = 0, sat_acc = 0, val_acc = 0;
    for (int d = 0; d < 3; d++) {
        int k = (best - 1 + d + BUCKETS) % BUCKETS;
        hue_acc += weight[k] * (uint64_t) (d * BUCKET_DEG) + hue_off[k];
        sat_acc += sat[k];
        val_acc += val[k];
    }
    int h = (best - 1) * BUCKET_DEG + (int) (hue_acc / best_w);
    h = ((h % 360) + 360) % 360;
    int sv = (int) (sat_acc / best_w);
    int vv = (int) (val_acc / best_w);
    if (sv < 50) sv = 50;
    if (sv > 90) sv = 90;
    if (vv < 80) vv = 80;
    lv_color_t rgb = lv_color_hsv_to_rgb((uint16_t) h, (uint8_t) sv, (uint8_t) vv);
    *out_rgb = ((uint32_t) rgb.red << 16) | ((uint32_t) rgb.green << 8) | rgb.blue;
    return true;
}

/* "Match album art": the cover's most prominent vivid hue, where at least a
 * tenth of the cover is moderately colorful (s x v of 4000). Muted covers --
 * sepia, pastel, washed-out photos -- fall back to their dominant tint
 * (a tenth of the cover at s x v of 400), lifted like a vivid one, so they
 * still get their own accent instead of the fixed one; the app's artwork
 * accent does the same. False only for a gray or near-black cover. Runs on
 * the cover worker. */
bool player_cover_accent_from_rgb565(const uint16_t * px, uint32_t * out_rgb) {
    return cover_accent_pass(px, 25, 25, 400, out_rgb) || cover_accent_pass(px, 10, 10, 40, out_rgb);
}

uint8_t * compute_reflection_bytes(const uint8_t * cover_bytes, int blur_radius, int blur_passes, int darken_num, int darken_den) {
    return player_compute_reflection_bytes_cancelable(cover_bytes, blur_radius, blur_passes,
                                                      darken_num, darken_den, 0, 0, NULL, NULL);
}

uint8_t * player_compute_reflection_bytes_cancelable(const uint8_t * cover_bytes,
                                                    int blur_radius, int blur_passes,
                                                    int darken_num, int darken_den,
                                                    uint32_t tint_rgb, int tint_weight,
                                                    frosted_glass_cancel_fn cancel_cb,
                                                    void * user_data) {
    if (darken_den <= 0) darken_den = 1;
    if (blur_radius < 0) blur_radius = 0;
    if (blur_passes < 0) blur_passes = 0;
    if (darken_num < 0) darken_num = 0;

    /* Preserve the historical reflection dimensions, center crop, blur,
     * darkening, spatial dither, and unmirrored source pixel orientation. */
    return frosted_glass_blur_rgb565_tinted_cancelable((const uint16_t *) cover_bytes,
                                                       COVER_ART_WIDTH, COVER_ART_HEIGHT,
                                                       BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT,
                                                       blur_radius, blur_passes,
                                                       darken_num, darken_den,
                                                       tint_rgb, tint_weight, false,
                                                       cancel_cb, user_data);
}
