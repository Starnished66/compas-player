/* White-box wrapper around the real timeline-label proxy eligibility path. */
#include "../src/ui/fallback_font.h"
#include "../src/ui/player_timeline_labels.c"

#include <assert.h>

bool rtl_text_test_timeline_proxy(const char * text) {
    assert(text);
    lv_obj_t * screen = lv_obj_create(NULL);
    assert(screen);
    lv_obj_t * label = lv_label_create(screen);
    assert(label);
    lv_obj_set_style_width(label, 300, LV_PART_MAIN);
    lv_obj_set_style_height(label, 48, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &app_font_22, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    assert(lv_obj_get_style_base_dir(label, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(label, text);
    lv_obj_update_layout(screen);

    label_proxy_t proxy = {0};
    proxy.label = label;
    bool eligible = make_proxy(&proxy);
    release_proxy(&proxy, false);
    lv_obj_delete(screen);
    return eligible;
}
