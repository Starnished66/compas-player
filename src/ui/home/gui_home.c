#include "gui_home.h"
#include "home_background.h"
#include "home_layout.h"
#include "home_layout_sizing.h"
#include "../widgets/builders/screen_builders.h"
#include "../shell/gui_navigation.h"
#include "../library/browse/gui_library.h"
#include "../network/gui_network.h"
#include "../shell/gui_shell.h"
#include "../library/collections/gui_books.h"
#include "../network/subsonic/gui_subsonic.h"
#include "../../plugins/manager/plugin_manager.h"
#include "../text/i18n/i18n.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Published order for plugin.set_home_layout()'s `key` field (home_layout.h)
 * -- the one and only place this order is spelled out; plugin_manager.c
 * validates a Lua `key` string against this same array rather than keeping
 * its own copy that could drift out of sync. */
const char * const home_layout_tile_keys[HOME_LAYOUT_TILE_COUNT] = {
    "music", "stream_media", "wireless", "books", "settings", "dac", "subsonic",
};

/* Native per-tile metadata, same fixed order as home_layout_tile_keys[]
 * above -- shared by both branches of gui_home_build_screen() below so the
 * label/icon/click-target triple is written exactly once regardless of
 * which mode (tile or list) actually renders it. */
typedef struct {
    const char * icon_asset;
    const char * icon_asset_selected;
    const char * label;
    lv_event_cb_t on_click;
    uint32_t glow_color; /* 0 leaves the icon's background transparent. */
} home_native_tile_t;

/* One resolved entry (native tile or plugin-registered tile), independent
 * of tile-mode/list-mode -- resolve_home_tiles() below fills this once, and
 * both Home builder branches read from it, so the native-vs-plugin
 * resolution and style-override lookup are each written exactly once. */
typedef struct {
    const char * icon_asset;
    const char * icon_asset_selected;
    const char * label;
    lv_event_cb_t on_click;
    void * user_data;
    const home_tile_override_t * override; /* NULL = never restyled, every native default applies */
    uint32_t glow_color; /* 0 for plugin tiles */
} resolved_home_tile_t;

/* options.background_image (PLUGINS.md, plugin.set_home_layout()) -- sets
 * Home's OWN root object's bg_image_src directly, not style_theme_screen_bg
 * (shared by every screen in the app, mutated by plugin.set_background_
 * color("screen", ...)). scr's grid/tile children are already transparent
 * (build_icon_grid_screen()'s own bg_opa=0 on both, screen_builders.c)
 * unless a per-tile bg_color override says otherwise, so this shows through
 * cleanly behind them with no other change needed. Called from both
 * Home screen branches below, right after each builds its own scr,
 * so a plugin-set background applies whether Home is tile or list mode. */
/* A wallpaper shows through Home's tiles, so they drop their card surface. */
static void home_tiles_drop_cards(lv_obj_t * parent) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) {
        lv_obj_t * child = lv_obj_get_child(parent, (int32_t) i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_USER_1)) {
            lv_obj_remove_style(child, &style_theme_tile, 0);
            lv_obj_remove_style(child, &style_theme_tile_pressed, LV_STATE_PRESSED);
            /* Native tiles cleared their local transparency for the card;
             * restore it so LVGL's default white card doesn't show. */
            lv_style_value_t v;
            if (lv_obj_get_local_style_prop(child, LV_STYLE_BG_OPA, &v, 0) != LV_STYLE_RES_FOUND) {
                lv_obj_set_style_bg_opa(child, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(child, 0, 0);
                lv_obj_set_style_bg_opa(child, 64, LV_STATE_PRESSED);
                lv_obj_set_style_bg_color(child, lv_color_hex(GUI_COLOR_PRESSED), LV_STATE_PRESSED);
            }
        } else {
            home_tiles_drop_cards(child);
        }
    }
}

static void apply_home_background_image(lv_obj_t * scr) {
    if (!home_layout_config.configured || !home_layout_config.has_background_image) return;
    home_tiles_drop_cards(scr);
    home_background_apply(scr, home_layout_config.background_image);
}

static void music_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_library_get_music_screen());
}

static void wireless_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_network_get_wireless_screen());
}

static void dac_home_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_shell_get_dac_home_screen());
}

static void plugin_home_tile_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_home_tile_clicked(index);
}

/* home_layout_config.tiles[]/tile_count is a flat, unordered list keyed by
 * name (native key or plugin tile id) -- see home_layout.h's own comment.
 * Returns NULL, not a zeroed override, when `key` was never restyled, so
 * callers can tell "no override" apart from "an override that happens to
 * leave everything at its default" -- in practice both resolve the same
 * (every has_* false), so this distinction currently only matters for
 * clarity, not behavior. */
static const home_tile_override_t * find_home_tile_override(const char * key) {
    for (int i = 0; i < home_layout_config.tile_count; i++) {
        if (strcmp(home_layout_config.tiles[i].key, key) == 0) return &home_layout_config.tiles[i].override;
    }
    return NULL;
}

/* Resolves home_layout_config.order[] (or, when unconfigured, today's fixed
 * 6-native-tile order) into `out`, which must have room for
 * HOME_LAYOUT_MAX_TILES entries. Returns how many entries were actually
 * filled -- can be less than the order length when an entry names neither a
 * native key nor a currently-registered plugin tile id (that plugin failed
 * to load, or hasn't loaded yet -- see set_home_layout()'s own comment on
 * why this isn't rejected earlier, at Lua-call time); such an entry is
 * skipped and logged here, not treated as an error. This is also why this
 * resolution must happen here rather than in l_plugin_set_home_layout()
 * itself -- this always runs after plugin_manager_init() has finished
 * loading every plugin (gui_reload.c's own reload sequence runs plugin_
 * manager_init() before rebuilding any screen), so a plugin tile referenced
 * by an earlier-loading theme is reliably resolvable by the time Home is
 * actually built. */
static int resolve_home_tiles(resolved_home_tile_t * out, const home_native_tile_t * native_tiles) {
    int n = home_layout_config.order_count > 0 ? home_layout_config.order_count : HOME_LAYOUT_DEFAULT_TILE_COUNT;
    int count = 0;
    for (int i = 0; i < n; i++) {
        const char * key = home_layout_config.order_count > 0 ? home_layout_config.order[i] : home_layout_tile_keys[i];

        int native_idx = -1;
        for (int k = 0; k < HOME_LAYOUT_TILE_COUNT; k++) {
            if (strcmp(key, home_layout_tile_keys[k]) == 0) { native_idx = k; break; }
        }

        resolved_home_tile_t * r = &out[count];
        if (native_idx >= 0) {
            const home_native_tile_t * native = &native_tiles[native_idx];
            r->icon_asset = native->icon_asset;
            r->icon_asset_selected = native->icon_asset_selected;
            r->label = TR(native->label);
            r->on_click = native->on_click;
            r->user_data = NULL;
            r->glow_color = native->glow_color;
        } else {
            int plugin_idx = plugin_manager_find_home_tile_by_id(key);
            if (plugin_idx < 0) {
                fprintf(stderr, "[home] order entry '%s' is not a native tile or a currently-registered "
                                "plugin.register_home_tile() id -- skipping\n", key);
                continue;
            }
            r->icon_asset = plugin_manager_get_home_tile_icon(plugin_idx);
            r->icon_asset_selected = plugin_manager_get_home_tile_icon_selected(plugin_idx);
            r->label = plugin_manager_get_home_tile_label(plugin_idx);
            r->on_click = plugin_home_tile_click_cb;
            r->user_data = (void *) (intptr_t) plugin_idx;
            r->glow_color = 0;
        }
        r->override = find_home_tile_override(key);
        count++;
    }
    return count;
}

/* Home .theme measurements use the 480px R1 as their reference. Keep this
 * scaling private to Home so the same style forwarded to Music/Stream Media/
 * Wireless launchers continues to use the existing shared sizing rules. */
static int32_t home_tile_radius(const home_tile_override_t * ov, int32_t display_width) {
    return ov && ov->has_radius ? home_layout_scale_px(ov->radius, display_width) : 0;
}

static int32_t home_list_row_gap(int32_t display_width) {
    int32_t authored_gap = home_layout_config.row_gap > 0 ? home_layout_config.row_gap : 6;
    if (authored_gap > 84) authored_gap = 84;
    int32_t gap = home_layout_scale_px(authored_gap, display_width);
    if (gap > LIST_ROW_HEIGHT) gap = LIST_ROW_HEIGHT;
    return gap;
}

static void fit_home_list_rows(icon_grid_item_t * items, const resolved_home_tile_t * resolved,
                               int count, int32_t display_width, int32_t display_height,
                               int32_t row_gap) {
    if (count <= 0) return;
    int32_t preferred[HOME_LAYOUT_MAX_TILES];
    int32_t minimum[HOME_LAYOUT_MAX_TILES];
    int32_t heights[HOME_LAYOUT_MAX_TILES];
    int32_t total_gaps = (count - 1) * row_gap;
    int32_t viewport = display_height - STATUS_BAR_CLEARANCE;
    int32_t row_budget = viewport - total_gaps;

    for (int i = 0; i < count; ++i) {
        const home_tile_override_t * ov = resolved[i].override;
        int32_t authored = ov && ov->height > 0
                         ? home_layout_scale_py(ov->height, display_height)
                         : home_layout_scale_py(112, display_height);
        int32_t font_height = lv_font_get_line_height(
            pill_row_resolve_text_size(ov && ov->text_size[0] ? ov->text_size : NULL));
        int32_t font_min = font_height + 32;
        int32_t min_height = font_min > PILL_ROW_HEIGHT_MIN ? font_min : PILL_ROW_HEIGHT_MIN;
        if (ov && ov->has_icon && ov->icon && min_height < 64) min_height = 64;
        if (min_height > PILL_ROW_HEIGHT_MAX) min_height = PILL_ROW_HEIGHT_MAX;
        preferred[i] = authored;
        minimum[i] = min_height;

        int32_t width = ov && ov->width > 0
                      ? home_layout_scale_px(ov->width, display_width)
                      : display_width;
        if (width > display_width) width = display_width;
        if (width < PILL_ROW_WIDTH_MIN) width = PILL_ROW_WIDTH_MIN;
        items[i].row_width = width;
    }

    home_layout_fit_row_heights(preferred, minimum, count, row_budget,
                                PILL_ROW_HEIGHT_MAX, heights);
    for (int i = 0; i < count; ++i) items[i].row_height = heights[i];
}

lv_obj_t * gui_home_build_screen(lv_event_cb_t settings_cb, lv_event_cb_t stream_media_cb) {
    const home_native_tile_t native_tiles[HOME_LAYOUT_TILE_COUNT] = {
        { "launcher/music.png", "launcher/music_s.png", N_("Music"), music_tile_cb, 0xF5B457 },
        { "launcher/stream_media.png", "launcher/stream_media_s.png", N_("Stream Media"), stream_media_cb, 0x48ADE5 },
        { "launcher/wireless.png", "launcher/wireless_s.png", N_("Wireless"), wireless_tile_cb, 0x39BD95 },
        { "launcher/book.png", "launcher/book_s.png", N_("Books"), gui_books_home_tile_cb, 0xF5B457 },
        { "launcher/sys_set.png", "launcher/sys_set_s.png", N_("Settings"), settings_cb, 0x909EB5 },
        { "launcher/dac.png", "launcher/dac_s.png", N_("More"), dac_home_tile_cb, 0xA36BE4 },
        { "stream_media/subsonic.png", "stream_media/subsonic_s.png", N_("Subsonic"), subsonic_tile_cb, 0 },
    };
    static resolved_home_tile_t resolved[HOME_LAYOUT_MAX_TILES];
    int count = resolve_home_tiles(resolved, native_tiles);
    const home_tile_override_t zero_override = { 0 };

    /* plugin.set_home_layout()'s list-mode path -- a pill-list screen built
     * from the resolved tiles above instead of the icon grid, with each
     * row's style pulled from its own override. See home_layout.h's own
     * comment for why this only ever reflects whatever was configured at
     * THIS boot's plugin-load time (or the most recent plugin.refresh_
     * theme()/reload_ui()), never a live mid-session change outside that. */
    if (home_layout_config.configured && home_layout_config.list_mode) {
        static icon_grid_item_t items[HOME_LAYOUT_MAX_TILES];
        lv_display_t * display = lv_display_get_default();
        int32_t display_width = display ? lv_display_get_horizontal_resolution(display) : BOARD_SCREEN_WIDTH;
        int32_t display_height = display ? lv_display_get_vertical_resolution(display) : BOARD_SCREEN_HEIGHT;
        int32_t row_gap = home_list_row_gap(display_width);
        for (int i = 0; i < count; i++) {
            const home_tile_override_t * ov = resolved[i].override ? resolved[i].override : &zero_override;
            items[i] = (icon_grid_item_t){
                .icon_asset = resolved[i].icon_asset,
                .label = resolved[i].label,
                .on_click = resolved[i].on_click,
                .user_data = resolved[i].user_data,
                .has_bg_color = ov->has_bg_color, .bg_color = ov->bg_color,
                .has_text_color = ov->has_text_color, .text_color = ov->text_color,
                .has_radius = ov->has_radius,
                .radius = home_tile_radius(ov, display_width),
                /* Every field below is a per-tile LIST-MODE override
                 * (icon_grid_item_t's own doc comment, screen_builders.h) --
                 * always supplied here (has_* = true) since Home's per-tile
                 * defaults (no accessory/icon unless explicitly overridden)
                 * differ from build_launcher_menu_screen()'s own layout-level
                 * defaults, so there is no shared `layout` value worth
                 * falling back to. */
                .has_row_height = true, .row_height = ov->height,
                .has_row_width = true, .row_width = ov->width,
                .has_accessory = true, .accessory = ov->has_accessory && ov->accessory,
                .text_size = ov->text_size[0] ? ov->text_size : NULL,
                .text_align = ov->align[0] ? ov->align : NULL,
                .has_icon = true, .icon = ov->has_icon && ov->icon,
            };
        }
        fit_home_list_rows(items, resolved, count, display_width, display_height, row_gap);

        /* No per-screen style here (that lives entirely in each item's own
         * override above) -- this layout only carries what's genuinely
         * shared across every tile: list mode itself and the row gap. */
        launcher_menu_layout_t home_list_layout = {
            .list_mode = true,
            .row_gap = row_gap,
        };
        lv_obj_t * scr = build_launcher_menu_screen(NULL, NULL, items, count, 100, false, &home_list_layout);
        apply_home_background_image(scr);
        finalize_screen_navigation(scr);
        return scr;
    }

    lv_display_t * display = lv_display_get_default();
    int32_t display_width = display ? lv_display_get_horizontal_resolution(display) : BOARD_SCREEN_WIDTH;
    static icon_grid_item_t items[HOME_LAYOUT_MAX_TILES];
    for (int i = 0; i < count; i++) {
        const home_tile_override_t * ov = resolved[i].override ? resolved[i].override : &zero_override;
        items[i] = (icon_grid_item_t){
            .icon_asset = resolved[i].icon_asset,
            .icon_asset_selected = resolved[i].icon_asset_selected,
            .label = resolved[i].label,
            .on_click = resolved[i].on_click,
            .user_data = resolved[i].user_data,
            .has_bg_color = ov->has_bg_color, .bg_color = ov->bg_color,
            .has_text_color = ov->has_text_color, .text_color = ov->text_color,
            .has_radius = ov->has_radius,
            .radius = home_tile_radius(ov, display_width),
            /* Explicit plugin surfaces take precedence over native glows. */
            .icon_glow_color = ov->has_bg_color ? 0 : resolved[i].glow_color,
        };
    }
    /* No back_btn_cb -- this is the true root, nothing to go back to. No
     * title either -- matches the real stock launcher, which has no header
     * text above its icon grid. tile_gap stays 0 (today's exact flush-cell
     * look) unless a plugin configured one. l_plugin_set_home_layout()
     * already rejects a tile-mode `order` past 6 entries, so `count` here
     * never exceeds what build_icon_grid_screen()'s own row math expects. */
    lv_obj_t * scr = build_icon_grid_screen(NULL, NULL, items, count, 100, false,
                                             home_layout_config.configured
                                               ? home_layout_scale_px(home_layout_config.tile_gap, display_width)
                                               : 0);
    apply_home_background_image(scr);
    finalize_screen_navigation(scr);
    return scr;
}
