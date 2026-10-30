#ifndef PLAYER_LAYOUT_INTERNAL_H
#define PLAYER_LAYOUT_INTERNAL_H

#include "../../widgets/builders/screen_builders.h"
#include <stdint.h>

static inline int32_t player_x(int32_t px) { return BOARD_SCALE_PX(px); }
static inline int32_t player_y(int32_t px) { return (int32_t) (((int64_t) px * BOARD_SCREEN_HEIGHT + 400) / 800); }
static inline int32_t player_s(int32_t px) {
    int32_t sx = player_x(px), sy = player_y(px);
    return sx < sy ? sx : sy;
}

static inline void player_fit_small_screen_icon(lv_obj_t * icon, int32_t reference_px) {
#if defined(BOARD_R3II_2025) || defined(BOARD_TEMPOTEC_V1)
    /* Compact board transport PNGs still have the reference board's dimensions.
     * Contain scales their paint AND layout box, including later source swaps
     * for pressed/play/pause states. XML layouts retain their own sizing. */
    int32_t size = player_s(reference_px);
    lv_obj_set_size(icon, size, size);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
#else
    (void) icon;
    (void) reference_px;
#endif
}

static inline void player_init_overlay_panel(lv_obj_t * panel) {
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_size(panel, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_add_style(panel, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

static inline lv_obj_t * player_create_hidden_background_img(lv_obj_t * panel) {
    lv_obj_t * img = lv_image_create(panel);
    lv_obj_set_name_static(img, "background_img");
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

void gui_player_prepare_play_btn_images(void);

#endif /* PLAYER_LAYOUT_INTERNAL_H */
