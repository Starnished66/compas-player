#include "screen_builders.h"
#include "screen_builders_internal.h"
#include "../lists/scroll_blit.h"
#include "lvgl/src/themes/lv_theme_private.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/widgets/label/lv_label_private.h"

#include <string.h>

lv_style_t list_row_style;
lv_style_t list_row_pressed_style;
lv_style_t pill_row_bg_style;
lv_style_t native_row_min_style;
lv_style_t icon_press_style;
static lv_style_t row_marquee_style;
static lv_anim_t row_marquee_anim;

/* Card tiles: a rounded card with a hairline Breeze outline. */
#define ICON_GRID_TILE_RADIUS BOARD_SCALE_PX(20)

/* Theming: three shared, mutable-in-place bg_color styles, one per
 * "background category" the app has -- every screen root
 * (style_theme_screen_bg), every popup/EQ-card/slider-card
 * (style_theme_card_bg), and every list row (list_row_style itself,
 * reused directly -- no separate style needed, its bg_color just needs to
 * become live-mutable too). Declared here (not gui.c) since screen_builders.c
 * already owns list_row_style, the one existing precedent for a style
 * shared across both files; gui.c's plugin_manager bridge
 * (gui_plugin_set_background_color()) mutates whichever of these three a
 * plugin's plugin.set_background_color() call names, then
 * lv_obj_report_style_change()s it -- the exact same "shared style updated
 * in place + report_style_change" mechanism apply_accent_color() already
 * uses successfully for the app's existing accent color feature. Not
 * style_accent itself: that style already claims bg_color for its own
 * purpose (slider/switch fill), so reusing it here would wrongly tie
 * every screen's background to whatever accent color is picked. */
lv_style_t style_theme_screen_bg;
lv_style_t style_theme_card_bg;
lv_style_t style_theme_tile;
lv_style_t style_theme_tile_pressed;
lv_style_t style_theme_scrollbar;
lv_style_t style_theme_switch_track;
lv_style_t style_theme_switch_knob;

/* Same mechanism, for text color -- see screen_builders.h's own comment on
 * style_theme_text_primary/style_theme_text_muted for why destructive-red
 * and accent-tinted text are deliberately excluded. */
lv_style_t style_theme_text_primary;
lv_style_t style_theme_text_muted;

void gui_style_settings_dropdown(lv_obj_t * dropdown) {
    lv_obj_add_style(dropdown, &style_theme_card_bg, 0);
    lv_obj_add_style(dropdown, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(dropdown, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_all(dropdown, BOARD_SCALE_PX(16), 0);
    lv_obj_t * list = lv_dropdown_get_list(dropdown);
    lv_obj_add_style(list, &style_theme_card_bg, 0);
    lv_obj_add_style(list, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(list, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_style(list, gui_theme_accent_style(), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, lv_color_black(), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_pad_ver(list, BOARD_SCALE_PX(12), 0);
}

/* Runtime geometry replaces the original panel-specific 448/464/476px
 * constants. Rows are Breeze cards inset from both screen edges, the same
 * edge spacing as the Home cards; lists centre them. */
int32_t screen_builders_active_display_width(void) {
    lv_display_t * display = lv_display_get_default();
    int32_t width = display ? lv_display_get_horizontal_resolution(display) : 480;
    return width > 0 ? width : 480;
}

/* Keep the scrollbar easy to follow while it is moving, then let LVGL hide
 * it again. Breeze draws it neutral, in the theme's secondary text color. */
void screen_builders_configure_content_scrollbar(lv_obj_t * list) {
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list, BOARD_SCALE_PX(3), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, BOARD_SCALE_PX(2), LV_PART_SCROLLBAR);
    lv_obj_add_style(list, &style_theme_scrollbar, LV_PART_SCROLLBAR);
}

lv_obj_t * build_song_list_body(lv_obj_t * screen) {
    lv_obj_t * list = lv_obj_create(screen);
    lv_obj_set_size(list, lv_pct(100), lv_display_get_vertical_resolution(lv_display_get_default()) -
                    STATUS_BAR_CLEARANCE - TITLE_ROW_HEIGHT - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_top(list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_bottom(list, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_gap(list, GUI_ROW_GAP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    ui_scroll_blit_enable(list);
    return list;
}

int32_t ui_list_row_width(void) {
    return screen_builders_active_display_width() - 2 * GUI_LIST_SIDE_MARGIN;
}

/* Every switch, app-wide, in Breeze's form: a frame-grey track, the accent
 * when on (the accent style the switches already carry), and a knob in the
 * text color. A theme layer over LVGL's default reaches each lv_switch as it
 * is created, so no call site has to remember it. */
static lv_theme_t breeze_theme;
static void breeze_theme_apply(lv_theme_t * theme, lv_obj_t * obj) {
    (void)theme;
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &style_theme_switch_track, LV_PART_MAIN);
        lv_obj_add_style(obj, &style_theme_switch_knob, LV_PART_KNOB);
    }
}
static void install_breeze_theme(void) {
    static bool installed;
    lv_display_t * display = lv_display_get_default();
    if (installed || !display) return;
    lv_theme_t * base = lv_display_get_theme(display);
    if (!base) return;
    breeze_theme = *base;
    lv_theme_set_parent(&breeze_theme, base);
    lv_theme_set_apply_cb(&breeze_theme, breeze_theme_apply);
    lv_display_set_theme(display, &breeze_theme);
    installed = true;
}

void screen_builders_init_list_row_style(void) {
    /* gui_reload.c's in-process UI reload calls this a second (or Nth) time
     * in the same process -- LVGL's own lv_style_init() header comment warns
     * against calling it on a style that already has properties ("Potential
     * memory leak"): it just lv_memzero()s the struct without freeing the
     * existing values_and_props allocation first, unlike lv_style_reset(),
     * which frees it before zeroing. Skip the reset on this function's
     * very-first-ever call (a freshly zero-initialized static lv_style_t has
     * no allocation to free, and LV_USE_ASSERT_STYLE builds would otherwise
     * trip lv_style_reset()'s own sentinel assert on it). */
    static bool already_initialized = false;
    if (already_initialized) {
        lv_style_reset(&list_row_style);
        lv_style_reset(&pill_row_bg_style);
        lv_style_reset(&native_row_min_style);
        lv_style_reset(&list_row_pressed_style);
        lv_style_reset(&icon_press_style);
        lv_style_reset(&row_marquee_style);
        lv_style_reset(&style_theme_screen_bg);
        lv_style_reset(&style_theme_card_bg);
        lv_style_reset(&style_theme_text_primary);
        lv_style_reset(&style_theme_text_muted);
        lv_style_reset(&style_theme_tile);
        lv_style_reset(&style_theme_tile_pressed);
        lv_style_reset(&style_theme_scrollbar);
        lv_style_reset(&style_theme_switch_track);
        lv_style_reset(&style_theme_switch_knob);
    }
    already_initialized = true;

    lv_style_init(&list_row_style);
    lv_style_init(&native_row_min_style);
    lv_style_set_min_height(&native_row_min_style, GUI_ROW_HEIGHT);
    lv_style_set_width(&list_row_style, LIST_ROW_WIDTH);
    lv_style_set_height(&list_row_style, LIST_ROW_HEIGHT);
    lv_style_set_radius(&list_row_style, LIST_ROW_RADIUS);
    lv_style_set_bg_color(&list_row_style, LIST_ROW_BG_COLOR);
    lv_style_set_bg_opa(&list_row_style, LV_OPA_COVER);
    lv_style_set_border_width(&list_row_style, 0);
    lv_style_set_pad_left(&list_row_style, LIST_ROW_LABEL_INSET);
    lv_style_set_pad_top(&list_row_style, (LIST_ROW_HEIGHT - lv_font_get_line_height(&LIST_ROW_FONT)) / 2);
    lv_style_set_text_color(&list_row_style, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_text_font(&list_row_style, &LIST_ROW_FONT);

    /* Background/radius only (no padding/dimensions/text) for pill rows
     * that manage child layout with explicit lv_obj_align() offsets.
     * Keeps pill row backgrounds in sync with the plugin-mutable list_row slot
     * without shifting the alignment origin. */
    lv_style_init(&pill_row_bg_style);
    lv_style_set_radius(&pill_row_bg_style, LIST_ROW_RADIUS);
    lv_style_set_bg_color(&pill_row_bg_style, LIST_ROW_BG_COLOR);
    lv_style_set_bg_opa(&pill_row_bg_style, LV_OPA_COVER);
    lv_style_set_border_width(&pill_row_bg_style, 0);

    /* Visual feedback for pressed song rows (attached with LV_STATE_PRESSED).
     * Provides a lighter background and recolor overlay during tap or hold. */
    lv_style_init(&list_row_pressed_style);
    lv_style_set_bg_color(&list_row_pressed_style, lv_color_hex(GUI_COLOR_PRESSED));
    lv_style_set_bg_opa(&list_row_pressed_style, LV_OPA_COVER);
    lv_style_set_bg_image_recolor(&list_row_pressed_style, lv_color_make(255, 255, 255));
    lv_style_set_bg_image_recolor_opa(&list_row_pressed_style, LV_OPA_30);

    /* Touch-press feedback for player screen icon buttons.
     * Darkens the icon via partial-opacity black recolor without requiring separate pressed assets. */
    lv_style_init(&icon_press_style);
    lv_style_set_image_recolor(&icon_press_style, lv_color_make(0, 0, 0));
    lv_style_set_image_recolor_opa(&icon_press_style, LV_OPA_40);

    lv_anim_init(&row_marquee_anim);
    lv_anim_set_delay(&row_marquee_anim, 2000);
    lv_anim_set_repeat_delay(&row_marquee_anim, 2000);
    /* LVGL copies the template's repeat count over the label's own infinite
     * one (overwrite_anim_property); lv_anim_init() leaves it at 1, so the
     * marquee ran a single pass and then stopped for good. */
    lv_anim_set_repeat_count(&row_marquee_anim, LV_ANIM_REPEAT_INFINITE);
    lv_style_init(&row_marquee_style);
    lv_style_set_anim(&row_marquee_style, &row_marquee_anim);
    /* LVGL's label default is 40px/s capped at 10 seconds. That cap makes
     * very long titles accelerate while modest overflows remain slower.
     * A lower constant speed with a much larger cap keeps the perceived
     * rate uniform across both cases. */
    lv_style_set_anim_duration(&row_marquee_style, lv_anim_speed_clamped(30, 600, 30000));

    /* Charcoal surface hierarchy; plugin palette setters update these live. */
    lv_style_init(&style_theme_screen_bg);
    lv_style_set_bg_color(&style_theme_screen_bg, lv_color_hex(GUI_COLOR_SCREEN));
    lv_style_init(&style_theme_card_bg);
    lv_style_set_bg_color(&style_theme_card_bg, lv_color_hex(GUI_COLOR_PANEL));
    lv_style_set_border_color(&style_theme_card_bg, lv_color_hex(GUI_COLOR_BORDER));

    /* Semantic text roles follow the same live theme override mechanism. */
    lv_style_init(&style_theme_text_primary);
    lv_style_set_text_color(&style_theme_text_primary, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_image_recolor(&style_theme_text_primary, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_image_recolor_opa(&style_theme_text_primary, LV_OPA_COVER);
    lv_style_init(&style_theme_text_muted);
    lv_style_set_text_color(&style_theme_text_muted, lv_color_hex(GUI_COLOR_SECONDARY));
    lv_style_set_image_recolor(&style_theme_text_muted, lv_color_hex(GUI_COLOR_SECONDARY));
    lv_style_set_image_recolor_opa(&style_theme_text_muted, LV_OPA_COVER);

    lv_style_init(&style_theme_tile);
    lv_style_set_bg_opa(&style_theme_tile, LV_OPA_COVER);
    lv_style_set_border_width(&style_theme_tile, ICON_GRID_TILE_BORDER);
    lv_style_set_border_opa(&style_theme_tile, LV_OPA_COVER);
    lv_style_set_radius(&style_theme_tile, ICON_GRID_TILE_RADIUS);
    lv_style_init(&style_theme_tile_pressed);
    lv_style_set_bg_opa(&style_theme_tile_pressed, LV_OPA_COVER);

    /* Rows carry the same Breeze frame as the tiles. */
    lv_style_set_border_width(&list_row_style, ICON_GRID_TILE_BORDER);
    lv_style_set_border_opa(&list_row_style, LV_OPA_COVER);
    lv_style_set_border_width(&pill_row_bg_style, ICON_GRID_TILE_BORDER);
    lv_style_set_border_opa(&pill_row_bg_style, LV_OPA_COVER);
    lv_style_init(&style_theme_scrollbar);
    lv_style_set_bg_opa(&style_theme_scrollbar, LV_OPA_COVER);
    lv_style_init(&style_theme_switch_track);
    lv_style_set_bg_opa(&style_theme_switch_track, LV_OPA_COVER);
    lv_style_init(&style_theme_switch_knob);
    lv_style_set_bg_opa(&style_theme_switch_knob, LV_OPA_COVER);
    lv_style_set_shadow_width(&style_theme_switch_knob, 0);
    install_breeze_theme();
    gui_theme_update_surface_contrast();
}

int32_t row_label_descender_margin(const lv_font_t * font) {
    int32_t margin = (font ? lv_font_get_line_height(font) : 24) / 8;
    return margin < 4 ? 4 : margin;
}

/* The height row_label_apply_bounded_height() will set for this font, for
 * callers that must position a following element without reading the label's
 * height back (lv_obj_get_height() returns a stale value until the next
 * layout pass). */
int32_t row_label_bounded_height(const lv_font_t * font) {
    return (font ? lv_font_get_line_height(font) : 24) + row_label_descender_margin(font) * 2;
}

/* Applies a bounded height and descender margin to scrolling row labels,
 * preventing descender clipping and spurious vertical scroll animation.
 * Adds proportional top padding to maintain vertical centering within the row. */
void row_label_apply_bounded_height(lv_obj_t * label, const lv_font_t * font) {
    lv_obj_set_height(label, row_label_bounded_height(font));
    lv_obj_set_style_pad_top(label, row_label_descender_margin(font), 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_USER_3);
}

static void row_label_layout_identity(lv_obj_t * row, lv_obj_t * secondary, int32_t height) {
    bool has_subtitle = lv_label_get_text(secondary)[0] != '\0';
    int32_t title_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_ROW));
    int32_t sub_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
    int32_t text_h = title_h + (has_subtitle ? sub_h + 8 : 0);
    int32_t top = (height - text_h) / 2;
    if (top < 0) top = 0;
    int32_t old_top = lv_obj_get_style_pad_top(row, 0);
    lv_obj_set_style_height(row, height, 0);
    lv_obj_set_style_pad_top(row, top, 0);
    /* Native editing buttons and badges align against the padded content
     * box. Keep their physical centre stable across font-tier changes. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        lv_obj_t * child = lv_obj_get_child(row, i);
        if (child == secondary) continue;
        lv_align_t align = lv_obj_get_style_align(child, 0);
        if (align == LV_ALIGN_LEFT_MID || align == LV_ALIGN_RIGHT_MID)
            lv_obj_set_style_y(child, lv_obj_get_style_y(child, 0) + old_top / 2 - top / 2, 0);
    }
    if (has_subtitle) {
        lv_obj_set_width(secondary, lv_pct(100));
        lv_obj_set_height(secondary, sub_h + 4);
        lv_obj_set_pos(secondary, 0, title_h + 8);
        lv_obj_remove_flag(secondary, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(secondary, LV_OBJ_FLAG_HIDDEN);
}

static char music_list_row_layout_marker;
static char music_list_row_badge_marker;

static void layout_music_list_row(lv_obj_t * row) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    lv_obj_t * title = lv_obj_get_child(row, 0);
    lv_obj_t * subtitle = lv_obj_get_child(row, 1);
    bool has_subtitle = lv_label_get_text(subtitle)[0] != '\0';
    const lv_font_t *title_font = lv_obj_get_style_text_font(title, LV_PART_MAIN);
    const lv_font_t *subtitle_font = lv_obj_get_style_text_font(subtitle, LV_PART_MAIN);
    int32_t title_height = row_label_bounded_height(title_font);
    int32_t subtitle_height = row_label_bounded_height(subtitle_font);
    int32_t gap = has_subtitle ? BOARD_SCALE_PX(4) : 0;
    int32_t group_height = title_height + (has_subtitle ? gap + subtitle_height : 0);
    int32_t height = group_height + 2 * BOARD_SCALE_PX(8);
    if (height < ui_music_row_height()) height = ui_music_row_height();
    int32_t top = (height - group_height) / 2;
    lv_obj_set_height(row, height);
    lv_obj_set_y(title, top);
    if (has_subtitle) {
        lv_obj_set_y(subtitle, top + title_height + gap);
        lv_obj_remove_flag(subtitle, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(subtitle, LV_OBJ_FLAG_HIDDEN);
    }
}

static void layout_music_list_badge(lv_obj_t * badge) {
    if (!badge) return;
    lv_obj_t * row = lv_obj_get_parent(badge);
    if (!row || lv_obj_get_child_count(row) < 2) return;
    lv_obj_t * title = lv_obj_get_child(row, 0);
    lv_obj_t * subtitle = lv_obj_get_child(row, 1);
    lv_obj_t * anchor = lv_obj_has_flag(subtitle, LV_OBJ_FLAG_HIDDEN) ? title : subtitle;
    int32_t anchor_height = row_label_bounded_height(
        lv_obj_get_style_text_font(anchor, LV_PART_MAIN));
    int32_t badge_height = row_label_bounded_height(lv_obj_get_style_text_font(badge, LV_PART_MAIN));
    lv_obj_set_y(badge, lv_obj_get_style_y(anchor, 0) +
                        (anchor_height - badge_height) / 2);
}

void music_list_row_align_badge(lv_obj_t * row, lv_obj_t * badge) {
    if (!row || !badge || lv_obj_get_parent(badge) != row) return;
    lv_obj_set_user_data(badge, &music_list_row_badge_marker);
    layout_music_list_badge(badge);
}

static void refresh_icon_caption_geometry_recursive(lv_obj_t * obj) {
    if (lv_obj_get_user_data(obj) == &music_list_row_layout_marker)
        layout_music_list_row(obj);
    /* Reserved for native headers, secondary labels and virtual lists. */
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_4)) {
        if (!lv_obj_check_type(obj, &lv_label_class)) compact_list_refresh_font_layout(obj);
        else if (lv_obj_get_user_data(obj)) {
            int32_t height = (int32_t)(intptr_t)lv_obj_get_user_data(obj);
            lv_obj_t * row_parent = lv_obj_get_parent(lv_obj_get_parent(obj));
            bool pooled = row_parent && lv_obj_has_flag(row_parent, LV_OBJ_FLAG_USER_4);
            if (!pooled && height >= MUSIC_LIST_ROW_HEIGHT) height = ui_music_row_height();
            row_label_layout_identity(lv_obj_get_parent(obj), obj, height);
        } else {
            screen_title_fit(obj);
        }
    }
    /* Re-applies row_label_apply_bounded_height() using this label's
     * current text_font -- after fallback_font_apply_size_tier()/
     * fallback_font_apply_custom() swap the stable app_font_* descriptors
     * in place, a label that already has one of them assigned as its
     * text_font picks up the new glyph metrics automatically the next time
     * it draws, but its own explicit height/pad_top (set once at
     * construction) does not move on its own. */
    if (lv_obj_check_type(obj, &lv_label_class) && lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_3)) {
        row_label_apply_bounded_height(obj, lv_obj_get_style_text_font(obj, LV_PART_MAIN));
    }
    if (lv_obj_check_type(obj, &lv_label_class) &&
        lv_obj_get_user_data(obj) == &music_list_row_badge_marker)
        layout_music_list_badge(obj);

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) icon_grid_refresh_tile(obj);

    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++)
        refresh_icon_caption_geometry_recursive(lv_obj_get_child(obj, i));
}

void screen_builders_refresh_font_geometry(lv_obj_t * root) {
    if (!root) {
        int32_t minimum = ui_list_row_height();
        lv_style_set_min_height(&native_row_min_style, minimum);
        lv_obj_report_style_change(&native_row_min_style);
        lv_style_set_height(&list_row_style, minimum);
        lv_style_set_pad_top(&list_row_style,
                             (minimum - lv_font_get_line_height(&LIST_ROW_FONT)) / 2);
        lv_style_set_text_font(&list_row_style, &LIST_ROW_FONT);
        lv_obj_report_style_change(&list_row_style);
        return;
    }
    lv_obj_update_layout(root);
    refresh_icon_caption_geometry_recursive(root);
    lv_obj_update_layout(root);
}

void screen_builders_refresh_all_font_geometry(void) {
    /* Shared styles first so native_row_min_style's new min_height is in
     * place before any screen's rows are remeasured. */
    screen_builders_refresh_font_geometry(NULL);
    lv_display_t * disp = lv_display_get_default();
    if (!disp) return;
    /* Every lv_obj_create(NULL) screen lives in this array for as long as
     * it exists -- Music Settings, Power, Playback, and the other Font Size
     * misses that are neither on the nav stack nor in the snapshot cache.
     * One walk, no per-module screen list to keep in sync. */
    for (uint32_t i = 0; i < disp->screen_cnt; i++) {
        if (disp->screens[i]) screen_builders_refresh_font_geometry(disp->screens[i]);
    }
}

void row_label_enable_marquee(lv_obj_t * label) {
    lv_obj_add_style(label, &row_marquee_style, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

/* The scroll animation, and with it the 2s start pause, is created when the
 * long mode is set; re-setting the mode deletes it and zeroes the offset
 * (see quick_drawer_restart_marquee()). Only labels with a running animation
 * overflow, so fitting ones skip the text re-measure. Plain recursion, no
 * allocation. */
void ui_restart_marquees(lv_obj_t * root) {
    if (lv_obj_check_type(root, &lv_label_class)) {
        if (lv_label_get_long_mode(root) == LV_LABEL_LONG_SCROLL_CIRCULAR && lv_anim_get(root, NULL))
            lv_label_set_long_mode(root, LV_LABEL_LONG_SCROLL_CIRCULAR);
        return;
    }
    uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; i++) ui_restart_marquees(lv_obj_get_child(root, i));
}

static void marquee_screen_loaded_cb(lv_event_t * e) {
    ui_restart_marquees(lv_event_get_param(e));
}

void ui_marquee_init(void) {
    lv_display_add_event_cb(lv_display_get_default(), marquee_screen_loaded_cb,
                            LV_EVENT_SCREEN_LOADED, NULL);
}

int32_t ui_list_row_height(void) {
    int32_t needed = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_ROW)) +
                     lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) + 32;
    return needed > GUI_ROW_HEIGHT ? needed : GUI_ROW_HEIGHT;
}

lv_obj_t * row_label_create_subtitle(lv_obj_t * row) {
    lv_obj_t * subtitle = lv_label_create(row);
    lv_obj_add_style(subtitle, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(subtitle, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    lv_obj_remove_flag(subtitle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(subtitle, LV_OBJ_FLAG_USER_4);
    lv_obj_set_user_data(subtitle, (void *)(intptr_t)LIST_ROW_HEIGHT);
    return subtitle;
}

/* True when `label` already holds exactly text[0..len) and no marquee animation would be restarted
 * by setting it again. */
static bool label_shows(lv_obj_t * label, const char * text, size_t len) {
    lv_label_long_mode_t mode = lv_label_get_long_mode(label);
    if (mode == LV_LABEL_LONG_SCROLL || mode == LV_LABEL_LONG_SCROLL_CIRCULAR) return false;
    /* A dotted label whose text is cut holds the dotted copy, not what was set: never equal. */
    if (mode == LV_LABEL_LONG_DOT && ((lv_label_t *) label)->dot_begin != 0xFFFFFFFFu) return false;
    const char * shown = lv_label_get_text(label);
    return shown && strlen(shown) == len && memcmp(shown, text, len) == 0;
}

void row_label_set_identity(lv_obj_t * row, lv_obj_t * secondary,
                            const char * title, const char * subtitle, int32_t height) {
    if (!title) title = "";
    const char * newline = strchr(title, '\n');
    if (!subtitle && newline) subtitle = newline + 1;
    size_t title_len = newline ? (size_t)(newline - title) : strlen(title);
    /* A rebind to the text the label already shows must not repaint it: the title label is the
     * whole row, so a same-text set would invalidate the entire row for a subtitle change. */
    if (!label_shows(row, title, title_len)) {
        if (newline) lv_label_set_text_fmt(row, "%.*s", (int)(newline - title), title);
        else lv_label_set_text(row, title);
    }
    bool has_subtitle = subtitle && subtitle[0];
    const char * second = has_subtitle ? subtitle : "";
    if (!label_shows(secondary, second, strlen(second))) lv_label_set_text(secondary, second);
    lv_obj_set_user_data(secondary, (void *)(intptr_t)height);
    row_label_layout_identity(row, secondary, height);
}

lv_obj_t * build_list_section(lv_obj_t * parent, const char * title) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, LIST_ROW_WIDTH_WIDE);
    lv_obj_set_style_pad_left(label, GUI_TEXT_INSET, 0);
    lv_obj_set_style_pad_top(label, 16, 0);
    lv_obj_set_style_pad_bottom(label, 4, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    return label;
}

/* One bounded, marquee-capable label of a music row, before its text is set. */
static lv_obj_t * music_row_label(lv_obj_t * row, lv_style_t * color_style, const lv_font_t * font,
                                  int32_t width) {
    lv_obj_t * label = lv_label_create(row);
    lv_obj_add_style(label, color_style, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_pos(label, GUI_TEXT_INSET, 0);
    lv_obj_set_width(label, width);
    row_label_apply_bounded_height(label, font);
    row_label_enable_marquee(label);
    return label;
}

lv_obj_t * build_music_list_row(lv_obj_t * parent, const char * title, const char * subtitle,
                               int32_t trailing_space) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_size(row, LIST_ROW_WIDTH, ui_music_row_height());
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(row, &music_list_row_layout_marker);

    const char * newline = title ? strchr(title, '\n') : NULL;
    if (!subtitle && newline) subtitle = newline + 1;
    /* The text goes in last: font, width, height and the scroll mode are settled first, so each
     * label measures its text once instead of after every one of those changes. */
    int32_t label_width = LIST_ROW_WIDTH - GUI_TEXT_INSET -
                          (trailing_space > 0 ? trailing_space : GUI_TEXT_INSET);
    lv_obj_t * primary = music_row_label(row, &style_theme_text_primary,
                                         gui_theme_font(GUI_FONT_ROLE_ROW), label_width);
    if (newline) lv_label_set_text_fmt(primary, "%.*s", (int)(newline - title), title);
    else lv_label_set_text(primary, title ? title : "");

    lv_obj_t * secondary = music_row_label(row, &style_theme_text_muted,
                                           gui_theme_font(GUI_FONT_ROLE_BODY), label_width);
    lv_label_set_text(secondary, subtitle ? subtitle : "");
    if (!subtitle || !subtitle[0]) lv_obj_add_flag(secondary, LV_OBJ_FLAG_HIDDEN);
    layout_music_list_row(row);
    return row;
}

lv_obj_t * build_list_message(lv_obj_t * parent, const char * title, const char * detail) {
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, LIST_ROW_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, GUI_TEXT_INSET, 0);
    lv_obj_set_style_pad_row(box, GUI_ROW_GAP, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * heading = lv_label_create(box);
    lv_obj_set_width(heading, lv_pct(100));
    lv_label_set_text(heading, title);
    lv_obj_add_style(heading, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(heading, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    if (detail && detail[0]) {
        lv_obj_t * description = lv_label_create(box);
        lv_obj_set_width(description, lv_pct(100));
        lv_label_set_text(description, detail);
        lv_obj_add_style(description, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(description, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    }
    return box;
}
