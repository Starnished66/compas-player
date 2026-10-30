#include "screen_builders.h"
#include "screen_builders_internal.h"
#include "lvgl/src/themes/lv_theme_private.h"
#include "../../assets/cover_card_preview.h"
#include "../../text/i18n/i18n.h"
#include "../../appearance/gui_theme.h"
#include "../../plugins/gui_plugins.h"
#include "../../shell/gui_navigation.h"
#include "../../assets/assets.h"
#include "../../appearance/gui_themes.h"
#include "../../../core/system/logging/debug_log.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/widgets/label/lv_label_private.h"

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point -- each file that needs it defines its own copy
   * rather than sharing a header, same convention gui.c/audio.c/
   * plugin_manager.c already use. Needed here for pill_row_apply_icon()'s
   * own relative-icon-path resolution -- see its own comment. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

lv_style_t list_row_style;
lv_style_t list_row_pressed_style;
lv_style_t pill_row_bg_style;
lv_style_t native_row_min_style;
lv_style_t icon_press_style;
static lv_style_t row_marquee_style;
static lv_anim_t row_marquee_anim;
#define ICON_GRID_ICON_LABEL_GAP 4
/* Card tiles: a rounded card with a hairline Breeze outline, and at least
 * this much space around and between cards so they read as separate. */
#define ICON_GRID_TILE_RADIUS BOARD_SCALE_PX(20)
#define ICON_GRID_TILE_BORDER BOARD_SCALE_PX(2)
#define ICON_GRID_CARD_GAP BOARD_SCALE_PX(16)

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

static void row_label_layout_identity(lv_obj_t * row, lv_obj_t * secondary, int32_t height);

int32_t ui_list_row_width(void) {
    return screen_builders_active_display_width() - 2 * GUI_LIST_SIDE_MARGIN;
}

int32_t ui_list_row_width_wide(void) {
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

/* The height row_label_apply_bounded_height() will set for this font, for
 * callers that must position a following element without reading the label's
 * height back (lv_obj_get_height() returns a stale value until the next
 * layout pass). */
int32_t row_label_bounded_height(const lv_font_t * font) {
    int32_t line_h = font ? lv_font_get_line_height(font) : 24;
    int32_t descender_margin = line_h / 8;
    if (descender_margin < 4) descender_margin = 4;
    return line_h + descender_margin * 2;
}

/* Applies a bounded height and descender margin to scrolling row labels,
 * preventing descender clipping and spurious vertical scroll animation.
 * Adds proportional top padding to maintain vertical centering within the row. */
void row_label_apply_bounded_height(lv_obj_t * label, const lv_font_t * font) {
    int32_t line_h = font ? lv_font_get_line_height(font) : 24;
    int32_t descender_margin = line_h / 8;
    if (descender_margin < 4) descender_margin = 4;
    lv_obj_set_height(label, row_label_bounded_height(font));
    lv_obj_set_style_pad_top(label, descender_margin, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_USER_3);
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
    /* Reserved for native headers, secondary labels and virtual lists. */
    if (lv_obj_get_user_data(obj) == &music_list_row_layout_marker)
        layout_music_list_row(obj);
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

    uint32_t child_count = lv_obj_get_child_count(obj);
    if (child_count >= 2 && lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) {
        lv_obj_t * img_wrap = lv_obj_get_child(obj, 0);
        lv_obj_t * label = lv_obj_get_child(obj, 1);
        if (lv_obj_check_type(label, &lv_label_class) &&
            lv_obj_get_child_count(img_wrap) == 1 &&
            lv_obj_check_type(lv_obj_get_child(img_wrap, 0), &lv_image_class)) {
            int32_t icon_h = lv_obj_get_height(img_wrap);
            bool label_inside_icon = lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_2);
            int32_t available_h = lv_obj_get_content_height(obj);
            if (label_inside_icon) {
                int32_t top = (available_h - icon_h) / 2;
                if (top < 0) top = 0;
                int32_t label_cy = top + (int32_t)(((int64_t)icon_h * 83) / 100);
                lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top);
                lv_obj_align(label, LV_ALIGN_TOP_MID, 0,
                             label_cy - lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) / 2);
            } else {
                int32_t line_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY));
#if defined(BOARD_TEMPOTEC_V1)
                if (lv_label_get_long_mode(label) == LV_LABEL_LONG_SCROLL_CIRCULAR) {
                    int32_t label_gap = BOARD_SCALE_PX(12);
                    int32_t content_h = icon_h + label_gap + line_h;
                    int32_t top = (available_h - content_h) / 2;
                    if (top < 0) top = 0;
                    lv_obj_set_height(label, line_h);
                    lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top);
                    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, top + icon_h + label_gap);
                } else {
#endif
                int32_t label_h = line_h * 2;
                int32_t content_h = icon_h + ICON_GRID_ICON_LABEL_GAP + label_h;
                int32_t top = (available_h - content_h) / 2;
                if (top < 0) top = 0;
                lv_obj_set_height(label, label_h);
                lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top);
                lv_obj_align(label, LV_ALIGN_TOP_MID, 0, top + icon_h + ICON_GRID_ICON_LABEL_GAP);
#if defined(BOARD_TEMPOTEC_V1)
                }
#endif
            }
        }
    }

    child_count = lv_obj_get_child_count(obj);
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

int32_t ui_list_row_height(void) {
    int32_t needed = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_ROW)) +
                     lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) + 32;
    return needed > GUI_ROW_HEIGHT ? needed : GUI_ROW_HEIGHT;
}

int32_t ui_music_row_height(void) {
    return ui_list_row_height();
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
    lv_obj_t * primary = lv_label_create(row);
    lv_obj_add_style(primary, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(primary, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_pos(primary, GUI_TEXT_INSET, 0);
    lv_obj_set_width(primary, LIST_ROW_WIDTH - GUI_TEXT_INSET -
                             (trailing_space > 0 ? trailing_space : GUI_TEXT_INSET));
    row_label_apply_bounded_height(primary, gui_theme_font(GUI_FONT_ROLE_ROW));
    row_label_enable_marquee(primary);
    if (newline) lv_label_set_text_fmt(primary, "%.*s", (int)(newline - title), title);
    else lv_label_set_text(primary, title ? title : "");

    lv_obj_t * secondary = lv_label_create(row);
    lv_obj_add_style(secondary, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(secondary, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_pos(secondary, GUI_TEXT_INSET, 0);
    lv_obj_set_width(secondary, LIST_ROW_WIDTH - GUI_TEXT_INSET -
                               (trailing_space > 0 ? trailing_space : GUI_TEXT_INSET));
    row_label_apply_bounded_height(secondary, gui_theme_font(GUI_FONT_ROLE_BODY));
    row_label_enable_marquee(secondary);
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

/* Generous upper bound on rows for a 2-column icon grid -- every real
 * screen using build_icon_grid_screen tops out at 6 items (3 rows). */
#define ICON_GRID_MAX_ROWS 6

/* Every full icon-grid screen today (Home, Music, Wireless) has exactly 3
 * rows, and that's the row size every tile has been visually tuned against.
 * Rows are sized against this reference instead of "however many rows this
 * particular screen has" so a screen with fewer items (Stream Media: 4
 * items, 2 rows) gets normal-sized tiles with genuine leftover space below,
 * rather than 2 rows stretched to fill the same height 3 would -- confirmed
 * on real hardware as the cause of Stream Media reading as mostly empty. */
#define ICON_GRID_REFERENCE_ROWS 3

/* On-screen icon footprint (longest edge, in px) every icon-grid tile
 * targets, regardless of its source PNG's native resolution -- icons in the
 * "category" folder (Music, Home) are 100x100 and were the ones the old
 * flat lv_image_set_scale(img, 340) constant was tuned against
 * (100*340/256 = ~133px), but the "stream_media" icons and
 * wireless/dlna.png are natively 212x190, over 2x bigger. Applying that
 * same flat scale to those rendered them at ~280x252px -- nearly a whole
 * tile's height on its own -- which is what was pushing Stream Media's
 * row-2 labels out of their tiles entirely (confirmed on real hardware).
 * Scaling per-tile against each source image's actual size keeps every
 * icon the same size on screen instead. */
#define ICON_GRID_TARGET_ICON_PX 133

/* Tile inner padding and icon-to-label gap, both in px -- pulled out as
 * named constants since the manual absolute-positioning math below (see
 * its own comment) needs the exact same numbers used by the tile's own
 * pad_all style. */
#define ICON_GRID_TILE_PAD BOARD_SCALE_PX(8)

typedef struct {
    lv_draw_buf_t * pixels;
    lv_obj_t * icon_wrapper;
} icon_glow_t;

static void icon_glow_event_cb(lv_event_t * e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    icon_glow_t * glow = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        lv_draw_buf_destroy(glow->pixels);
        free(glow);
    } else if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) {
        lv_area_t area;
        lv_obj_get_coords(glow->icon_wrapper, &area);
        int32_t diameter = glow->pixels->header.w;
        area.x1 = (area.x1 + area.x2 + 1 - diameter) / 2;
        area.y1 = (area.y1 + area.y2 + 1 - diameter) / 2;
        area.x2 = area.x1 + diameter - 1;
        area.y2 = area.y1 + diameter - 1;
        lv_draw_image_dsc_t dsc;
        lv_draw_image_dsc_init(&dsc);
        dsc.src = glow->pixels;
        lv_draw_image(lv_event_get_layer(e), &dsc, &area);
    }
}

void add_icon_glow(lv_obj_t * tile, lv_obj_t * wrapper, uint32_t color,
                    int32_t diameter) {
    if (!color || diameter < 4) return;
    icon_glow_t * glow = calloc(1, sizeof(*glow));
    if (!glow) return;
    glow->pixels = lv_draw_buf_create(diameter, diameter, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (!glow->pixels) { free(glow); return; }
    glow->icon_wrapper = wrapper;
    /* Compact support, rather than an opaque rectangular card: alpha and
     * its slope both reach zero before the bitmap's edge. Bake once, then
     * draw like an ordinary image during screen transitions. */
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    int32_t radius2 = (diameter - 2) * (diameter - 2);
    for (int32_t y = 0; y < diameter; ++y) {
        lv_color32_t * row = (lv_color32_t *)(glow->pixels->data + y * glow->pixels->header.stride);
        for (int32_t x = 0; x < diameter; ++x) {
            int32_t dx = 2 * x - diameter + 1, dy = 2 * y - diameter + 1;
            int32_t distance2 = dx * dx + dy * dy;
            int32_t alpha = 0;
            if (distance2 < radius2) {
                int32_t t = 255 - distance2 * 255 / radius2;
                alpha = ((92 * t * t / 255) * t) / (255 * 255);
                /* Fixed spatial noise reduces RGB565 banding; fade the
                 * noise out with the glow, so the perimeter stays clear. */
                alpha += ((int32_t)bayer[y & 3][x & 3] - 8) * t / (2 * 255);
                if (alpha < 0) alpha = 0;
            }
            row[x] = (lv_color32_t){ .red = color >> 16, .green = color >> 8,
                                    .blue = color, .alpha = alpha };
        }
    }
    lv_obj_add_event_cb(tile, icon_glow_event_cb, LV_EVENT_ALL, glow);
}

/* STATUS_BAR_CLEARANCE / TITLE_ROW_HEIGHT now live in screen_builders.h --
 * gui.c's hand-built screens (player, accent color, EQ, ...) need them too. */

static const char * breeze_asset_if_present(const char * relative);

/* Breeze art for title-bar actions (see titlebar/BREEZE_SOURCES.txt). A
 * theme that brings its own copy of one of these keeps it, and the stock icon
 * stays the fallback when the Breeze file is missing. The override root is
 * not consulted: on devices it mostly holds stock copies left by the legacy
 * Themes plugin. */
static const struct {
    const char * stock;
    const char * breeze;
} titlebar_breeze_icons[] = {
    { "navigation/back.png", "titlebar/breeze_back.png" },
    { "navigation/search.png", "titlebar/breeze_search.png" },
    { "navigation/more.png", "titlebar/breeze_menu.png" },
    { "settings/sound.png", "titlebar/breeze_sound.png" },
    { "settings/playback.png", "titlebar/breeze_playback.png" },
    { "submenu/music_database.png", "titlebar/breeze_database.png" },
    { "playlist_actions/trash.png", "titlebar/breeze_delete.png" },
    { "stream_media/download.png", "titlebar/breeze_download.png" },
    { "playing_plane/collect_in.png", "titlebar/breeze_favorite.png" },
    { "playing_plane/collect_out.png", "titlebar/breeze_favorite_off.png" },
};

const char * titlebar_breeze_icon(const char * asset) {
    if (!asset) return NULL;
    for (size_t i = 0; i < sizeof(titlebar_breeze_icons) / sizeof(titlebar_breeze_icons[0]); i++) {
        const char * stock = asset_path(titlebar_breeze_icons[i].stock);
        if (!stock || strcmp(asset, stock) != 0) continue;
        char layer[GUI_THEMES_MAX_PATH + 64];
        if (gui_themes_resolve_icon_layer(titlebar_breeze_icons[i].stock, layer, sizeof(layer)) &&
            !strstr(layer, asset_stock_root())) return NULL;
        const char * breeze = asset_path_plain(titlebar_breeze_icons[i].breeze);
        if (!breeze || access(breeze, R_OK) != 0) return NULL;
        return text_glyph_asset(titlebar_breeze_icons[i].breeze);
    }
    return NULL;
}

/* Breeze glyphs are pre-tinted in the text color (text_glyph_asset()); only
 * "Tint title bar icons" recolors them, to follow the live accent. */
void style_titlebar_breeze_icon(lv_obj_t * image) {
    lv_obj_add_style(image, gui_theme_titlebar_tint_style(), 0);
}

static const char * breeze_asset_if_present(const char * relative) {
    const char * plain = asset_path_plain(relative);
    return plain && access(plain, R_OK) == 0 ? asset_path(relative) : NULL;
}

/* White Breeze glyphs ship pre-tinted twice: <name>.png in Breeze's text
 * color for dark surfaces and <name>_dark.png for light ones. Picking the
 * file replaces a per-draw recolor. */
const char * text_glyph_asset(const char * relative) {
    lv_style_value_t text;
    bool dark_text = lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &text) ==
                         LV_STYLE_RES_FOUND && gui_theme_color_is_dark(text.color);
    if (dark_text) {
        char dark[128];
        const char * dot = strrchr(relative, '.');
        int n = dot ? snprintf(dark, sizeof(dark), "%.*s_dark%s", (int) (dot - relative), relative, dot) : -1;
        if (n > 0 && (size_t) n < sizeof(dark)) {
            const char * variant = breeze_asset_if_present(dark);
            if (variant) return variant;
        }
    }
    return asset_path(relative);
}

void set_reorder_handle_icon(lv_obj_t * grip) {
    if (breeze_asset_if_present("titlebar/breeze_handle.png")) {
        lv_image_set_src(grip, text_glyph_asset("titlebar/breeze_handle.png"));
        return;
    }
    lv_image_set_src(grip, asset_path("navigation/reorder.png"));
    lv_obj_add_style(grip, &style_theme_text_primary, 0);
}

static lv_obj_t * build_header_icon_button(lv_obj_t * scr, const char * asset,
                                          lv_align_t alignment, lv_event_cb_t cb) {
    /* Hitbox is deliberately larger than the visual icon -- real-hardware
     * testing showed taps aimed at this corner landing a few pixels outside
     * a tight 44x44 box, so the touch area is padded out generously while
     * the icon itself stays centered at its normal size. */
    lv_obj_t * btn = lv_obj_create(scr);
    lv_obj_set_size(btn, HEADER_BUTTON_WIDTH, HEADER_BUTTON_WIDTH);
    lv_obj_align(btn, alignment, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - HEADER_BUTTON_WIDTH) / 2);
    lv_obj_set_style_bg_opa(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(btn, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, V1_SCALE_PX(12), 0);

    if (asset) {
        const char * breeze = titlebar_breeze_icon(asset);
        lv_obj_t * arrow = lv_image_create(btn);
        lv_image_set_src(arrow, breeze ? breeze : asset);
#if defined(BOARD_TEMPOTEC_V1)
        lv_obj_set_size(arrow, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_image_set_inner_align(arrow, LV_IMAGE_ALIGN_CONTAIN);
#endif
        lv_obj_align(arrow, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
        if (breeze) style_titlebar_breeze_icon(arrow);
    }

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

lv_obj_t * build_header_back_button(lv_obj_t * scr, lv_event_cb_t cb) {
    const char * asset = asset_path("navigation/back.png");
    lv_obj_t * button = build_header_icon_button(scr, asset, LV_ALIGN_TOP_LEFT, cb);
    if (!button) return NULL;
    lv_obj_t * image = lv_obj_get_child(button, 0);
    if (image) {
        lv_obj_set_size(image, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
        if (titlebar_breeze_icon(asset)) style_titlebar_breeze_icon(image);
        else lv_obj_add_style(image, &style_theme_text_primary, 0);
    }
    return button;
}

lv_obj_t * build_navigation_chevron(lv_obj_t * parent, int32_t right_inset,
                                    bool has_color_override, uint32_t color_override) {
    if (!parent) return NULL;

    lv_obj_t * chevron = lv_image_create(parent);
    if (!chevron) return NULL;
    lv_obj_set_size(chevron, BOARD_SCALE_PX(24), BOARD_SCALE_PX(24));
    lv_image_set_src(chevron, asset_path("navigation/chevron-right.png"));
    lv_image_set_inner_align(chevron, LV_IMAGE_ALIGN_CONTAIN);

    if (has_color_override) {
        lv_obj_set_style_image_recolor(chevron, lv_color_hex(color_override), 0);
        lv_obj_set_style_image_recolor_opa(chevron, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_style(chevron, &style_theme_text_muted, 0);
    }
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -right_inset, 0);
    lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
    return chevron;
}

void align_screen_header_action(lv_obj_t * action, int32_t right_inset) {
    lv_obj_align(action, LV_ALIGN_TOP_RIGHT, -right_inset, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT / 2);
    lv_obj_set_style_translate_y(action, lv_pct(-50), 0);
#if defined(BOARD_TEMPOTEC_V1)
    /* Refresh and other late-added actions must reserve their real header
     * footprint just like actions passed to build_screen_header(). */
    lv_obj_t * parent = lv_obj_get_parent(action);
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t * child = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            lv_obj_has_flag(child, LV_OBJ_FLAG_USER_4) && !lv_obj_get_user_data(child))
            reserve_title_width_before(child, action);
    }
#endif
}

/* Same glyph, accent color and font as the Remote Control PIN refresh icon,
 * placed like the Wi-Fi/Bluetooth Rescan action. The glyph is small, so its
 * touch area is widened. */
static void header_refresh_rotation_cb(void * var, int32_t angle) {
    /* Images rotate in their own draw; the fallback glyph needs a transform. */
    if (lv_obj_check_type((lv_obj_t *) var, &lv_image_class)) lv_image_set_rotation((lv_obj_t *) var, angle);
    else lv_obj_set_style_transform_rotation((lv_obj_t *) var, angle, 0);
}

lv_obj_t * build_header_refresh_action(lv_obj_t * scr, lv_event_cb_t click_cb) {
    /* Match the shared header controls' large touch target; keep the glyph
     * centered so this remains easy to hit on a small display. */
    lv_obj_t * button = build_header_icon_button(scr, NULL, LV_ALIGN_TOP_RIGHT, click_cb);
    align_screen_header_action(button, V1_SCALE_PX(20));

    const char * breeze = asset_path_plain("titlebar/breeze_refresh.png");
    if (breeze && access(breeze, R_OK) == 0) {
        lv_obj_t * image = lv_image_create(button);
        lv_image_set_src(image, text_glyph_asset("titlebar/breeze_refresh.png"));
#if defined(BOARD_TEMPOTEC_V1)
        lv_obj_set_size(image, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
#endif
        lv_obj_align(image, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
        style_titlebar_breeze_icon(image);
        set_header_refresh_action_busy(button, false);
        return button;
    }

    lv_obj_t * icon = lv_label_create(button);
    lv_label_set_text(icon, LV_SYMBOL_REFRESH);
    lv_obj_add_style(icon, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(icon, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_transform_pivot_x(icon, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(icon, lv_pct(50), 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    set_header_refresh_action_busy(button, false);
    return button;
}

/* Spin and disable the refresh action while its background refresh runs. */
void set_header_refresh_action_busy(lv_obj_t * button, bool busy) {
    if (!button) return;
    lv_obj_t * icon = lv_obj_get_child(button, 0);
    if (!icon) return;
    lv_anim_delete(icon, header_refresh_rotation_cb);
    if (busy) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, icon);
        lv_anim_set_values(&anim, 0, 3600);
        lv_anim_set_duration(&anim, 900);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&anim, header_refresh_rotation_cb);
        lv_anim_start(&anim);
    } else {
        header_refresh_rotation_cb(icon, 0);
    }
    lv_obj_set_style_opa(button, busy ? LV_OPA_60 : LV_OPA_COVER, 0);
    if (busy) lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t * build_top_right_icon_button(lv_obj_t * scr, const char * icon_asset, lv_event_cb_t click_cb) {
    lv_obj_t * btn = build_header_icon_button(scr, icon_asset, LV_ALIGN_TOP_RIGHT, click_cb);
    /* Some screens add their action after the shared header was built. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t * child = lv_obj_get_child(scr, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            lv_obj_has_flag(child, LV_OBJ_FLAG_USER_4) && !lv_obj_get_user_data(child)) {
            lv_obj_set_width(child, screen_builders_active_display_width() - V1_SCALE_PX(76) - HEADER_BUTTON_WIDTH - V1_SCALE_PX(12));
            screen_title_fit(child);
        }
    }
    return btn;
}

/* 76 (64px back button + 12px breathing room) / 20 -- same values as
 * gui.c's own TITLE_LABEL_LEFT_INSET/TITLE_LABEL_DEFAULT_RIGHT_MARGIN
 * (private to that file, so duplicated here rather than shared through the
 * header for one constant pair) used by every other screen's own
 * left-aligned, marquee-capable title (build_group_songs_screen(),
 * build_subsonic_list_screen(), build_files_screen(), ...). Kept in sync by
 * hand -- if either changes, check the other. */
#define BUILD_TITLE_LEFT_INSET V1_SCALE_PX(76)
#define BUILD_TITLE_RIGHT_MARGIN V1_SCALE_PX(20)


/* Narrows an already left-aligned, auto-scrolling title label (see
 * build_subsonic_list_screen()'s and build_group_songs_screen()'s own
 * construction) so its right edge stops before `right_icon`'s own left
 * edge, instead of running underneath it -- call once, right after
 * right_icon's own final on-screen position is set (real coordinates, not
 * a pending layout -- lv_label/lv_image both size/position synchronously
 * on lv_obj_align(), no intervening refresh pass needed). Safe to call
 * even while right_icon is currently hidden (e.g. a Download button only
 * shown for some views of a reused screen) -- a hidden object still has a
 * real position, and reserving room for it whether or not it's showing
 * right now is simpler and safer than tracking two different widths.
 *
 * Uses lv_obj_get_coords() after lv_obj_update_layout() on the common
 * parent so absolute coordinates are resolved before calculating the
 * available title width. */
void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon) {
    lv_obj_update_layout(lv_obj_get_parent(title));

    lv_area_t title_area, icon_area;
    lv_obj_get_coords(title, &title_area);
    lv_obj_get_coords(right_icon, &icon_area);
    int32_t width = icon_area.x1 - title_area.x1 - V1_SCALE_PX(12);
    if (width < V1_SCALE_PX(40)) width = V1_SCALE_PX(40); /* never collapse to nothing/negative on a pathological layout */
    lv_obj_set_width(title, width);
    screen_title_fit(title);
}


/* Keeps a header title on its one line, vertically centered within the
 * TITLE_ROW_HEIGHT band below the status bar (matching the back button's
 * vertical center). A title wider than the label (translations often are) drops
 * to the row font; if even that is too wide it ends in an ellipsis. The
 * fixed one-line height is what stops LV_LABEL_LONG_DOT from wrapping onto
 * the first row, including for titles whose text changes later. Rerun after
 * the label's width or the font tier changes. */
void screen_title_fit(lv_obj_t * label) {
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_TITLE);
    int32_t width = lv_obj_get_style_width(label, LV_PART_MAIN);
    const char * text = lv_label_get_text(label);
    if (text && LV_COORD_IS_PX(width) && width > 0) {
        lv_point_t size;
        lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > width) font = gui_theme_font(GUI_FONT_ROLE_ROW);
    }
    int32_t line_h = lv_font_get_line_height(font);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_height(label, line_h);
    lv_obj_set_y(label, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - line_h) / 2);
}

static lv_obj_t * build_title(lv_obj_t * scr, const char * title) {
    lv_obj_t * label = lv_label_create(scr);
    lv_obj_add_flag(label, LV_OBJ_FLAG_USER_4);
    lv_label_set_text(label, title);
    /* Keep long titles within the header and show an ellipsis. Marquee
     * animation can begin mid-string after screen transitions, so headers
     * remain still and readable until the user opens the corresponding list. */
    lv_obj_set_width(label, lv_display_get_horizontal_resolution(lv_display_get_default()) -
                                 BUILD_TITLE_LEFT_INSET - BUILD_TITLE_RIGHT_MARGIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, BUILD_TITLE_LEFT_INSET, 0);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    screen_title_fit(label);
    return label;
}

/* ---- Icon grid ---- */
lv_obj_t * build_screen_header(lv_obj_t * scr, const char * title, lv_event_cb_t back_cb,
                              const char * trailing_asset, lv_event_cb_t trailing_cb) {
    if (back_cb) build_header_back_button(scr, back_cb);
    lv_obj_t * label = title ? build_title(scr, title) : NULL;
    if (trailing_asset) {
        build_top_right_icon_button(scr, trailing_asset, trailing_cb);
        if (label) {
            lv_obj_set_width(label, screen_builders_active_display_width() - BUILD_TITLE_LEFT_INSET - HEADER_BUTTON_WIDTH - V1_SCALE_PX(12));
            screen_title_fit(label);
        }
    }
    return label;
}

typedef struct {
    lv_obj_t * img;
    const char * normal_path;
    const char * selected_path;
} icon_tile_press_ctx_t;

typedef struct {
    int32_t cols[3];
    int32_t rows[ICON_GRID_MAX_ROWS + 1];
} icon_grid_dsc_ctx_t;

static void heap_ctx_delete_cb(lv_event_t * e) {
    free(lv_event_get_user_data(e));
}

static void lv_heap_ctx_delete_cb(lv_event_t * e) {
    lv_free(lv_event_get_user_data(e));
}

static void icon_tile_press_event_cb(lv_event_t * e) {
    icon_tile_press_ctx_t * ctx = (icon_tile_press_ctx_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_image_set_src(ctx->img, asset_path(ctx->selected_path));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_image_set_src(ctx->img, asset_path(ctx->normal_path));
    }
}

lv_obj_t * build_icon_grid_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const icon_grid_item_t * items, int item_count,
                                   int32_t icon_scale_percent, bool label_inside_icon, int32_t tile_gap) {
    /* Clamped here, not just left to whatever the caller passed -- every
     * native call site already passes the literal 0, but
     * plugin.set_home_layout()'s tile_gap (PLUGINS.md) is plugin-controlled
     * and otherwise unbounded all the way from the Lua boundary. Unlike
     * build_pill_list_screen()'s row_gap (plain pad_gap spacing), this one
     * is subtracted straight into `available_h` below (icon/label vertical
     * budget) -- a large enough value drives that negative and produces
     * broken tile content positioning, not just a wide gap, so it needs a
     * real ceiling rather than a cosmetic one. */
    if (tile_gap < 0) tile_gap = 0;
    if (tile_gap > BOARD_SCALE_PX(64)) tile_gap = BOARD_SCALE_PX(64);
    if (tile_gap < ICON_GRID_CARD_GAP) tile_gap = ICON_GRID_CARD_GAP;

    int32_t target_icon_px = (BOARD_SCALE_PX(ICON_GRID_TARGET_ICON_PX) * icon_scale_percent) / 100;
#if defined(BOARD_TEMPOTEC_V1)
    bool compact_home_grid = !title && !back_btn_cb;
#endif

    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* NULL back_btn_cb omits the back button (for Home launcher).
     * NULL title omits the header title, leaving a small gap below the status bar. */
    build_screen_header(scr, title, back_btn_cb, NULL, NULL);

    /* 2-column grid with equal-sized cells and divider lines between them. */
    int col_count = 2;
    int row_count = (item_count + col_count - 1) / col_count;

    /* Allocated per-grid because lv_obj_set_grid_dsc_array() stores the pointer.
     * The owning grid frees this context on LV_EVENT_DELETE so soft reloads do not leak. */
    icon_grid_dsc_ctx_t * grid_dsc = lv_malloc(sizeof(*grid_dsc));
    if (!grid_dsc) {
        fprintf(stderr, "[screen_builders] out of memory building icon grid '%s'\n",
                title ? title : "Home");
        return scr; /* valid header-only screen is safer than a NULL screen */
    }
    int32_t * col_dsc = grid_dsc->cols;
    col_dsc[0] = LV_GRID_FR(1);
    col_dsc[1] = LV_GRID_FR(1);
    col_dsc[2] = LV_GRID_TEMPLATE_LAST;

    /* Grid height is screen height minus active header bands (status bar clearance
     * and optional title row) to keep cell sizing consistent across all icon grids. */
    int32_t scr_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t header_h = STATUS_BAR_CLEARANCE + (title ? TITLE_ROW_HEIGHT : 0);

    /* Rows use fixed pixel height based on ICON_GRID_REFERENCE_ROWS rather than
     * fractional shares, so cell heights remain consistent regardless of row count. */
    int32_t row_h = (scr_h - header_h) / ICON_GRID_REFERENCE_ROWS;

    int32_t * row_dsc = grid_dsc->rows;
    for (int r = 0; r < row_count && r < ICON_GRID_MAX_ROWS; r++) row_dsc[r] = row_h;
    row_dsc[row_count < ICON_GRID_MAX_ROWS ? row_count : ICON_GRID_MAX_ROWS] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t * grid = lv_obj_create(scr);
    lv_obj_add_event_cb(grid, lv_heap_ctx_delete_cb, LV_EVENT_DELETE, grid_dsc);
    lv_obj_set_size(grid, lv_pct(100), row_count * row_h);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, header_h);
    lv_obj_set_style_bg_opa(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    /* Half a gap at the screen edges, completing the other half each tile's
     * margin adds, so outer and inner spacing match. */
    lv_obj_set_style_pad_hor(grid, tile_gap / 2, 0);
    /* Zero row/column pad gaps so the grid height precisely matches row_count * row_h. */
    lv_obj_set_style_pad_row(grid, 0, 0);
    lv_obj_set_style_pad_column(grid, 0, 0);
    /* Vertical-only scroll direction so horizontal swipes pass through to gesture handling. */
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    for (int i = 0; i < item_count; i++) {
        const icon_grid_item_t * item = &items[i];
        int col = i % col_count;
        int row = i / col_count;

        lv_obj_t * tile = lv_obj_create(grid);
        /* USER_1/USER_2 are zero-allocation markers used by the one-shot
         * font geometry refresh; unlike a per-tile context they add no
         * steady-state heap or callback overhead. */
        lv_obj_add_flag(tile, LV_OBJ_FLAG_USER_1);
        if (label_inside_icon) lv_obj_add_flag(tile, LV_OBJ_FLAG_USER_2);
        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
        /* Margins visually inset the tile within its stretched grid cell,
         * with tile_gap / 2 per side so adjacent tiles have a total gap of tile_gap. */
        if (tile_gap > 0) lv_obj_set_style_margin_all(tile, tile_gap / 2, 0);
        lv_obj_set_style_bg_opa(tile, 0, 0);
        /* Native tiles are theme cards; a tile a theme paints itself keeps
         * its own color or image. */
        bool card = !item->has_bg_color && !item->bg_image;
        /* Per-tile style overrides (plugin.set_home_layout()'s per-tile
         * config) -- every native tile leaves these unset, so this changes
         * nothing about today's transparent-background tiles. */
        if (item->has_bg_color) {
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(item->bg_color), 0);
        }
        if (item->has_radius) lv_obj_set_style_radius(tile, item->radius, 0);
        /* Per-tile background image (rounded card shape + accent glow are
         * already baked into the asset's own alpha/pixels) -- drawn via
         * bg_image_src rather than bg_color, so it composites independently
         * of the has_bg_color branch above. */
        if (item->bg_image) {
            lv_obj_set_style_bg_image_src(tile, asset_path(item->bg_image), 0);
            lv_obj_set_style_bg_image_opa(tile, LV_OPA_COVER, 0);
        }
        /* Zero default card border so only grid-level divider lines are drawn. */
        lv_obj_set_style_border_width(tile, 0, 0);
        if (card) {
            lv_obj_remove_local_style_prop(tile, LV_STYLE_BG_OPA, 0);
            lv_obj_remove_local_style_prop(tile, LV_STYLE_BORDER_WIDTH, 0);
            lv_obj_add_style(tile, &style_theme_tile, 0);
            lv_obj_add_style(tile, &style_theme_tile_pressed, LV_STATE_PRESSED);
        } else {
            /* Apply a 25% press tint, preserving opaque custom tile backgrounds. */
            const lv_opa_t press_opa = 64; /* 25% of 255 */
            lv_color_t press_color = lv_color_hex(GUI_COLOR_PRESSED);
            if (item->has_bg_color)
                press_color = lv_color_mix(press_color, lv_color_hex(item->bg_color), press_opa);
            lv_obj_set_style_bg_opa(tile, item->has_bg_color ? LV_OPA_COVER : press_opa, LV_STATE_PRESSED);
            lv_obj_set_style_bg_color(tile, press_color, LV_STATE_PRESSED);
        }
        lv_obj_set_style_pad_all(tile, ICON_GRID_TILE_PAD, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

        /* Icon and label are positioned with explicit pixel offsets rather
         * than flex auto-centering for stable rendering across layout passes. */
        lv_obj_t * img_wrap = lv_obj_create(tile);
        lv_obj_remove_style_all(img_wrap);
        lv_obj_remove_flag(img_wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(img_wrap, LV_OBJ_FLAG_CLICKABLE); /* let taps fall through to tile's own click handler */

        lv_obj_t * img = lv_image_create(img_wrap);
        const char * icon_full_path = asset_path(item->icon_asset);
        lv_image_set_src(img, icon_full_path);

        lv_image_header_t icon_header;
        int32_t icon_scale = (int32_t) (((int64_t) target_icon_px * 256) / 100); /* fallback: category icons are natively 100px */
        int32_t icon_w = 100, icon_h = 100; /* fallback: matches the "category" folder icons' native size */
        if (lv_image_decoder_get_info(icon_full_path, &icon_header) == LV_RESULT_OK) {
            int32_t native_max = icon_header.w > icon_header.h ? icon_header.w : icon_header.h;
            if (native_max > 0) icon_scale = (int32_t) (((int64_t) target_icon_px * 256) / native_max);
            icon_w = icon_header.w;
            icon_h = icon_header.h;
        }
        lv_image_set_scale(img, icon_scale);
        int32_t target_icon_w = (int32_t) (((int64_t) icon_w * icon_scale) / 256);
        int32_t target_icon_h = (int32_t) (((int64_t) icon_h * icon_scale) / 256);
        lv_obj_set_size(img_wrap, target_icon_w, target_icon_h);
        lv_obj_center(img);

        lv_obj_t * label = lv_label_create(tile);
        lv_label_set_text(label, item->label);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        if (item->has_text_color) lv_obj_set_style_text_color(label, lv_color_hex(item->text_color), 0);
        /* Wrap label text within the tile width to prevent overflow into adjacent tiles. */
        lv_obj_set_width(label, lv_pct(100));
#if defined(BOARD_TEMPOTEC_V1)
        if (compact_home_grid) lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        else
#endif
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

        /* tile_gap shrinks the tile's own STRETCH-computed height by that
         * much (its margin_top+margin_bottom) below row_h -- see the
         * tile_gap margin comment above -- so it has to come off here too,
         * or icon/label placement would overflow the tile's real (now
         * smaller) bottom edge. */
        int32_t available_h = row_h - tile_gap - 2 * ICON_GRID_TILE_PAD;
        if (label_inside_icon) {
            /* See this function's own header comment (screen_builders.h) --
             * these assets already reserve their own caption band near the
             * bottom of the image, so there's no separate label row to
             * budget space for: the icon alone gets centered in the full
             * available height, bigger than the below-icon layout would
             * allow. */
            int32_t top_offset = (available_h - target_icon_h) / 2;
            if (top_offset < 0) top_offset = 0;
            lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top_offset);
            /* ~83% down the icon's own scaled height -- matches where the
             * reserved band actually starts, per a pixel scan of every
             * wireless PNG asset (glyph content ends around 60-68% down
             * their shared 190px-tall native canvas; centering the label a
             * bit past that, within the remaining flat-background band,
             * comfortably clears every icon's glyph regardless of its
             * exact bottom edge). */
            int32_t label_cy = top_offset + (int32_t) (((int64_t) target_icon_h * 83) / 100);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, label_cy - lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) / 2);
        } else {
            /* Real bug caught in review: the label above is LV_LABEL_LONG_
             * WRAP (genuinely multi-line capable, see its own comment), but
             * this layout budgeted only a single line_h -- a caption long
             * enough to actually wrap (more likely at a bigger font tier,
             * or just a longer label like "Remote Control" at a narrow
             * tile width) extended past its own reserved band and could
             * overlap the next tile below it. Bounding the label's own
             * height at exactly 2 lines (rather than leaving it auto-size
             * to however much text it holds) means a 3rd line clips
             * instead of pushing the overlap even further -- two lines
             * comfortably covers every real label this screen shows. */
            int32_t line_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY));
#if defined(BOARD_TEMPOTEC_V1)
            int32_t label_h = compact_home_grid ? line_h : line_h * 2;
            int32_t label_gap = compact_home_grid ? BOARD_SCALE_PX(12) : ICON_GRID_ICON_LABEL_GAP;
#else
            int32_t label_h = line_h * 2;
            int32_t label_gap = ICON_GRID_ICON_LABEL_GAP;
#endif
            lv_obj_set_height(label, label_h);
            int32_t content_h = target_icon_h + label_gap + label_h;
            int32_t top_offset = (available_h - content_h) / 2;
            if (top_offset < 0) top_offset = 0;
            lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top_offset);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, top_offset + target_icon_h + label_gap);
        }

        /* A card already sets its icon apart; the glow only muddies it. */
        if (item->icon_glow_color && !item->has_bg_color && !card) {
            int32_t diameter = target_icon_px * 4 / 3;
            int32_t max_diameter = screen_builders_active_display_width() / col_count - tile_gap - 2 * ICON_GRID_TILE_PAD;
            if (diameter > max_diameter) diameter = max_diameter;
            if (diameter > available_h) diameter = available_h;
            add_icon_glow(tile, img_wrap, item->icon_glow_color, diameter);
        }

        if (item->icon_asset_selected) {
            icon_tile_press_ctx_t * ctx = malloc(sizeof(icon_tile_press_ctx_t));
            if (ctx) {
                ctx->img = img;
                ctx->normal_path = item->icon_asset;
                ctx->selected_path = item->icon_asset_selected;
                lv_obj_add_event_cb(tile, icon_tile_press_event_cb, LV_EVENT_PRESSED, ctx);
                lv_obj_add_event_cb(tile, icon_tile_press_event_cb, LV_EVENT_RELEASED, ctx);
                lv_obj_add_event_cb(tile, icon_tile_press_event_cb, LV_EVENT_PRESS_LOST, ctx);
                lv_obj_add_event_cb(tile, heap_ctx_delete_cb, LV_EVENT_DELETE, ctx);
            }
        }

        if (item->on_click) lv_obj_add_event_cb(tile, item->on_click, LV_EVENT_CLICKED, item->user_data);
    }

    return scr;
}

/* ---- Pill list ---- */

typedef struct {
    lv_obj_t * toggle_sw; /* real lv_switch, not an image -- see pill_toggle_row_event_cb() */
} pill_toggle_ctx_t;

/* ctx is malloc()'d once per toggle row (below) and otherwise never freed --
 * every pill-list screen with a toggle row leaked one per row per rebuild
 * until this was added. Registered as a second, separate callback on the
 * same row/ctx pair, same free-on-LV_EVENT_DELETE convention
 * compact_list_delete_event_cb() above already uses for its own heap
 * user_data. */
static void pill_toggle_ctx_delete_cb(lv_event_t * e) {
    free(lv_event_get_user_data(e));
}

static void pill_toggle_row_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    pill_toggle_ctx_t * ctx = (pill_toggle_ctx_t *) lv_event_get_user_data(e);
    lv_obj_t * sw = ctx->toggle_sw;

    bool now_checked = !lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (now_checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(sw, LV_STATE_CHECKED);
    /* Real lv_switch now -- CHECKED state alone drives its own visual
     * (LV_PART_INDICATOR|LV_STATE_CHECKED style, default-theme white knob),
     * no manual sprite swap needed. VALUE_CHANGED still fires so callers
     * keep reading state the same way they already do for every other
     * lv_switch (lv_obj_has_state(target, LV_STATE_CHECKED)). */
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

void pill_row_apply_icon(lv_obj_t * row, lv_obj_t * label, const char * icon_path, int32_t icon_px,
                          lv_align_t align, int32_t x, int32_t y) {
    if (!icon_path) return;

    /* Relative icon paths are resolved against <sd_root>/.plugins/ (the plugin directory),
     * while absolute paths starting with '/' are preserved verbatim.
     * A path that does not fit is skipped; a truncated path could open a different file. */
    char resolved[600];
    int resolved_len;
    if (icon_path[0] == '/') {
        resolved_len = snprintf(resolved, sizeof(resolved), "%s", icon_path);
    } else {
        resolved_len = snprintf(resolved, sizeof(resolved), "%s/.plugins/%s", MUSIC_ROOT_DIR, icon_path);
    }
    if (resolved_len < 0 || (size_t)resolved_len >= sizeof(resolved)) return;

    /* "S:" is LVGL's POSIX-filesystem driver letter (lv_conf.h). Opens
     * resolved as an absolute filesystem path. Two extra bytes hold that prefix. */
    char src[sizeof(resolved) + 2];
    int src_len = snprintf(src, sizeof(src), "S:%s", resolved);
    if (src_len < 0 || (size_t)src_len >= sizeof(src)) return;

    /* lv_image_set_scale() only scales rendering without altering widget bounds.
     * Wrap in an explicitly-sized container matching target dimensions so layout
     * and label-offset calculations receive the actual on-screen width. */
    lv_obj_t * img_wrap = lv_obj_create(row);
    lv_obj_remove_style_all(img_wrap);
    lv_obj_remove_flag(img_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(img_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * img = lv_image_create(img_wrap);
    lv_image_set_src(img, src);

    /* Same scale-to-target-px-on-screen formula as build_icon_grid_screen()
     * above -- read the source's native size, compute the zoom that lands
     * it at icon_px regardless of the source file's own resolution. 256 =
     * LV_SCALE_NONE (100%), used as a safe fallback if the header can't be
     * read (a missing/corrupt file just renders at native size rather than
     * erroring -- a plugin icon is cosmetic, not worth failing the whole
     * row over). */
    lv_image_header_t header;
    int32_t scale = 256;
    int32_t native_w = icon_px, native_h = icon_px;
    if (lv_image_decoder_get_info(src, &header) == LV_RESULT_OK) {
        int32_t native_max = header.w > header.h ? header.w : header.h;
        if (native_max > 0) scale = (int32_t) (((int64_t) icon_px * 256) / native_max);
        native_w = header.w;
        native_h = header.h;
    }
    lv_image_set_scale(img, scale);
    int32_t target_w = (int32_t) (((int64_t) native_w * scale) / 256);
    int32_t target_h = (int32_t) (((int64_t) native_h * scale) / 256);
    lv_obj_set_size(img_wrap, target_w, target_h);
    lv_obj_center(img);
    lv_obj_align(img_wrap, align, x, y);

    /* Re-align, not re-inset -- label may already be non-default-aligned
     * (e.g. a caller that already moved it for its own reasons); this always
     * wins as the final word on the label's position, matching every other
     * call site's own single lv_obj_align() call. icon_px (not target_w) is
     * the reserved gap -- icon_px is always the longer edge, so this never
     * underestimates the icon's real width even for a non-square source. */
    lv_obj_align(label, align, x + icon_px + 12, y);
}

const lv_font_t * pill_row_resolve_text_size(const char * text_size) {
    if (!text_size) return gui_theme_font(GUI_FONT_ROLE_BODY);
    if (strcmp(text_size, "small") == 0) return &app_font_16;
    if (strcmp(text_size, "medium") == 0) return &app_font_22;
    if (strcmp(text_size, "large") == 0) return &app_font_28;
    /* 8x16 monospace bitmap font (lv_conf.h's LV_FONT_UNSCII_16) -- ASCII-only,
     * see that flag's own comment. Plugin rows only (plugin.set_home_layout()'s
     * text_size = "mono", PLUGINS.md). */
    if (strcmp(text_size, "mono") == 0) return &lv_font_unscii_16;
    return gui_theme_font(GUI_FONT_ROLE_BODY);
}

int32_t pill_row_default_width(void) {
    return ui_list_row_width();
}

lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const pill_list_item_t * items, int item_count,
                                   lv_style_t * toggle_accent_style, int32_t row_gap,
                                   int32_t icon_scale_pct) {
    /* Same scale-to-target-px formula as build_icon_grid_screen()'s own
     * target_icon_px -- 100 (every native call site) reproduces
     * PILL_ROW_ICON_PX_DEFAULT exactly, unchanged from before this
     * parameter existed. */
    int32_t icon_px = (BOARD_SCALE_PX(PILL_ROW_ICON_PX_DEFAULT) * icon_scale_pct) / 100;

    /* Clamped here, not just left to whatever the caller passed -- every
     * native call site already passes a small literal (6), but
     * plugin.set_home_layout()'s row_gap (PLUGINS.md) is plugin-controlled
     * and otherwise unbounded all the way from the Lua boundary. A
     * pathological value here is only a layout-quality issue (pad_gap, not
     * subtracted from any available-space math the way tile_gap is below),
     * but there's no legitimate reason to want more than a native row's own
     * height in gap, so bound it there. */
    if (row_gap < 0) row_gap = 0;
    if (row_gap > LIST_ROW_HEIGHT) row_gap = LIST_ROW_HEIGHT;

    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* NULL back_btn_cb means "this screen has nothing to go back to" (the
     * Home launcher) -- skip the arrow entirely rather than drawing a dead
     * button. NULL title similarly means "no header row at all" (Home's own
     * list-mode path, plugin.set_home_layout()) -- build_title() unconditionally
     * calling lv_label_set_text(label, NULL) on a freshly-created label (whose
     * own ->text starts NULL) reaches lv_strlen(NULL) and crashes, so this
     * must be skipped, not just cosmetic; mirrors build_icon_grid_screen()'s
     * own `if (title) build_title(...)` above. */
    build_screen_header(scr, title, back_btn_cb, NULL, NULL);

    /* Only reserve the title-row band when something is actually drawn into
     * it (a title label or a back button) -- Home's own list-mode call
     * passes neither, same "no header" case build_icon_grid_screen() already
     * gives the Home tile grid today. */
    int32_t header_h = STATUS_BAR_CLEARANCE + ((title || back_btn_cb) ? TITLE_ROW_HEIGHT : 0);
    int32_t display_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t bottom_inset = (!title && !back_btn_cb) ? 0 : HOME_INDICATOR_CONTENT_INSET;

    lv_obj_t * list = lv_obj_create(scr);
    lv_obj_set_size(list, lv_pct(100), display_h - header_h - bottom_inset);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -bottom_inset);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    /* Zero container padding so rows center across the full display width. */
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    screen_builders_configure_content_scrollbar(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    /* track_cross_place (3rd arg) set to CENTER centers the column track within
     * the container, ensuring narrower rows are properly centered rather than left-aligned. */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(list, row_gap, 0);
    lv_obj_set_style_pad_top(list, GUI_ROW_GAP, 0);
    /* The system home-indicator strip sits above the bottom of every
     * screen. Keep a small scrollable tail so the last row can be moved
     * fully clear of that overlay. */
    lv_obj_set_style_pad_bottom(list, BOARD_SCALE_PX(8), 0);

    int32_t content_h = item_count > 0 ? (item_count - 1) * row_gap : 0;
    for (int i = 0; i < item_count; i++) {
        const pill_list_item_t * item = &items[i];

        /* Native density; explicit plugin dimensions remain authoritative. */
        int32_t height = ui_list_row_height();
        int32_t font_min = lv_font_get_line_height(pill_row_resolve_text_size(item->text_size)) + 32;
        if (height < font_min) height = font_min;
        int32_t width = pill_row_default_width();
        if (item->row_height > 0) {
            height = item->row_height;
            if (height < PILL_ROW_HEIGHT_MIN) height = PILL_ROW_HEIGHT_MIN;
            if (height > PILL_ROW_HEIGHT_MAX) height = PILL_ROW_HEIGHT_MAX;
        }
        if (item->row_width > 0) {
            width = item->row_width;
            if (width < PILL_ROW_WIDTH_MIN) width = PILL_ROW_WIDTH_MIN;
            if (width > PILL_ROW_WIDTH_MAX) width = PILL_ROW_WIDTH_MAX;
        }
        content_h += height;

        lv_obj_t * row = lv_obj_create(list);
        if (item->out_row) *item->out_row = row;
        lv_obj_set_size(row, width, height);
        if (item->row_height <= 0) lv_obj_add_style(row, &native_row_min_style, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_style(row, &pill_row_bg_style, 0);
        if (item->has_radius) lv_obj_set_style_radius(row, item->radius, 0);
        if (item->has_bg_color) lv_obj_set_style_bg_color(row, lv_color_hex(item->bg_color), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, item->label);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        /* Bumped from montserrat_16 -- see the matching comment on the icon
         * grid's own label above. text_size NULL (every native row) ->
         * gui_theme_font(GUI_FONT_ROLE_BODY), unchanged from before this field existed -- see
         * pill_row_resolve_text_size()'s own comment. */
        lv_obj_set_style_text_font(label, pill_row_resolve_text_size(item->text_size), 0);
        if (item->has_text_color) lv_obj_set_style_text_color(label, lv_color_hex(item->text_color), 0);
        int32_t label_inset = BOARD_SCALE_PX(24);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, label_inset, 0);
        pill_row_apply_icon(row, label, item->icon_asset, icon_px, LV_ALIGN_LEFT_MID, label_inset, 0);

        /* Keep long labels inside their own row instead of letting the
         * label's content-sized box extend over the accessory or following
         * row. LV_LABEL_LONG_SCROLL_CIRCULAR is inert when the text fits and
         * becomes a single-line marquee only when it does not. */
        int32_t label_left = label_inset + (item->icon_asset ? icon_px + 12 : 0);
        int32_t accessory_space = item->accessory == PILL_ACCESSORY_TOGGLE ? BOARD_SCALE_PX(112)
                                  : item->accessory == PILL_ACCESSORY_CHEVRON ? BOARD_SCALE_PX(60)
                                  : label_inset;
        int32_t label_width = width - label_left - accessory_space;
        /* configure_scrolling_row_label() (gui_plugins.c) is the one shared
         * construction path for every bounded scrolling row label, native or
         * plugin -- see row_label_apply_bounded_height()'s own comment for
         * why the label's height/pad_top can't just be lv_font_get_line_
         * height() directly. It also clamps label_width itself and defaults
         * text_align to LEFT, overridden below for center/right rows. */
        configure_scrolling_row_label(label, label_width);
        /* NULL/"left" (every native row) -> unchanged default. "center"/
         * "right" only shift where the text sits inside its own already-
         * reserved box (label_width above) -- pill_row_apply_icon()'s icon
         * placement is untouched either way. plugin.set_home_layout() only
         * (PLUGINS.md), list mode. */
        lv_text_align_t text_align = LV_TEXT_ALIGN_LEFT;
        if (item->text_align) {
            if (strcmp(item->text_align, "center") == 0) text_align = LV_TEXT_ALIGN_CENTER;
            else if (strcmp(item->text_align, "right") == 0) text_align = LV_TEXT_ALIGN_RIGHT;
        }
        lv_obj_set_style_text_align(label, text_align, 0);

        if (item->accessory == PILL_ACCESSORY_TOGGLE) {
            lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
            /* Non-clickable switch visual driven by row tap (row handles the click event). */
            lv_obj_t * toggle_sw = lv_switch_create(row);
            lv_obj_remove_flag(toggle_sw, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(toggle_sw, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(20), 0);
            if (item->toggle_initial_state) lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
            if (toggle_accent_style) lv_obj_add_style(toggle_sw, toggle_accent_style, LV_PART_INDICATOR | LV_STATE_CHECKED);

            if (item->out_toggle_img) *item->out_toggle_img = toggle_sw;

            pill_toggle_ctx_t * ctx = malloc(sizeof(pill_toggle_ctx_t));
            if (ctx) {
                ctx->toggle_sw = toggle_sw;
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, pill_toggle_row_event_cb, LV_EVENT_CLICKED, ctx);
                lv_obj_add_event_cb(row, pill_toggle_ctx_delete_cb, LV_EVENT_DELETE, ctx);

                if (item->on_toggle_change) {
                    lv_obj_add_event_cb(toggle_sw, item->on_toggle_change, LV_EVENT_VALUE_CHANGED, item->user_data);
                }
            } else {
                /* Out of memory -- degrade to a non-interactive row (shows
                 * the correct initial sprite, just can't be tapped) rather
                 * than dereferencing a NULL ctx. Without ctx, taps have
                 * nothing to flip, so on_toggle_change is deliberately left
                 * unwired too -- it would never fire anyway. */
                fprintf(stderr, "[screen_builders] out of memory building toggle row '%s'\n",
                        item->label ? item->label : "");
            }
        } else {
            if (item->accessory == PILL_ACCESSORY_CHEVRON) {
                build_navigation_chevron(row, BOARD_SCALE_PX(20), item->has_text_color, item->text_color);
            }

            if (item->on_click) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
                lv_obj_add_event_cb(row, item->on_click, LV_EVENT_CLICKED, item->user_data);
            }
        }
    }

    /* Headerless Home lists should occupy the actual Home viewport, ending
     * within its full viewport. Keep START and the native pads
     * for oversized lists so their first row remains reachable by scrolling. */
    int32_t list_h = lv_display_get_vertical_resolution(lv_display_get_default())
                   - header_h;
    if (!title && !back_btn_cb && content_h <= list_h) {
        lv_obj_set_style_pad_top(list, 0, 0);
        lv_obj_set_style_pad_bottom(list, 0, 0);
        lv_obj_set_flex_align(list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    return scr;
}

lv_obj_t * build_launcher_menu_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const icon_grid_item_t * items, int item_count,
                                      int icon_scale_pct, bool label_inside_icon,
                                      const launcher_menu_layout_t * layout) {
    if (!layout || !layout->list_mode)
        return build_icon_grid_screen(title, back_btn_cb, items, item_count, icon_scale_pct,
                                      label_inside_icon, 0);

    pill_list_item_t rows[item_count];
    for (int i = 0; i < item_count; i++) {
        bool accessory = items[i].has_accessory ? items[i].accessory
                                                 : (layout->has_accessory && layout->accessory);
        bool show_icon = items[i].has_icon ? items[i].icon : (layout->has_icon && layout->icon);
        rows[i] = (pill_list_item_t) {
            .label = items[i].label,
            .accessory = accessory ? PILL_ACCESSORY_CHEVRON : PILL_ACCESSORY_NONE,
            .on_click = items[i].on_click, .user_data = items[i].user_data,
            .icon_asset = show_icon ? asset_path_plain(items[i].icon_asset) : NULL,
            .row_height = items[i].has_row_height ? items[i].row_height : layout->height,
            .row_width = items[i].has_row_width ? items[i].row_width : layout->width,
            .has_bg_color = items[i].has_bg_color || layout->has_bg_color,
            .bg_color = items[i].has_bg_color ? items[i].bg_color : layout->bg_color,
            .has_text_color = items[i].has_text_color || layout->has_text_color,
            .text_color = items[i].has_text_color ? items[i].text_color : layout->text_color,
            .has_radius = items[i].has_radius || layout->has_radius,
            .radius = items[i].has_radius ? items[i].radius : layout->radius,
            .text_size = items[i].text_size ? items[i].text_size
                                             : (layout->text_size[0] ? layout->text_size : NULL),
            .text_align = items[i].text_align ? items[i].text_align
                                               : (layout->align[0] ? layout->align : NULL),
        };
    }
    return build_pill_list_screen(title, back_btn_cb, rows, item_count, gui_theme_accent_style(),
                                  layout->row_gap > 0 ? layout->row_gap : 6, icon_scale_pct);
}

typedef struct category_shared_image {
    char * relative_path;
    bool gradient;
    unsigned refcount;
    asset_decoded_image_t asset;
    struct category_shared_image * next;
} category_shared_image_t;

typedef struct {
    category_shared_image_t * shared;
} category_image_owner_t;

static category_shared_image_t * category_shared_images;
static void category_shared_image_release(category_shared_image_t * shared);

typedef struct {
    lv_obj_t * label;
    lv_obj_t * icon;
    lv_obj_t * background;
    lv_color_t background_default_color;
    bool plugin_icon; /* icon drawn by pill_row_apply_icon(), not retained here */
} category_row_context_t;

static void category_image_delete_cb(lv_event_t * e) {
    /* LV_EVENT_DELETE can bubble. This callback owns exactly the image it
     * was attached to, so a descendant's deletion must not drop its ref. */
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    category_image_owner_t * owner = lv_event_get_user_data(e);
    if (!owner) return;
    category_shared_image_t * shared = owner->shared;
    free(owner);
    category_shared_image_release(shared);
}

static category_shared_image_t * category_shared_image_acquire(const char * relative_path,
                                                                bool gradient) {
    if (!relative_path) return NULL;
    for (category_shared_image_t * shared = category_shared_images; shared;
         shared = shared->next) {
        if (shared->gradient == gradient &&
            strcmp(shared->relative_path, relative_path) == 0) {
            shared->refcount++;
            return shared;
        }
    }

    category_shared_image_t * shared = calloc(1, sizeof(*shared));
    if (!shared) return NULL;
    shared->relative_path = strdup(relative_path);
    if (!shared->relative_path) {
        free(shared);
        return NULL;
    }
    bool opened = gradient
                    ? asset_decoded_gradient_open(&shared->asset, relative_path)
                    : asset_decoded_image_open(&shared->asset, relative_path);
    if (!opened) {
        free(shared->relative_path);
        free(shared);
        return NULL;
    }
    shared->gradient = gradient;
    shared->refcount = 1;
    shared->next = category_shared_images;
    category_shared_images = shared;
    return shared;
}

static void category_shared_image_release(category_shared_image_t * shared) {
    if (!shared || shared->refcount == 0) return;
    if (--shared->refcount != 0) return;
    category_shared_image_t ** link = &category_shared_images;
    while (*link && *link != shared) link = &(*link)->next;
    if (*link == shared) *link = shared->next;
    asset_decoded_image_close(&shared->asset);
    free(shared->relative_path);
    free(shared);
}

static lv_obj_t * category_create_retained_image(lv_obj_t * row, const char * relative_path,
                                                  bool gradient) {
    category_shared_image_t * shared = category_shared_image_acquire(relative_path, gradient);
    if (!shared) return NULL;
    category_image_owner_t * owner = malloc(sizeof(*owner));
    if (!owner) {
        category_shared_image_release(shared);
        return NULL;
    }
    owner->shared = shared;
    lv_obj_t * image = lv_image_create(row);
    if (!image) {
        category_shared_image_release(shared);
        free(owner);
        return NULL;
    }
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(image, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_image_set_src(image, asset_decoded_image_source(&shared->asset));
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_event_cb(image, category_image_delete_cb, LV_EVENT_DELETE, owner);
    return image;
}

static void category_row_fit(lv_obj_t * row, category_row_context_t * context) {
    int32_t width = lv_obj_get_width(row);
    int32_t height = lv_obj_get_height(row);
    if (width <= 0 || height <= 0) return;
    if (context->background) {
        if (lv_obj_get_width(context->background) != width ||
            lv_obj_get_height(context->background) != height)
            lv_obj_set_size(context->background, width, height);
        lv_obj_align(context->background, LV_ALIGN_CENTER, 0, 0);
    }
    if ((context->icon || context->plugin_icon) && context->label) {
        int32_t left = BOARD_SCALE_PX(96);
        int32_t available = width - left - 60;
        if (available < 1) available = 1;
        if (lv_obj_get_width(context->label) != available)
            lv_obj_set_width(context->label, available);
        lv_obj_align(context->label, LV_ALIGN_LEFT_MID, left, 0);
    }
}

static void category_row_sync_background_style(lv_obj_t * row,
                                                category_row_context_t * context) {
    if (!context->background) return;
    lv_color_t row_color = lv_obj_get_style_bg_color(row, LV_PART_MAIN);
    if (lv_color_eq(row_color, context->background_default_color))
        lv_obj_remove_flag(context->background, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(context->background, LV_OBJ_FLAG_HIDDEN);
}

static void category_row_event_cb(lv_event_t * e) {
    category_row_context_t * context = lv_event_get_user_data(e);
    lv_obj_t * row = lv_event_get_current_target(e);
    if (!context || lv_event_get_target(e) != row) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        free(context);
        return;
    }
    if (code == LV_EVENT_SIZE_CHANGED) category_row_fit(row, context);
    else if (code == LV_EVENT_STYLE_CHANGED) category_row_sync_background_style(row, context);
    else if (context->background &&
             (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)) {
        lv_obj_set_style_image_recolor_opa(context->background,
                                          code == LV_EVENT_PRESSED ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    }
}

bool breeze_row_icon(const char * stock, char * out, size_t out_size) {
    int n = snprintf(out, out_size, "breeze_rows/%s", stock);
    if (n <= 0 || (size_t) n >= out_size || strchr(stock, '/') != strrchr(stock, '/')) return false;
    for (char * c = out + strlen("breeze_rows/"); *c; c++) if (*c == '/') *c = '_';
    char layer[GUI_THEMES_MAX_PATH + 64];
    if (gui_themes_resolve_icon_layer(stock, layer, sizeof(layer)) && !strstr(layer, asset_stock_root()))
        return false;
    return breeze_asset_if_present(out) != NULL;
}

static void decorate_category_row_impl(lv_obj_t * row, const char * icon_asset, const char * bg_asset,
                                       const char * plugin_icon) {
    if (!row) return;
    /* Stock row art gives way to pre-tinted Breeze art named after it,
     * breeze_rows/<folder>_<name>.png, unless the active theme brings its
     * own (see breeze_rows/BREEZE_SOURCES.txt). */
    char breeze_row[96];
    if (icon_asset && breeze_row_icon(icon_asset, breeze_row, sizeof(breeze_row))) icon_asset = breeze_row;
    category_row_context_t * context = calloc(1, sizeof(*context));
    if (!context) return;
    /* build_pill_list_screen() creates the primary label first. */
    context->label = lv_obj_get_child(row, 0);
    context->background_default_color = LIST_ROW_BG_COLOR;
    context->background = category_create_retained_image(row, bg_asset, true);
    if (context->background) {
        lv_obj_set_style_clip_corner(row, true, 0);
        lv_obj_move_to_index(context->background, 0);
        lv_obj_set_style_image_recolor(context->background, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(context->background, LV_OPA_TRANSP, 0);
    }
    context->icon = category_create_retained_image(row, icon_asset, false);
    if (context->icon) {
        lv_obj_set_size(context->icon, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_obj_align(context->icon, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(28), 0);
    }
    if (plugin_icon && context->label) {
        uint32_t children = lv_obj_get_child_count(row);
        pill_row_apply_icon(row, context->label, plugin_icon, BOARD_SCALE_PX(44), LV_ALIGN_LEFT_MID,
                            BOARD_SCALE_PX(28), 0);
        context->plugin_icon = lv_obj_get_child_count(row) > children;
    }
    lv_obj_add_event_cb(row, category_row_event_cb, LV_EVENT_ALL, context);
    category_row_sync_background_style(row, context);
    category_row_fit(row, context);
}

void decorate_category_row(lv_obj_t * row, const char * icon_asset, const char * bg_asset) {
    decorate_category_row_impl(row, icon_asset, bg_asset, NULL);
}

void decorate_category_row_plugin_icon(lv_obj_t * row, const char * plugin_icon) {
    decorate_category_row_impl(row, NULL, NULL, plugin_icon);
}

/* Play All rows can be pooled labels or ordinary container rows. Align to
 * the physical card in both cases: LVGL's child alignment origin excludes
 * the parent's padding, so cancel the horizontal inset and asymmetric
 * vertical text padding used by shared list-row labels. */
lv_obj_t * decorate_play_all_row(lv_obj_t * row) {
    if (!row) return NULL;

    lv_obj_t * circle = lv_obj_create(row);
    if (!circle) return NULL;
    lv_obj_set_size(circle, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_add_style(circle, gui_theme_accent_style(), 0);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(circle, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t * play = lv_label_create(circle);
    if (play) {
        lv_label_set_text(play, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(play, lv_color_white(), 0);
        const lv_font_t * play_font = gui_theme_font(GUI_FONT_ROLE_BODY);
        lv_obj_set_style_text_font(play, play_font, 0);
        lv_obj_align(play, LV_ALIGN_CENTER, 0, 0);
        /* The play glyph's advance box is centered by the label, but the
         * ink is a right-pointing triangle whose centroid sits one third
         * across its glyph box. Center that ink using the active font's
         * actual bearing and bounds (including any fallback font). */
        lv_font_glyph_dsc_t play_glyph;
        if (lv_font_get_glyph_dsc(play_font, &play_glyph, 0xF04B, 0) && play_glyph.box_w > 0) {
            int32_t advance_center = play_glyph.adv_w / 2;
            int32_t ink_center = play_glyph.ofs_x + play_glyph.box_w / 3;
            lv_obj_set_style_translate_x(play, advance_center - ink_center, 0);
        }
        lv_obj_remove_flag(play, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    int32_t pad_left = 0;
    int32_t pad_top = 0;
    int32_t pad_bottom = 0;
    if (lv_obj_check_type(row, &lv_label_class)) {
        pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
        pad_top = lv_obj_get_style_pad_top(row, LV_PART_MAIN);
        pad_bottom = lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
    }
    lv_obj_align(circle, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(28) - pad_left,
                 (pad_top - pad_bottom) / -2);
    return circle;
}

lv_obj_t * build_category_menu_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const icon_grid_item_t * items, int item_count,
                                      const launcher_menu_layout_t * layout) {
    if (!items || item_count <= 0) return NULL;
    pill_list_item_t rows[item_count];
    lv_obj_t * row_objects[item_count];
    memset(row_objects, 0, sizeof(row_objects));
    /* Category rows use the shared native default. Leave that default
     * implicit so native_row_min_style can follow live font-tier changes. */

    for (int i = 0; i < item_count; ++i) {
        bool accessory = items[i].has_accessory ? items[i].accessory
                         : !(layout && layout->has_accessory && !layout->accessory);
        const char * text_size = items[i].text_size ? items[i].text_size
                              : (layout && layout->text_size[0] ? layout->text_size : NULL);
        rows[i] = (pill_list_item_t) {
            .label = items[i].label,
            .accessory = accessory ? PILL_ACCESSORY_CHEVRON : PILL_ACCESSORY_NONE,
            .on_click = items[i].on_click,
            .user_data = items[i].user_data,
            .row_height = items[i].has_row_height ? items[i].row_height
                        : (layout && layout->height > 0 ? layout->height : 0),
            .row_width = items[i].has_row_width ? items[i].row_width : (layout ? layout->width : 0),
            .text_size = text_size,
            .has_bg_color = items[i].has_bg_color || (layout && layout->has_bg_color),
            .bg_color = items[i].has_bg_color ? items[i].bg_color : (layout ? layout->bg_color : 0),
            .has_text_color = items[i].has_text_color || (layout && layout->has_text_color),
            .text_color = items[i].has_text_color ? items[i].text_color : (layout ? layout->text_color : 0),
            .has_radius = items[i].has_radius || (layout && layout->has_radius),
            .radius = items[i].has_radius ? items[i].radius : (layout ? layout->radius : 0),
            .text_align = items[i].text_align ? items[i].text_align
                        : (layout && layout->align[0] ? layout->align : NULL),
            .out_row = &row_objects[i],
        };
    }
    int32_t row_gap = layout && layout->row_gap > 0 ? layout->row_gap : GUI_ROW_GAP;
    lv_obj_t * scr = build_pill_list_screen(title, back_btn_cb, rows, item_count,
                                            gui_theme_accent_style(), row_gap, 100);
    for (int i = 0; i < item_count; ++i) {
        bool show_icon = items[i].has_icon ? items[i].icon
                       : !(layout && layout->has_icon && !layout->icon);
        bool custom_background = items[i].has_bg_color || (layout && layout->has_bg_color);
        if (row_objects[i]) {
            if (show_icon && items[i].plugin_icon)
                decorate_category_row_plugin_icon(row_objects[i], items[i].plugin_icon);
            else
                decorate_category_row(row_objects[i], show_icon ? items[i].icon_asset : NULL,
                                      custom_background ? NULL : items[i].bg_image);
        }
    }
    return scr;
}

int append_plugin_list_rows(pill_list_item_t * items, int count, int max_items,
                            plugin_list_item_count_cb_t get_count_fn,
                            plugin_list_item_label_cb_t get_label_fn,
                            plugin_list_item_options_cb_t get_options_fn,
                            lv_event_cb_t click_cb) {
    if (!items || count < 0 || max_items <= 0 || !get_count_fn || !get_label_fn || !get_options_fn) {
        return count;
    }

    int plugin_count = get_count_fn();
    for (int i = 0; i < plugin_count && i < max_items; i++) {
        pill_list_item_t item = {
            get_label_fn(i), PILL_ACCESSORY_CHEVRON, false,
            click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        get_options_fn(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }
    return count;
}

lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    int32_t row_width = pill_row_default_width();
    int32_t height = ui_list_row_height();
    lv_obj_set_size(row, row_width, height);
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET, 0);
    configure_scrolling_row_label(label, row_width - 136);
    return row;
}

lv_obj_t * add_pill_summary_label(lv_obj_t * row) {
    lv_obj_update_layout(row);
    lv_obj_t * title = lv_obj_get_child(row, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(24), -BOARD_SCALE_PX(16));
    lv_obj_t * label = lv_label_create(row);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(label, lv_obj_get_width(row) - BOARD_SCALE_PX(84));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(24), BOARD_SCALE_PX(20));
    int32_t needed = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) +
                     lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) + BOARD_SCALE_PX(48);
    if (lv_obj_get_height(row) < needed) lv_obj_set_height(row, needed);
    return label;
}

lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);

    /* Non-clickable switch visual; the row itself handles the click event. */
    lv_obj_t * toggle_sw = lv_switch_create(row);
    lv_obj_remove_flag(toggle_sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(toggle_sw, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (checked) lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
    lv_obj_align(toggle_sw, LV_ALIGN_RIGHT_MID, -20, 0);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * label_text, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);
    build_navigation_chevron(row, BOARD_SCALE_PX(20), false, 0);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

lv_obj_t * add_pill_option_row(lv_obj_t * parent, const char * label_text, bool selected,
                              lv_event_cb_t on_click, void * user_data) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);
    if (selected) {
        lv_obj_add_style(row, gui_theme_accent_outline_style(), 0);
        lv_obj_set_style_border_width(row, BOARD_SCALE_PX(3), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_t * text = lv_obj_get_child(row, 0);
        configure_scrolling_row_label(text, pill_row_default_width() - GUI_TEXT_INSET - BOARD_SCALE_PX(64));
        lv_obj_t * check = lv_label_create(row);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_obj_add_style(check, gui_theme_accent_style(), 0);
        lv_obj_set_style_text_font(check, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-20), 0);
        lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, user_data);
    return row;
}

lv_obj_t * build_check_box(lv_obj_t * parent, bool checked) {
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, BOARD_SCALE_PX(24), BOARD_SCALE_PX(24));
    lv_obj_set_style_radius(box, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_border_width(box, BOARD_SCALE_PX(2), 0);
    lv_obj_set_style_border_color(box, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(box, checked ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(box, accent_lv_color(), 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (checked) {
        lv_obj_t * check = lv_label_create(box);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_color_t check_color = lv_color_brightness(accent_lv_color()) > 160
            ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white();
        lv_obj_set_style_text_color(check, check_color, 0);
        lv_obj_center(check);
        lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    return box;
}

lv_obj_t * add_section_header(lv_obj_t * parent, const char * text) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
#if defined(BOARD_TEMPOTEC_V1)
    /* Section captions can be longer than the entire narrow display. Give
     * them the list's content width so LVGL wraps instead of centering a
     * source-sized label beyond both edges of the viewport. */
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(label, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_right(label, BOARD_SCALE_PX(24), 0);
#else
    lv_obj_set_style_pad_top(label, 12, 0);
    lv_obj_set_style_pad_left(label, 24, 0);
#endif
    return label;
}

lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * title_label = build_screen_header(scr, default_title, generic_back_cb, NULL, NULL);

    lv_obj_t * list = lv_obj_create(scr);
    lv_obj_set_size(list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    /* Clear padding so rows align cleanly to screen edges without horizontal offset. */
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER); /* see build_icon_grid_screen's comment in screen_builders.c */
    screen_builders_configure_content_scrollbar(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_top(list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_bottom(list, BOARD_SCALE_PX(8), 0);

    *out_title_label = title_label;
    *out_list = list;
    finalize_screen_navigation(scr);
    return scr;
}

void configure_cover_card_grid(lv_obj_t * list, int columns) {
    if (!list || columns < 1) return;
    int32_t side = BOARD_SCALE_PX(16), gap = BOARD_SCALE_PX(14);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(list, side, 0);
    lv_obj_set_style_pad_right(list, side, 0);
    lv_obj_set_style_pad_column(list, gap, 0);
    lv_obj_set_style_pad_row(list, gap, 0);
}

typedef struct {
    lv_obj_t * cover;
    lv_point_t press_point;
    bool tracking;
    bool cancelled;
} cover_card_event_ctx_t;

/* LVGL suppresses CLICKED once scrolling owns the pointer. Track the
 * original touch point too, so small drags and press-lost transitions do
 * not select a card. The caller's callback keeps its original user data. */
static void cover_card_guard_event_cb(lv_event_t * e) {
    cover_card_event_ctx_t * ctx = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        lv_free(ctx);
        return;
    }
    if (!ctx) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t * indev = lv_event_get_indev(e);
        if (indev) lv_indev_get_point(indev, &ctx->press_point);
        ctx->tracking = indev != NULL;
        ctx->cancelled = indev && lv_indev_get_scroll_obj(indev) != NULL;
        lv_obj_set_style_border_width(ctx->cover, BOARD_SCALE_PX(2), 0);
    } else if (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED) {
        lv_indev_t * indev = lv_event_get_indev(e);
        if (ctx->tracking && indev) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            if (LV_ABS(point.x - ctx->press_point.x) > BOARD_SCALE_PX(8) ||
                LV_ABS(point.y - ctx->press_point.y) > BOARD_SCALE_PX(8) ||
                lv_indev_get_scroll_obj(indev))
                ctx->cancelled = true;
        }
        if (code == LV_EVENT_RELEASED) {
            ctx->tracking = false;
            lv_obj_set_style_border_width(ctx->cover, 0, 0);
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        ctx->tracking = false;
        ctx->cancelled = true;
        lv_obj_set_style_border_width(ctx->cover, 0, 0);
    } else if (code == LV_EVENT_CLICKED) {
        if (ctx->cancelled) lv_event_stop_processing(e);
        ctx->tracking = false;
        ctx->cancelled = false;
    }
}

lv_obj_t * add_cover_card(lv_obj_t * list, const char * label,
                          const char * resolved_image_src, int columns,
                          lv_event_cb_t on_click, void * user_data) {
    if (!list || columns < 1) return NULL;
    /* Resolve percentage widths before sizing cards in an inset container,
     * such as Quick Setup; full-screen grids retain the same geometry. */
    lv_obj_update_layout(list);
    int32_t gap = lv_obj_get_style_pad_column(list, 0);
    int32_t card_w = (lv_obj_get_content_width(list) - (columns - 1) * gap) / columns;
    if (card_w < 1) return NULL;
    int32_t cover_h = card_w * 3 / 2;
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);

    lv_obj_t * card = lv_obj_create(list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(6), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * cover = lv_obj_create(card);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, card_w, cover_h);
    lv_obj_add_style(cover, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cover, BOARD_SCALE_PX(10), 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_border_color(cover, lv_color_white(), 0);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (resolved_image_src && resolved_image_src[0]) {
        lv_obj_t * image = lv_image_create(cover);
        lv_image_header_t header;
        if (cover_card_preview_set(image, resolved_image_src, card_w, cover_h)) {
            lv_obj_center(image);
        } else if (lv_image_decoder_get_info(resolved_image_src, &header) == LV_RESULT_OK &&
                   header.w > 0 && header.h > 0) {
            lv_image_set_src(image, resolved_image_src);
            int64_t scale_w = (int64_t) card_w * 256 / header.w;
            int64_t scale_h = (int64_t) cover_h * 256 / header.h;
            lv_image_set_scale(image, (uint32_t) (scale_w < scale_h ? scale_w : scale_h));
            lv_obj_center(image);
        } else {
            lv_obj_delete(image);
        }
    }
    if (lv_obj_get_child_count(cover) == 0) {
        lv_obj_t * placeholder = lv_label_create(cover);
        lv_label_set_text(placeholder, label);
        lv_obj_add_style(placeholder, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(placeholder, font, 0);
        lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(placeholder, card_w - BOARD_SCALE_PX(16));
        lv_label_set_long_mode(placeholder, LV_LABEL_LONG_WRAP);
        lv_obj_center(placeholder);
    }

    lv_obj_t * title = lv_label_create(card);
    lv_label_set_text(title, label);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(title, card_w, lv_font_get_line_height(font) * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    if (on_click) {
        cover_card_event_ctx_t * ctx = lv_malloc(sizeof(*ctx));
        if (ctx) {
            *ctx = (cover_card_event_ctx_t) { .cover = cover };
            lv_obj_add_event_cb(card, cover_card_guard_event_cb, LV_EVENT_ALL, ctx);
            lv_obj_add_event_cb(card, on_click, LV_EVENT_CLICKED, user_data);
        } else {
            lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    return card;
}

/* Shared 2-button confirmation popup builder (backdrop, card, wrapped title,
 * and confirm/cancel buttons) using LV_SIZE_CONTENT and flex layout to
 * accommodate varying font sizes. */
lv_obj_t * build_popup_surface(lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop) {
    lv_obj_t * top = lv_layer_top();
    lv_obj_t * backdrop = lv_obj_create(top);
    lv_obj_set_size(backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    if (backdrop_cb) lv_obj_add_event_cb(backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * popup = lv_obj_create(top);
    lv_obj_set_width(popup, lv_pct(92));
    lv_obj_set_height(popup, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(popup,
        lv_display_get_vertical_resolution(lv_display_get_default()) - 2 * STATUS_BAR_CLEARANCE, 0);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(popup, LIST_ROW_RADIUS, 0);
    /* The card's Breeze frame (its border color follows the theme). */
    lv_obj_add_style(popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, ICON_GRID_TILE_BORDER, 0);
    lv_obj_set_style_pad_all(popup, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_row(popup, GUI_ROW_GAP, 0);
    lv_obj_set_scroll_dir(popup, LV_DIR_VER);
    screen_builders_configure_content_scrollbar(popup);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    *out_backdrop = backdrop;
    return popup;
}

void style_popup_action_row(lv_obj_t * row) {
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, BOARD_SCALE_PX(88), 0);
    lv_obj_set_style_pad_all(row, BOARD_SCALE_PX(14), 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_set_style_radius(row, BOARD_SCALE_PX(12), 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    /* Flex gives wrapped labels a real content height and centers short
     * labels without reducing the surrounding touch target. */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static bool popup_text_is_cancel(const char * text) {
    return text && (strcmp(text, "Cancel") == 0 || strcmp(text, TR("Cancel")) == 0);
}

lv_obj_t * add_popup_action_row(lv_obj_t * popup, const char * text, lv_color_t color,
                                lv_event_cb_t cb, void * user_data) {
    lv_obj_t * row = lv_obj_create(popup);
    style_popup_action_row(row);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t * label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void build_titled_popup(gui_popup_t * popup, lv_event_cb_t backdrop_cb, lv_obj_t ** title_out,
                        lv_obj_t ** body_out) {
    popup->popup = build_popup_surface(backdrop_cb, &popup->backdrop);

    lv_obj_t * title = lv_label_create(popup->popup);
    lv_label_set_text(title, "");
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    if (title_out) *title_out = title;

    if (body_out) {
        /* A long description scrolls inside the popup instead of pushing the
         * buttons off a small screen. */
        lv_obj_t * scroll = lv_obj_create(popup->popup);
        lv_obj_remove_style_all(scroll);
        lv_obj_set_width(scroll, lv_pct(100));
        lv_obj_set_height(scroll, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(scroll, BOARD_SCALE_PX(180), 0);
        lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
        lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_t * body = lv_label_create(scroll);
        lv_label_set_text(body, "");
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_style(body, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        *body_out = body;
        lv_obj_remove_flag(popup->popup, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(popup->popup, LV_DIR_NONE);
    }
}

lv_obj_t * build_confirm_popup_with_labels(const char * title_text, lv_label_long_mode_t title_long_mode,
                                           lv_obj_t ** out_title, const char * body_text, lv_obj_t ** out_body,
                                           const char * confirm_text, lv_obj_t ** out_confirm_label,
                                           lv_color_t confirm_color, lv_event_cb_t confirm_cb,
                                           lv_obj_t ** out_confirm_row, const char * cancel_text,
                                           lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                                           lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb,
                                           lv_obj_t ** out_backdrop) {
    lv_obj_t * popup = build_popup_surface(backdrop_cb, out_backdrop);
    lv_obj_set_style_pad_row(popup, BOARD_SCALE_PX(12), 0);

    lv_obj_t * title = lv_label_create(popup);
    lv_label_set_text(title, title_text);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, title_long_mode);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    if (out_title) *out_title = title;

    if (body_text) {
        lv_obj_t * body = lv_label_create(popup);
        lv_label_set_text(body, body_text);
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_style(body, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        if (out_body) *out_body = body;
    } else if (out_body) {
        *out_body = NULL;
    }

    lv_obj_t * confirm_row = lv_obj_create(popup);
    style_popup_action_row(confirm_row);
    lv_obj_add_event_cb(confirm_row, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * confirm_label = lv_label_create(confirm_row);
    lv_label_set_text(confirm_label, confirm_text);
    lv_obj_set_style_text_color(confirm_label, confirm_color, 0);
    lv_obj_set_style_text_font(confirm_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(confirm_label, lv_pct(100));
    lv_label_set_long_mode(confirm_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(confirm_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(confirm_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (out_confirm_label) *out_confirm_label = confirm_label;
    if (out_confirm_row) *out_confirm_row = confirm_row;

    lv_obj_t * cancel_row = lv_obj_create(popup);
    style_popup_action_row(cancel_row);
    lv_obj_add_event_cb(cancel_row, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, cancel_text);
    lv_obj_set_style_text_color(cancel_label, popup_text_is_cancel(cancel_text)
        ? lv_color_make(255, 120, 120) : cancel_color, 0);
    lv_obj_set_style_text_font(cancel_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(cancel_label, lv_pct(100));
    lv_label_set_long_mode(cancel_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(cancel_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(cancel_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (out_cancel_row) *out_cancel_row = cancel_row;

    return popup;
}

lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode,
                               lv_obj_t ** out_title, const char * body_text, const char * confirm_text,
                               lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row,
                               const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                               lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop) {
    return build_confirm_popup_with_labels(title_text, title_long_mode, out_title, body_text, NULL,
                                           confirm_text, NULL, confirm_color, confirm_cb, out_confirm_row,
                                           cancel_text, cancel_color, cancel_cb, out_cancel_row,
                                           backdrop_cb, out_backdrop);
}

static lv_obj_t * power_hub_card(lv_obj_t * parent, const char * title,
                                 const char * icon_path, const char * fallback,
                                 lv_event_cb_t cb, int index) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(48));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, BOARD_SCALE_PX(132), 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(16), 0);
    lv_obj_add_style(card, &pill_row_bg_style, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, BOARD_SCALE_PX(8), 0);
    lv_obj_add_style(card, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(8), 0);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    char breeze[96];
    const char * source = asset_path(breeze_row_icon(icon_path, breeze, sizeof(breeze)) ? breeze : icon_path);
    if (source) {
        lv_obj_t * icon = lv_image_create(card);
        lv_image_set_src(icon, source);
        /* Fit installed assets of any size into the same icon box. */
        lv_obj_set_size(icon, BOARD_SCALE_PX(48), BOARD_SCALE_PX(48));
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_add_style(icon, gui_theme_accent_style(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_t * icon = lv_label_create(card);
        lv_label_set_text(icon, fallback);
        lv_obj_set_style_text_font(icon, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
        lv_obj_add_style(icon, gui_theme_accent_style(), 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t * label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t * build_power_hub_panel(lv_obj_t * parent, lv_event_cb_t action_cb,
                                 lv_obj_t ** out_status, lv_obj_t ** out_actions) {
    static const char * titles[] = {N_("Power off"), N_("Restart"), N_("Screen off"), N_("Sleep timer")};
    static const char * icons[] = {"power_action/power.png", "power_action/reboot.png",
                                   "power_action/screen_off.png", "power_action/sleep_timer.png"};
    static const char * symbols[] = {LV_SYMBOL_POWER, LV_SYMBOL_REFRESH, LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_SETTINGS};
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel,
        lv_display_get_vertical_resolution(lv_display_get_default()), 0);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_style(panel, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_hor(panel, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_top(panel, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_bottom(panel, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_row(panel, BOARD_SCALE_PX(12), 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    screen_builders_configure_content_scrollbar(panel);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * title = lv_label_create(panel);
    lv_label_set_text(title, TR("Power controls"));
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * subtitle = lv_label_create(panel);
    lv_label_set_text(subtitle, TR("Playback and device actions"));
    lv_obj_set_width(subtitle, lv_pct(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_style(subtitle, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(subtitle, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_remove_flag(subtitle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * grid = lv_obj_create(panel);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(grid, BOARD_SCALE_PX(10), 0);
    lv_obj_set_style_pad_column(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int row_index = 0; row_index < 2; ++row_index) {
        lv_obj_t * row = lv_obj_create(grid);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        for (int column = 0; column < 2; ++column) {
            int i = row_index * 2 + column;
            lv_obj_t * card = power_hub_card(row, TR(titles[i]), icons[i], symbols[i], action_cb, i);
            if (out_actions) out_actions[i] = card;
        }
    }

    lv_obj_t * cancel = lv_obj_create(panel);
    lv_obj_set_width(cancel, lv_pct(100));
    lv_obj_set_height(cancel, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(cancel, LV_MAX(44, BOARD_SCALE_PX(64)), 0);
    lv_obj_set_style_radius(cancel, BOARD_SCALE_PX(14), 0);
    lv_obj_add_style(cancel, &pill_row_bg_style, 0);
    lv_obj_add_style(cancel, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(cancel, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(cancel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cancel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cancel, action_cb, LV_EVENT_CLICKED, (void *)(intptr_t)4);
    lv_obj_t * cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, TR("Cancel"));
    lv_obj_set_width(cancel_label, lv_pct(100));
    lv_obj_set_style_text_align(cancel_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(cancel_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_remove_flag(cancel_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (out_actions) out_actions[4] = cancel;

    lv_obj_t * status = lv_label_create(panel);
    lv_label_set_text(status, "");
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(status, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(status, gui_theme_font(GUI_FONT_ROLE_STATUS), 0);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (out_status) *out_status = status;
    return panel;
}

static unsigned visible_popup_count;

void gui_popup_show(gui_popup_t * p) {
    if (!p) return;
    if (!p->visible && (p->popup || p->backdrop)) {
        p->visible = true;
        visible_popup_count++;
    }
    if (p->backdrop) lv_obj_remove_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    if (p->popup) lv_obj_remove_flag(p->popup, LV_OBJ_FLAG_HIDDEN);
    if (p->backdrop) lv_obj_move_foreground(p->backdrop);
    if (p->popup) lv_obj_move_foreground(p->popup);
}

void gui_popup_hide(gui_popup_t * p) {
    if (!p) return;
    if (p->visible) {
        p->visible = false;
        if (visible_popup_count > 0) visible_popup_count--;
    }
    if (p->backdrop) lv_obj_add_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    if (p->popup) lv_obj_add_flag(p->popup, LV_OBJ_FLAG_HIDDEN);
}

void gui_popup_teardown(gui_popup_t * p) {
    if (!p) return;
    gui_popup_hide(p);
    if (p->popup) {
        lv_obj_delete(p->popup);
        p->popup = NULL;
    }
    if (p->backdrop) {
        lv_obj_delete(p->backdrop);
        p->backdrop = NULL;
    }
}

bool gui_popup_is_visible(const gui_popup_t * p) {
    return p && p->visible;
}

unsigned gui_popup_visible_count(void) {
    return visible_popup_count;
}

int find_nearest_step_index(const int * steps, int count, int value) {
    if (!steps || count <= 0) return 0;
    int best = 0;
    int best_diff = abs(value - steps[0]);
    for (int i = 1; i < count; i++) {
        int diff = abs(value - steps[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

extern void register_swipe_dead_zone(lv_obj_t * obj);

lv_obj_t * build_setting_slider_card(lv_obj_t * parent, lv_obj_t * align_target, int32_t card_height,
                                      int32_t track_top_offset, int32_t min_range, int32_t max_range,
                                      int32_t initial_val, lv_event_cb_t slider_cb,
                                      lv_obj_t ** out_slider, lv_obj_t ** out_value_label) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(90), card_height);
    lv_obj_align_to(card, align_target, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_all(card, BOARD_SCALE_PX(14), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_width(slider, lv_pct(94));
    lv_obj_set_height(slider, SLIDER_TRACK_HEIGHT);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, track_top_offset);
    lv_slider_set_range(slider, min_range, max_range);
    lv_slider_set_value(slider, initial_val, LV_ANIM_OFF);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_knob_style(), LV_PART_KNOB);
    lv_obj_set_style_width(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(slider, BOARD_SCALE_PX(26));

    lv_obj_t * value_label = lv_label_create(card);
    lv_obj_add_style(value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_set_width(value_label, lv_pct(100));
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(value_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_remove_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(card);

    if (out_slider) *out_slider = slider;
    if (out_value_label) *out_value_label = value_label;
    return card;
}

lv_obj_t * build_font_change_mask(int target) {
    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    lv_obj_set_user_data(mask, (void *)(intptr_t)(target + 1));
    lv_display_t * display = lv_display_get_default();
    lv_obj_set_size(mask,
                    lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display));
    lv_obj_align(mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);
    lv_obj_invalidate(mask);
    return mask;
}

int32_t ui_row_label_width(lv_obj_t * row, int32_t row_w, int32_t label_x, int32_t right_space) {
    const int32_t border = lv_obj_get_style_border_width(row, LV_PART_MAIN);
    const int32_t pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
    return row_w - (border + pad_left) - label_x - right_space;
}

/* Measured on the label's own (shaped) text, as drawn. */
bool ui_row_fit_label(lv_obj_t * row, lv_obj_t * label, const lv_font_t * font,
                      int32_t row_w, int32_t label_x, int32_t label_w) {
    const int32_t row_h = ui_list_row_height();
    const int32_t line_h = lv_font_get_line_height(font);
    lv_point_t text_size;
    lv_text_get_size(&text_size, lv_label_get_text(label), font, 0, 0, label_w, LV_TEXT_FLAG_NONE);
    const bool tall = text_size.y > line_h;
    lv_label_set_long_mode(label, tall ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    if (!tall) {
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, label_x, 0);
        return false;
    }
    /* Symmetric padding keeps the text off the card borders and puts its
     * first line where a one-line row centres it. */
    const int32_t border = lv_obj_get_style_border_width(row, LV_PART_MAIN);
    lv_obj_set_size(row, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, row_h, 0);
    lv_obj_set_style_pad_ver(row, (row_h - line_h) / 2 - border, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, label_x, 0);
    return true;
}

/* Native wide rows shared by saved playlists and CUE tracks. */
lv_obj_t * build_wide_list_row(lv_obj_t * parent, const char * label_text, bool chevron) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, ui_list_row_height());
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    configure_scrolling_row_label(label, LIST_ROW_WIDTH_WIDE - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(60));
    if (chevron) {
        build_navigation_chevron(row, GUI_TEXT_INSET, false, 0);
    }
    return row;
}
