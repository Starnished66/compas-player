#include "../screen/gui_player_internal.h"
#include "../../text/rendering/ui_label_text.h"
#include <stdio.h>

/* Count/badge row geometry. The three widgets are owned by gui_player.c. */
typedef struct {
    bool valid;
    int32_t count_x, count_width;
    int32_t pill_x, pill_width, gap;
} player_count_row_geometry_t;
static player_count_row_geometry_t player_count_row_geometry;


/* Keep the original count and badge geometry as the baseline. Each update is
 * recomputed from that snapshot so changing queue or format text cannot cause
 * cumulative position or width drift. */
void player_set_song_count(int index, int total) {
    if (!song_count_label) return;
    char text[48];
    snprintf(text, sizeof(text), "%d/%d", index + 1, total);
    if (ui_label_text_matches(song_count_label, text)) return;
    lv_label_set_text(song_count_label, text);
    player_count_row_refresh();
}

void player_set_format_badge_text(const char * text) {
    if (!format_badge_label) return;
    if (ui_label_text_matches(format_badge_label, text)) return;
    lv_label_set_text(format_badge_label, text);
    player_count_row_refresh();
}

static int32_t player_label_text_width(lv_obj_t * label) {
    if (!label) return 0;
    const char * text = lv_label_get_text(label);
    const lv_font_t * font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (!text || !font || !text[0]) return 0;
    lv_point_t size;
    lv_text_get_size(&size, text, font,
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x > 0 ? size.x : 0;
}

void player_count_row_refresh(void) {
    player_count_row_geometry_t * g = &player_count_row_geometry;
    if (!g->valid || !song_count_label || !quality_pill || !format_badge_label) return;

    const int32_t count_text_width = player_label_text_width(song_count_label);
    const int32_t count_content_width = count_text_width + (count_text_width > 0 ? 1 : 0) +
        lv_obj_get_style_pad_left(song_count_label, LV_PART_MAIN) +
        lv_obj_get_style_pad_right(song_count_label, LV_PART_MAIN) +
        2 * lv_obj_get_style_border_width(song_count_label, LV_PART_MAIN);
    int32_t badge_text_width = player_label_text_width(format_badge_label);
    if (badge_text_width == 0) {
        /* An empty badge keeps the full authored slot available for a later
         * format update, avoiding a needless jump when metadata arrives. */
        badge_text_width = g->pill_width;
    } else {
        badge_text_width += lv_obj_get_style_pad_left(format_badge_label, LV_PART_MAIN) +
                            lv_obj_get_style_pad_right(format_badge_label, LV_PART_MAIN) +
                            2 * lv_obj_get_style_border_width(format_badge_label, LV_PART_MAIN) +
                            lv_obj_get_style_pad_left(quality_pill, LV_PART_MAIN) +
                            lv_obj_get_style_pad_right(quality_pill, LV_PART_MAIN) +
                            2 * lv_obj_get_style_border_width(quality_pill, LV_PART_MAIN);
    }

    int32_t count_width = count_content_width > g->count_width ? count_content_width : g->count_width;
    int32_t growth_limit = g->pill_width > badge_text_width ? g->pill_width - badge_text_width : 0;
    if (count_width > g->count_width + growth_limit)
        count_width = g->count_width + growth_limit;

    int32_t pill_x = g->count_x + count_width + g->gap;
    int32_t pill_width = g->pill_x + g->pill_width - pill_x;
    lv_obj_set_width(song_count_label, count_width);
    lv_obj_set_x(quality_pill, pill_x);
    lv_obj_set_width(quality_pill, pill_width);
}

void player_count_row_capture(lv_obj_t * count, lv_obj_t * pill, lv_obj_t * badge) {
    player_count_row_geometry = (player_count_row_geometry_t){0};
    if (!count || !pill || !badge || lv_obj_get_parent(count) != lv_obj_get_parent(pill) ||
        lv_obj_get_parent(badge) != pill || lv_obj_get_child_count(pill) != 1) return;
    lv_obj_t * parent = lv_obj_get_parent(count);
    if (!parent || lv_obj_get_style_layout(parent, LV_PART_MAIN) != LV_LAYOUT_NONE) return;
    lv_align_t count_align = lv_obj_get_style_align(count, LV_PART_MAIN);
    lv_align_t pill_align = lv_obj_get_style_align(pill, LV_PART_MAIN);
    lv_align_t badge_align = lv_obj_get_style_align(badge, LV_PART_MAIN);
    if ((count_align != LV_ALIGN_DEFAULT && count_align != LV_ALIGN_TOP_LEFT) ||
        (pill_align != LV_ALIGN_DEFAULT && pill_align != LV_ALIGN_TOP_LEFT) ||
        (badge_align != LV_ALIGN_DEFAULT && badge_align != LV_ALIGN_TOP_LEFT) ||
        lv_obj_get_style_x(count, LV_PART_MAIN) != lv_obj_get_x(count) ||
        lv_obj_get_style_y(count, LV_PART_MAIN) != lv_obj_get_y(count) ||
        lv_obj_get_style_x(pill, LV_PART_MAIN) != lv_obj_get_x(pill) ||
        lv_obj_get_style_y(pill, LV_PART_MAIN) != lv_obj_get_y(pill) ||
        !LV_COORD_IS_PX(lv_obj_get_style_x(count, LV_PART_MAIN)) ||
        !LV_COORD_IS_PX(lv_obj_get_style_y(count, LV_PART_MAIN)) ||
        !LV_COORD_IS_PX(lv_obj_get_style_x(pill, LV_PART_MAIN)) ||
        !LV_COORD_IS_PX(lv_obj_get_style_y(pill, LV_PART_MAIN)) ||
        lv_obj_get_style_x(badge, LV_PART_MAIN) != 0 ||
        !LV_COORD_IS_PCT(lv_obj_get_style_width(badge, LV_PART_MAIN)) ||
        LV_COORD_GET_PCT(lv_obj_get_style_width(badge, LV_PART_MAIN)) != 100) return;
    int32_t count_authored_width = lv_obj_get_style_width(count, LV_PART_MAIN);
    int32_t pill_authored_width = lv_obj_get_style_width(pill, LV_PART_MAIN);
    if (!LV_COORD_IS_PX(count_authored_width) || count_authored_width <= 0 ||
        !LV_COORD_IS_PX(pill_authored_width) || pill_authored_width <= 0) return;
    int32_t count_x = lv_obj_get_x(count), pill_x = lv_obj_get_x(pill);
    int32_t count_width = lv_obj_get_width(count), pill_width = lv_obj_get_width(pill);
    int32_t gap = pill_x - (count_x + count_width);
    if (count_width <= 0 || pill_width <= 0 || gap < 0 ||
        lv_obj_get_y(count) + lv_obj_get_height(count) <= lv_obj_get_y(pill) ||
        lv_obj_get_y(pill) + lv_obj_get_height(pill) <= lv_obj_get_y(count)) return;

    player_count_row_geometry = (player_count_row_geometry_t){
        .valid = true,
        .count_x = count_x,
        .count_width = count_width,
        .pill_x = pill_x,
        .pill_width = pill_width,
        .gap = gap,
    };
}

void player_count_row_reset(void) {
    player_count_row_geometry = (player_count_row_geometry_t){0};
}
