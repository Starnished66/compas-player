#include "gui_rating_popup.h"
#include "star_rating.h"
#include "../../appearance/gui_theme.h"
#include "../../../library/workers/track_metadata_worker.h"
#include <stdio.h>
#include "../builders/screen_builders.h"
#include "../../text/i18n/i18n.h"

static gui_popup_t popup;
static lv_obj_t * title_label, * rating;
static gui_rating_popup_cb_t callback;
static void * callback_data;

static uint64_t target_token;
static lv_timer_t *rating_timer;

static void stop_loading(void) {
    ++target_token;
    if (rating_timer) lv_timer_delete(rating_timer);
    rating_timer = NULL;
}

static void rating_poll(lv_timer_t *timer) {
    (void)timer;
    track_metadata_result_t result;
    if (!track_metadata_take(TRACK_METADATA_POPUP, &result)) return;
    if (result.request.token == target_token && rating)
        star_rating_set(rating, result.stars);
    stop_loading();
}

void gui_rating_popup_load_stars(const char *key) {
    if (!rating) return;
    stop_loading();
    track_metadata_request_t request = {.token = target_token};
    snprintf(request.path, sizeof(request.path), "%s", key);
    if (track_metadata_submit(TRACK_METADATA_POPUP, &request))
        rating_timer = lv_timer_create(rating_poll, 25, NULL);
}

static void close_popup(lv_event_t * e) {
    (void) e;
    stop_loading();
    gui_popup_hide(&popup);
    callback = NULL;
    callback_data = NULL;
}

static void rating_changed(int stars, void * data) {
    (void) data;
    gui_rating_popup_cb_t cb = callback;
    void * user_data = callback_data;
    close_popup(NULL);
    if (cb) cb(stars, user_data);
}

static lv_obj_t * create_rating_row(void) {
    lv_obj_update_layout(popup.popup);
    int32_t gap = BOARD_SCALE_PX(8);
    int32_t px = (lv_obj_get_content_width(popup.popup) - 4 * gap) / 5;
    px = LV_CLAMP(BOARD_SCALE_PX(48), px, BOARD_SCALE_PX(64));
    lv_obj_t * row = star_rating_create(popup.popup, px, false);
    if (!row) return NULL;
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_set_style_pad_ver(row, BOARD_SCALE_PX(14), 0);
    star_rating_set_on_change(row, rating_changed, NULL);
    return row;
}

void gui_rating_popup_open(const char * title, int stars, gui_rating_popup_cb_t cb, void * user_data) {
    stop_loading();
    if (!popup.popup) {
        build_titled_popup(&popup, close_popup, &title_label, NULL);
        rating = create_rating_row();
        if (!rating) { gui_rating_popup_teardown(); return; }
        add_popup_action_row(popup.popup, TR("Cancel"), lv_color_hex(GUI_BREEZE_NEGATIVE), close_popup, NULL);
    }
    callback = cb;
    callback_data = user_data;
    lv_label_set_text(title_label, title ? title : TR("Rate song"));
    star_rating_set_lit_color(rating, lv_color_hex(gui_theme_accent_rgb()));
    star_rating_set(rating, stars);
    gui_popup_show(&popup);
}

void gui_rating_popup_teardown(void) {
    stop_loading();
    gui_popup_teardown(&popup);
    title_label = rating = NULL;
    callback = NULL;
    callback_data = NULL;
}
