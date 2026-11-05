#include "screen_builders.h"
#include "screen_builders_internal.h"
#include "../../assets/assets.h"
#include "../../appearance/gui_themes.h"

#include <unistd.h>
#include <stdio.h>
#include <string.h>

/* Screen header: Breeze title-bar art, header actions, title fitting and
 * build_screen_header(). */

static const char * breeze_asset_if_present(const char * relative) {
    const char * plain = asset_path_plain(relative);
    return plain && access(plain, R_OK) == 0 ? asset_path(relative) : NULL;
}

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

/* The title label build_screen_header() made on `scr` (a label flagged USER_4
 * with no user_data; subtitle labels carry user_data), or NULL. */
lv_obj_t * screen_header_title(lv_obj_t * scr) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t * child = lv_obj_get_child(scr, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            lv_obj_has_flag(child, LV_OBJ_FLAG_USER_4) && !lv_obj_get_user_data(child))
            return child;
    }
    return NULL;
}

/* A header action's own hitbox, without its image. */
static lv_obj_t * header_button_create(lv_obj_t * scr, lv_align_t alignment, lv_event_cb_t cb) {
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
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* Adds the non-clickable, centred image of a header action. Its image sits in
 * a 44x44 contain box on the V1; `always_fit` does so on every board. */
static lv_obj_t * header_button_add_image(lv_obj_t * btn, const char * src, bool always_fit) {
    lv_obj_t * image = lv_image_create(btn);
    lv_image_set_src(image, src);
    bool fit_box = always_fit;
#if defined(BOARD_TEMPOTEC_V1)
    fit_box = true;
#endif
    if (fit_box) {
        lv_obj_set_size(image, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    }
    lv_obj_align(image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    return image;
}

/* The back button is the same control, always fitted and text-coloured when
 * not Breeze art. */
static lv_obj_t * build_header_icon_button(lv_obj_t * scr, const char * asset,
                                          lv_align_t alignment, lv_event_cb_t cb, bool back) {
    lv_obj_t * btn = header_button_create(scr, alignment, cb);
    if (asset) {
        const char * breeze = titlebar_breeze_icon(asset);
        lv_obj_t * image = header_button_add_image(btn, breeze ? breeze : asset, back);
        if (breeze) style_titlebar_breeze_icon(image);
        else if (back) lv_obj_add_style(image, &style_theme_text_primary, 0);
    }
    return btn;
}

lv_obj_t * build_header_back_button(lv_obj_t * scr, lv_event_cb_t cb) {
    return build_header_icon_button(scr, asset_path("navigation/back.png"), LV_ALIGN_TOP_LEFT, cb, true);
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
    lv_obj_t * title = screen_header_title(lv_obj_get_parent(action));
    if (title) reserve_title_width_before(title, action);
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
    lv_obj_t * button = header_button_create(scr, LV_ALIGN_TOP_RIGHT, click_cb);
    align_screen_header_action(button, V1_SCALE_PX(20));

    const char * breeze = asset_path_plain("titlebar/breeze_refresh.png");
    if (breeze && access(breeze, R_OK) == 0) {
        lv_obj_t * image = header_button_add_image(
            button, text_glyph_asset("titlebar/breeze_refresh.png"), false);
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

/* Title label insets: 76 (64px back button + 12px breathing room) on the left,
 * 20 on the right. */
#define BUILD_TITLE_LEFT_INSET V1_SCALE_PX(76)
#define BUILD_TITLE_RIGHT_MARGIN V1_SCALE_PX(20)

lv_obj_t * build_top_right_icon_button(lv_obj_t * scr, const char * icon_asset, lv_event_cb_t click_cb) {
    lv_obj_t * btn = build_header_icon_button(scr, icon_asset, LV_ALIGN_TOP_RIGHT, click_cb, false);
    /* Some screens add their action after the shared header was built. */
    lv_obj_t * title = screen_header_title(scr);
    if (title) {
        lv_obj_set_width(title, screen_builders_active_display_width() - BUILD_TITLE_LEFT_INSET - HEADER_BUTTON_WIDTH - V1_SCALE_PX(12));
        screen_title_fit(title);
    }
    return btn;
}

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
    /* An ellipsised LV_LABEL_LONG_DOT label holds a truncated copy of its text: a NULL set
     * makes LVGL restore the full text (the dots are re-applied on the next refresh). */
    lv_label_set_text(label, NULL);
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

bool breeze_row_icon(const char * stock, char * out, size_t out_size) {
    int n = snprintf(out, out_size, "breeze_rows/%s", stock);
    if (n <= 0 || (size_t) n >= out_size || strchr(stock, '/') != strrchr(stock, '/')) return false;
    for (char * c = out + strlen("breeze_rows/"); *c; c++) if (*c == '/') *c = '_';
    char layer[GUI_THEMES_MAX_PATH + 64];
    if (gui_themes_resolve_icon_layer(stock, layer, sizeof(layer)) && !strstr(layer, asset_stock_root()))
        return false;
    return breeze_asset_if_present(out) != NULL;
}

const char * row_icon_or(const char * stock, const char * fallback) {
    char breeze[96];
    if (breeze_row_icon(stock, breeze, sizeof(breeze))) return stock;
    const char * plain = asset_path_plain(stock);
    return plain && access(plain, R_OK) == 0 ? stock : fallback;
}
