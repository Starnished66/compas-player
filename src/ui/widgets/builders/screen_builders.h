#ifndef SCREEN_BUILDERS_H
#define SCREEN_BUILDERS_H

#include "lvgl/lvgl.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../text/rendering/fallback_font.h"
#include "../../home/launcher_layout.h"
#include "../../appearance/gui_theme.h"
#include "../lists/compact_list.h"
#include <stdbool.h>

/* Settings -> Font Size uses the stable fallback-capable app_font_* handles
 * declared above, for fixed chrome and metadata alike. */

/* Two screen layouts repeat across the real stock UI: an icon grid (Home
 * launcher, Music/Wireless/Stream Media submenus) and a scrollable list of
 * pill-shaped rows (System settings, Books). Both builders are pure layout
 * -- navigation (back button behavior, swipe gestures) is wired by the
 * caller afterwards, same as every other screen in gui.c, so this module
 * has no dependency on the nav stack. */

/* Shared header-band heights -- every screen, including the hand-built ones
 * in gui.c that don't go through a builder here (player, accent color, EQ,
 * text entry, group songs, subsonic lists), reserves these so its own
 * content never competes with the persistent status bar (clock/battery/
 * wifi, drawn separately on lv_layer_top()) for the same pixels. */
/* Clearance band height (40px) between the display top and screen headers.
 * The largest status glyph (battery) is rendered at 38px, leaving 1px
 * margin above/below it. Consumers derive positions algebraically from this
 * constant. */
#if defined(BOARD_TEMPOTEC_V1)
/* The V1 uses the same compact 30px top chrome band as its physical
 * 240x320 stock UI. Other boards retain the established 1:1/board-scaled
 * dimensions below. */
#define STATUS_BAR_CLEARANCE 30
#define TITLE_ROW_HEIGHT 30
#define HEADER_BUTTON_WIDTH 30
#else
#define STATUS_BAR_CLEARANCE BOARD_SCALE_PX(40)
#define TITLE_ROW_HEIGHT BOARD_SCALE_PX(80)
#define HEADER_BUTTON_WIDTH BOARD_SCALE_PX(64)
#endif
#define HOME_INDICATOR_BAND_HEIGHT BOARD_SCALE_PX(24)
#define HOME_INDICATOR_CONTENT_INSET (HOME_INDICATOR_BAND_HEIGHT + BOARD_SCALE_PX(8))

/* Shared touch-list row geometry -- every tappable row-of-text list
 * (Artists/Albums/Album Artist/Genres/All Songs/group-songs drill-down,
 * Files, Wi-Fi/Bluetooth scan results, Subsonic browsing) uses these same
 * dimensions for consistency. Rows use a rounded rect
 * (LIST_ROW_BG_COLOR/LIST_ROW_RADIUS) with colors from the charcoal palette
 * and support live plugin overrides. */
int32_t ui_list_row_width(void);
int32_t ui_list_row_width_wide(void);
#define LIST_ROW_WIDTH (ui_list_row_width())
/* Compatibility name used by roomier library lists. Both row-width helpers
 * follow the active display width and intentionally add no outer gutter. */
#define LIST_ROW_WIDTH_WIDE (ui_list_row_width_wide())
#define LIST_ROW_HEIGHT GUI_ROW_HEIGHT
#define MUSIC_LIST_ROW_HEIGHT GUI_MUSIC_ROW_HEIGHT
#define LIST_ROW_RADIUS BOARD_SCALE_PX(16)
#define LIST_ROW_BG_COLOR lv_color_hex(GUI_COLOR_ROW)
#define LIST_ROW_FONT app_font_22 /* see fallback_font.h -- same metrics as lv_font_montserrat_22, plus a non-Latin fallback */
#define LIST_ROW_LABEL_INSET GUI_TEXT_INSET

/* Shared lv_style_t for row-of-text lists (build_compact_list_screen,
 * show_group_songs, populate_indexed_list). Using a shared style and
 * merging row+label into a single clickable lv_label minimizes object
 * count and construction overhead for large lists. pad_top is baked in
 * to vertically center LIST_ROW_FONT text within LIST_ROW_HEIGHT.
 * Call screen_builders_init_list_row_style() once before building any
 * list screen (handled in gui_init()). */
extern lv_style_t list_row_style;
/* LV_STATE_PRESSED-only bg_color override for a list_row_style row -- attach
 * with lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED),
 * see screen_builders_init_list_row_style()'s own comment on why this is a
 * separate style rather than folded into list_row_style itself. */
extern lv_style_t list_row_pressed_style;
/* Same live, plugin-mutable bg_color/radius as list_row_style, for pill rows
 * (build_pill_list_screen()'s resized branch, add_pill_row_base()) that
 * position every child themselves via fixed-offset lv_obj_align() calls --
 * deliberately carries none of list_row_style's width/height/padding/text
 * properties, since its pad_left/pad_top would shift the content-area
 * origin lv_obj_align() aligns against, displacing those rows' labels/icons.
 * gui_plugin_set_background_color() mutates this alongside list_row_style
 * whenever the "list_row" slot changes -- see screen_builders_init_list_row_
 * style()'s own comment. */
extern lv_style_t pill_row_bg_style;
/* Minimum touch/text clearance for native rows. Explicit plugin sizing
 * removes this style rather than silently clamping the plugin's layout. */
extern lv_style_t native_row_min_style;
/* LV_STATE_PRESSED-only tap indicator for a bare lv_image icon button (no
 * background to recolor the way list_row_pressed_style does) -- dims the
 * icon via image_recolor/image_recolor_opa rather than swapping to a
 * "_s" pressed-state asset, so it works uniformly regardless of which of an
 * icon's several possible source images (play/pause, loop/random/single,
 * favorited/not) happens to be showing right now. Attach with
 * lv_obj_add_style(icon, &icon_press_style, LV_STATE_PRESSED) to any
 * LV_OBJ_FLAG_CLICKABLE image -- LVGL enters/exits LV_STATE_PRESSED on its
 * own during a touch, no event callback needed. */
extern lv_style_t icon_press_style;

/* Enables the shared constant-speed row marquee with a 2-second pause. */
void row_label_enable_marquee(lv_obj_t * label);
/* A secondary label belongs to its row, never intercepts taps, and is
 * reused with the row pool. Legacy title\nmetadata strings remain valid. */
lv_obj_t * row_label_create_subtitle(lv_obj_t * row);
void row_label_set_identity(lv_obj_t * row, lv_obj_t * subtitle_label,
                            const char * title, const char * subtitle, int32_t height);
int32_t ui_music_row_height(void);
int32_t ui_list_row_height(void);
/* Container rows shared by bounded playlist/queue pages. */
lv_obj_t * build_music_list_row(lv_obj_t * parent, const char * title, const char * subtitle,
                               int32_t trailing_space);
/* Aligns a row's trailing status label to its metadata line, or to the title
 * when the row has no subtitle. Alignment is recomputed after font changes. */
void music_list_row_align_badge(lv_obj_t * row, lv_obj_t * badge);
lv_obj_t * build_list_section(lv_obj_t * parent, const char * title);
lv_obj_t * build_list_message(lv_obj_t * parent, const char * title, const char * detail);
/* Sizes a bounded scrolling row label's own box tall enough for real glyph
 * extents (descenders on p/q/g/y, etc.) instead of the font's bare
 * lv_font_get_line_height() -- see this function's own definition
 * (screen_builders.c) for why line_height alone isn't always enough and why
 * this doesn't shift the label's existing on-screen vertical position.
 * Tags the label (LV_OBJ_FLAG_USER_3) so screen_builders_refresh_font_
 * geometry()'s one-shot walk can find and recompute this after a live font-
 * tier change swaps the stable app_font_* descriptors in place. Called from
 * configure_scrolling_row_label() (gui_plugins.c) -- the single shared
 * construction path for every native pill row and every plugin/dynamic
 * scrolling row -- never call this directly from a new call site instead of
 * going through that. */
void row_label_apply_bounded_height(lv_obj_t * label, const lv_font_t * font);
int32_t row_label_bounded_height(const lv_font_t * font);
/* Theming: shared, mutable bg_color styles for the app's other two
 * "background categories" -- attach with lv_obj_add_style(x, &style_theme_screen_bg, 0)
 * on every screen root, or &style_theme_card_bg on every popup/EQ-card/
 * slider-card. Initialized with the native charcoal palette
 * alongside list_row_style by screen_builders_init_list_row_style() below
 * -- see its own comment for why these three specifically, and why not
 * gui.c's style_accent. gui.c's gui_plugin_set_background_color() is what
 * actually mutates these later, in response to a plugin's
 * plugin.set_background_color() call. */
extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_card_bg;
/* Same live-mutable-style mechanism, for text color instead of background:
 * "primary" (the app's dominant near-white text -- labels, titles, list
 * rows) and "muted" (secondary/disabled-ish gray text -- chevrons,
 * timestamps, subtitles). Deliberately NOT covering destructive-red text
 * (delete/reset confirmations) or accent-tinted text (already tracks the
 * user's accent color pick independently) -- those are semantically fixed,
 * not part of the light/dark background split. list_row_style's own
 * text_color is a separate literal (not one of these two), since rows
 * attach list_row_style directly rather than a second style object for
 * text; gui.c's gui_plugin_set_text_color() mutates list_row_style's
 * text_color too when slot is "primary" so rows stay in sync. */
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_muted;
/* Icon-grid tiles (Home, More): a card in the theme's card color with a
 * Breeze frame outline, and its pressed state. Kept in step with the theme
 * by gui_theme_update_surface_contrast(). */
extern lv_style_t style_theme_tile;
extern lv_style_t style_theme_tile_pressed;
/* List scrollbars and every lv_switch (track and knob), in Breeze's neutral
 * colors; see gui_theme_update_surface_contrast(). */
extern lv_style_t style_theme_scrollbar;
extern lv_style_t style_theme_switch_track;
extern lv_style_t style_theme_switch_knob;
void screen_builders_init_list_row_style(void);
/* Styles the settings dropdown and its native option list. */
void gui_style_settings_dropdown(lv_obj_t * dropdown);

typedef struct {
    const char * icon_asset;          /* e.g. "launcher/music.png" */
    const char * icon_asset_selected; /* "_s" variant, e.g. "launcher/music_s.png" */
    const char * label;
    lv_event_cb_t on_click;           /* fired on LV_EVENT_CLICKED, may be NULL */
    void * user_data;

    /* ---- Optional per-tile style overrides (plugin.set_home_layout()'s
     * per-tile config table, PLUGINS.md) -- every native tile leaves these
     * false/0 (a plain compound literal without designators for trailing
     * fields already defaults them that way), reproducing today's exact
     * look. Same trailing-fields-are-safe convention pill_list_item_t's own
     * plugin-row extensions below already established. ---- */
    bool has_bg_color;   uint32_t bg_color;   /* 0xRRGGBB */
    bool has_text_color; uint32_t text_color; /* 0xRRGGBB */
    bool has_radius;     int32_t radius;      /* px corner radius */

    /* ---- Optional per-item LIST-MODE overrides, ignored entirely in tile
     * mode -- build_launcher_menu_screen()'s own per-tile counterpart to its
     * `layout` argument's shared fields (plugin.set_home_layout(), PLUGINS.md).
     * Unset (false/0/NULL) means "use `layout`'s own shared value for this
     * field instead", same convention has_bg_color/has_text_color/has_radius
     * above already use. Music/Stream Media/Wireless (launcher_layout_config's
     * screens) leave every one of these unset -- their own layout is
     * genuinely uniform across every tile, so `layout` alone is enough.
     * Only Home's build_home_screen() (gui_settings.c), whose per-tile
     * plugin.set_home_layout() overrides can legitimately differ tile by
     * tile, ever sets these. ---- */
    bool has_row_height; int32_t row_height;
    bool has_row_width;  int32_t row_width;
    bool has_accessory;  bool accessory;
    const char * text_size;  /* non-NULL overrides layout's own text_size */
    const char * text_align; /* non-NULL overrides layout's own align */
    bool has_icon; bool icon; /* whether THIS item's icon_asset should render at all */

    /* Optional per-tile/category-row background image (theme2-relative,
     * e.g. "launcher/bg_music.png"). NULL reproduces the plain row/tile.
     * Drawn via bg_image_src rather than bg_color -- the asset itself
     * already bakes in the rounded-rect card shape (as alpha) and the
     * accent-tinted radial glow, so no separate radius/clip_corner styling
     * is needed here. */
    const char * bg_image;
    /* Optional compact, transparent glow centered on the icon (0 = none).
     * Unlike bg_image this has no card surface or visible tile boundary. */
    uint32_t icon_glow_color;
    /* Category menus only: plugin icon path (absolute or relative to .plugins),
     * resolved with the same rules as register_list_item options.icon. */
    const char * plugin_icon;
} icon_grid_item_t;

/* Titled screen: real back-arrow button (top-left, invokes back_btn_cb) and
 * a flex-wrap grid of icon tiles below, each swapping to its "_s" asset
 * while pressed for visual feedback. Caller owns navigation wiring
 * (nav_pop/nav_push, swipe gestures) via finalize_screen_navigation() after
 * this returns. icon_scale_percent scales ICON_GRID_TARGET_ICON_PX for this
 * screen only (100 = the shared default every other icon-grid screen uses;
 * pass a value above/below 100 to make just this screen's icons bigger or
 * smaller without affecting the others).
 *
 * label_inside_icon: false draws the caption below the icon in its own
 * reserved row (used for plain glyph-on-transparent assets). true (used for
 * Wireless cards) draws the caption inside the icon's own image near its bottom,
 * as those card assets have a built-in caption band at the bottom. */
/* tile_gap: px of visible space between adjacent tiles (0 = today's exact
 * flush-cell look with its thin divider lines -- every native caller passes
 * 0). A positive value insets each tile within its own already-computed grid
 * cell via a real LVGL margin (tile_gap/2 on every side) rather than
 * touching the row-height grid math at all, and suppresses the flush-cell
 * divider lines (redundant, and visually conflicting, once real gaps exist).
 * Only plugin.set_home_layout()'s options.tile_gap (PLUGINS.md, tile mode)
 * ever passes a nonzero value. */
lv_obj_t * build_icon_grid_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const icon_grid_item_t * items, int item_count,
                                   int32_t icon_scale_percent, bool label_inside_icon, int32_t tile_gap);

/* Top-right counterpart to build_header_back_button() --
 * same 64x64 transparent hitbox, centered within the title band,
 * so any caller's icon lands at exactly the same visual level as the screen's
 * own back arrow, mirrored to the right edge instead of the left. icon_asset
 * is passed straight to lv_image_set_src() (caller resolves it via
 * asset_path()/asset_path_plain() first, same as every other icon in this
 * codebase). click_cb may be NULL for a purely decorative icon. */
lv_obj_t * build_top_right_icon_button(lv_obj_t * scr, const char * icon_asset, lv_event_cb_t click_cb);
/* For title-bar images built by hand: the Breeze replacement for a resolved
 * asset path, or NULL when there is none (keep the asset and its styling). */
const char * titlebar_breeze_icon(const char * asset);
/* Text color, or the accent while "Tint title bar icons" is on. */
void style_titlebar_breeze_icon(lv_obj_t * image);
/* Drag handle art for reorder rows: pre-tinted Breeze handle-sort, else the
 * stock grip in the text color. */
void set_reorder_handle_icon(lv_obj_t * grip);
/* Resolved path of a white Breeze glyph's pre-tinted variant matching the
 * theme text color (<name>.png or <name>_dark.png). */
const char * text_glyph_asset(const char * relative);
/* breeze_rows/<folder>_<name>.png for a stock row icon, when present and the
 * active theme does not supply the stock image itself. */
bool breeze_row_icon(const char * stock, char * out, size_t out_size);
/* Shared back control for screens that retain its handle to toggle visibility. */
lv_obj_t * build_header_back_button(lv_obj_t * scr, lv_event_cb_t cb);
/* Align existing text actions/switches by their actual height, not an
 * assumed font or asset size. Position remains centered when size changes. */
void align_screen_header_action(lv_obj_t * action, int32_t right_inset);
/* Top-right refresh action with a generous header hitbox. Busy state spins
 * the glyph, greys out the action, and blocks taps until refresh completes. */
lv_obj_t * build_header_refresh_action(lv_obj_t * scr, lv_event_cb_t click_cb);
void set_header_refresh_action_busy(lv_obj_t * button, bool busy);
/* Fits a header title built by build_screen_header() to its single line
 * (smaller font, then ellipsis). Call again after changing its width. */
void screen_title_fit(lv_obj_t * label);
/* Reserve the title width up to a trailing action and fit it on one line. */
void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon);

/* Returns the title label. Optional trailing action reserves its hitbox. */
lv_obj_t * build_screen_header(lv_obj_t * scr, const char * title, lv_event_cb_t back_cb,
                              const char * trailing_asset, lv_event_cb_t trailing_cb);

typedef enum {
    PILL_ACCESSORY_NONE = 0,
    PILL_ACCESSORY_CHEVRON,
    PILL_ACCESSORY_TOGGLE,  /* real lv_switch, standardized to match the Settings screen's own switches */
} pill_accessory_type_t;

/* Creates a non-interactive navigation chevron aligned to the parent's
 * right-middle edge, recolored to the current muted theme text color unless
 * a per-item color override is supplied. A per-item color override is used for
 * plugin-defined rows; otherwise the live muted text color is used. */
lv_obj_t * build_navigation_chevron(lv_obj_t * parent, int32_t right_inset,
                                    bool has_color_override, uint32_t color_override);

typedef struct {
    const char * label;
    pill_accessory_type_t accessory;
    bool toggle_initial_state;      /* only used when accessory == PILL_ACCESSORY_TOGGLE */
    lv_event_cb_t on_click;         /* fired on row LV_EVENT_CLICKED (NONE/CHEVRON rows) */
    lv_event_cb_t on_toggle_change; /* fired on LV_EVENT_VALUE_CHANGED (TOGGLE rows) */
    void * user_data;
    /* Optional (NULL if not needed): on a PILL_ACCESSORY_TOGGLE row,
     * *out_toggle_img is set to the toggle's own lv_switch once built,
     * allowing callers to update its LV_STATE_CHECKED state later from
     * elsewhere (e.g. settings_crossfade_toggle_img in gui_settings.c). */
    lv_obj_t ** out_toggle_img;

    /* ---- Plugin-row extensions (plugin.register_list_item()'s optional
     * `options` table, PLUGINS.md) -- every existing native row across every
     * build_pill_list_screen() caller leaves these NULL/0 (a plain compound
     * literal without designators for trailing fields already defaults them
     * that way), selecting native rounded surfaces, font-aware 96px
     * minimum rows and app_font_20 labels. Only a
     * plugin-appended row (gui.c's build_settings_*_screen() plugin-row
     * loops) ever sets these. ---- */

    /* Raw absolute filesystem path (e.g. "/data/mnt/sd_0/.plugins/icon.png"),
     * NOT a theme2-relative asset path -- a plugin's icon lives on the SD
     * card, not in this app's own theme directory. See pill_row_apply_icon()
     * below. NULL = no icon, today's exact layout. */
    const char * icon_asset;

    /* 0 = native font-aware density. A non-zero value is clamped to
     * PILL_ROW_HEIGHT_MIN..MAX and preserves explicit plugin sizing. */
    int32_t row_height;

    /* 0 = the native display width minus gutters. Non-zero values use the
     * plugin-safe range below and keep the row centered in its list. */
    int32_t row_width;

    /* NULL = this call site's own default (native rows: app_font_20,
     * unchanged; plugin rows: gui.c always supplies a non-NULL default
     * before this reaches here -- see pill_row_resolve_text_size()'s own
     * comment). "small"/"medium"/"large" -> app_font_16/22/28
     * (fallback_font.h) -- the fallback-capable family, since plugin row
     * text (unlike native chrome) may be non-Latin. Validated by the Lua
     * boundary in plugin_manager.c; this layer trusts it's already one of
     * the three recognized values or NULL. */
    const char * text_size;

    /* ---- Optional per-tile style overrides, same fields/meaning as
     * icon_grid_item_t's own (plugin.set_home_layout()'s per-tile config
     * table, PLUGINS.md) -- every existing row leaves these false/0,
     * reproducing today's exact rendering. When has_bg_color/has_radius is
     * set, the row is forced onto the same plain rounded-rect fill path
     * row_height/row_width already use instead of the PNG pill sprite (a
     * raster asset can't be recolored), using these values in place of
     * LIST_ROW_BG_COLOR/LIST_ROW_RADIUS. ---- */
    bool has_bg_color;   uint32_t bg_color;
    bool has_text_color; uint32_t text_color;
    bool has_radius;     int32_t radius;

    /* NULL/"left" (default) = today's exact layout (label left-aligned,
     * starting right after any icon -- unaffected by this field). "center"/
     * "right" re-align the label alone within the row's own remaining width;
     * pill_row_apply_icon()'s icon placement is never touched by this.
     * plugin.set_home_layout() only, list mode only (PLUGINS.md) -- no other
     * caller sets this. */
    const char * text_align;
    lv_obj_t ** out_row;
} pill_list_item_t;

/* Row-height bounds for a plugin-resized pill row (register_list_item()'s
 * `height` option), a plugin-resized show_list() row (gui_plugin_show_list()'s
 * own `height` option), or a plugin.set_home_layout() list-mode tile
 * (PLUGINS.md) -- shared range for all three, silently clamped rather than
 * erroring (same convention plugin.set_interval()'s own minimum-clamp uses).
 * Values below PILL_ROW_ICON_PX_DEFAULT (64px) allow compact rows
 * (e.g. set_home_layout() list tiles). If a row has both an icon and a height
 * below 64px, the icon will be clipped to row bounds; rows without icons
 * are unaffected. PILL_ROW_HEIGHT_MAX prevents a single row from dominating
 * the screen. */
#define PILL_ROW_HEIGHT_MIN 48
#define PILL_ROW_HEIGHT_MAX 220
#define PILL_ROW_WIDTH_MIN 240
#define PILL_ROW_WIDTH_MAX (ui_list_row_width_wide())

/* Native width derived from the active display. */
int32_t pill_row_default_width(void);

/* On-screen icon footprint (longest edge, in px) a plugin row's icon targets
 * -- same scaling formula as build_icon_grid_screen()'s own
 * ICON_GRID_TARGET_ICON_PX, just a smaller target since this sits inside a
 * single row rather than a whole tile. Fits the native 96px row with
 * 10px clearance above and below. */
#define PILL_ROW_ICON_PX_DEFAULT 64

/* Adds a plugin-supplied icon to an already-built row and re-aligns `label`
 * to start after it -- shared by build_pill_list_screen() below (native
 * pill rows) and gui.c's own plugin-row builders (add_pill_row_base()'s
 * pill-toggle/-chevron rows, the settings-list slider row, gui_plugin_
 * show_list()'s icon rows), so the icon-loading/scaling logic exists in
 * exactly one place rather than four. No-op if icon_path is NULL --
 * `label`'s alignment is left completely untouched in that case, preserving
 * every existing call site's exact layout. `icon_px` is the on-screen
 * target size (longest edge); pass PILL_ROW_ICON_PX_DEFAULT unless a caller
 * has a specific reason not to. `align`/`x`/`y` are the alignment to apply
 * to BOTH the icon and (offset by icon_px + a 12px gap) the label -- most
 * rows are a single vertically-centered line (LV_ALIGN_LEFT_MID), but the
 * settings-list slider row's label sits top-left instead (its value label
 * and slider fill the rest of the card), so this isn't hardcoded. See this
 * file's own top-of-block comment for why icon_path is a raw filesystem
 * path, not a theme2-relative one. */
void pill_row_apply_icon(lv_obj_t * row, lv_obj_t * label, const char * icon_path, int32_t icon_px,
                          lv_align_t align, int32_t x, int32_t y);

/* Resolves a plugin's chosen text-size tier to the correspondingly-sized
 * fallback-capable font -- NULL returns app_font_20 (build_pill_list_screen()'s
 * own native-row default; every plugin call site is responsible for
 * supplying its OWN non-NULL default -- e.g. "medium" -- before calling
 * this, precisely so NULL unambiguously means "a genuine native row" here,
 * never "a plugin row that happened not to set text_size"). "small"/
 * "medium"/"large" -> &app_font_16/22/28 (fallback_font.h). Any other
 * string (shouldn't happen -- plugin_manager.c validates at the Lua
 * boundary) falls back to app_font_20 rather than dereferencing garbage. */
const lv_font_t * pill_row_resolve_text_size(const char * text_size);

/* Titled screen: real back-arrow button and a vertically scrollable list of
 * touch_list/item_bg.png pill rows, each with a label and an optional
 * right-side chevron or toggle. toggle_accent_style is applied (via
 * lv_obj_add_style(), not read as a plain value) to every PILL_ACCESSORY_
 * TOGGLE row's own lv_switch (LV_PART_INDICATOR|LV_STATE_CHECKED). The
 * caller owns the style object's lifetime; screen_builders.c only attaches
 * the style and never reads its properties directly. */
/* row_gap: vertical spacing between rows, in px (every existing caller
 * passes 6, today's exact hardcoded value -- see build_pill_list_screen()'s
 * own history). Only plugin.set_home_layout()'s options.row_gap
 * (PLUGINS.md, list mode) ever passes anything else. */
/* icon_scale_pct: scales every row's own icon (item->icon_asset) the same
 * way build_icon_grid_screen()'s icon_scale_percent scales a tile's icon --
 * 100 = PILL_ROW_ICON_PX_DEFAULT (64px), unchanged from before this
 * parameter existed. Every native call site passes 100. Only build_launcher_
 * menu_screen()'s list-mode branch (plugin.set_home_layout(), PLUGINS.md)
 * ever passes anything else, so its own icon_scale_pct argument no longer
 * gets silently dropped when list_mode is set. */
lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const pill_list_item_t * items, int item_count,
                                   lv_style_t * toggle_accent_style, int32_t row_gap,
                                   int32_t icon_scale_pct);

/* Function pointer signatures matching plugin_manager_get_<target>_list_item_* accessors. */
typedef int (*plugin_list_item_count_cb_t)(void);
typedef const char * (*plugin_list_item_label_cb_t)(int index);
typedef void (*plugin_list_item_options_cb_t)(int index, const char ** out_icon, int32_t * out_height,
                                              int32_t * out_width, const char ** out_text_size);

/* Appends up to max_items plugin-provided rows to items[] starting at index count,
 * querying row metadata via the given get_count/get_label/get_options accessors and
 * routing row clicks to click_cb with (void *)(intptr_t)index as user_data.
 * Preserves the standard "medium" text_size fallback and chevron accessory styling.
 * Returns the updated item count (count + rows appended). */
int append_plugin_list_rows(pill_list_item_t * items, int count, int max_items,
                            plugin_list_item_count_cb_t get_count_fn,
                            plugin_list_item_label_cb_t get_label_fn,
                            plugin_list_item_options_cb_t get_options_fn,
                            lv_event_cb_t click_cb);

lv_obj_t * build_launcher_menu_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const icon_grid_item_t * items, int item_count,
                                      int icon_scale_pct, bool label_inside_icon,
                                      const launcher_menu_layout_t * layout);

/* Adds the Settings-style 44px icon and stretched pill background to an
 * already-built native pill row. Both paths are theme-relative. Identical
 * paths share one retained decoded buffer across rows/screens; it is released
 * when the last referencing image is deleted. */
void decorate_category_row(lv_obj_t * row, const char * icon_asset, const char * bg_asset);
/* Same native category look for a plugin-supplied icon: absolute path or
 * relative to <SD>/.plugins/ (pill_row_apply_icon()). NULL or a path that
 * cannot be used leaves the row as a plain decorated row without icon. */
void decorate_category_row_plugin_icon(lv_obj_t * row, const char * plugin_icon);
/* Adds a passive accent-colored Play All marker to an existing row and
 * returns its circle so pooled label rows can realign it after padding changes. */
lv_obj_t * decorate_play_all_row(lv_obj_t * row);

/* Builds a Settings-style vertical category menu with the shared font-aware
 * native row height. Explicit item/layout overrides remain authoritative.
 * icon_asset/bg_image paths are theme-relative. */
lv_obj_t * build_category_menu_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const icon_grid_item_t * items, int item_count,
                                      const launcher_menu_layout_t * layout);

/* Recomputes the shared list padding, any icon-caption coordinates, and
 * every bounded scrolling row label's box height (see row_label_apply_
 * bounded_height()'s own comment) under root after the stable app_font_*
 * descriptors change metrics. A NULL root updates only the shared styles
 * (native_row_min_style, list_row_style). It does not walk beyond the
 * root passed in -- use screen_builders_refresh_all_font_geometry() when
 * every live screen must move, not just the nav stack. */
void screen_builders_refresh_font_geometry(lv_obj_t * root);
/* Font Size / Custom Font live apply: shared styles, then every screen the
 * display still owns. Settings submenus that were never on the nav stack
 * (Music Settings, Power, Playback, ...) keep USER_3 label boxes from the
 * previous tier unless this runs; LV_LABEL_LONG_SCROLL_CIRCULAR then rolls
 * those titles vertically when the submenu is first opened. */
void screen_builders_refresh_all_font_geometry(void);
lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list);
/* Shared cover cards for plugin supplied grids and the plugin store. The
 * caller resolves image paths to an LVGL source (for example, "S:/path"). */
void configure_cover_card_grid(lv_obj_t * list, int columns);
lv_obj_t * add_cover_card(lv_obj_t * list, const char * label,
                          const char * resolved_image_src, int columns,
                          lv_event_cb_t on_click, void * user_data);
/* Shared modal surface and full-width actions; callbacks remain owned by callers. */
lv_obj_t * build_popup_surface(lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);
void style_popup_action_row(lv_obj_t * row);

lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode,
                               lv_obj_t ** out_title, const char * body_text, const char * confirm_text,
                               lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row,
                               const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                               lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);

/* Bottom-sheet Power Hub. action_cb is attached to each full card/Cancel
 * surface; userdata indices are poweroff, restart, screenoff, sleep timer,
 * and cancel (0..4). out_actions receives those five tappable surfaces. */
lv_obj_t * build_power_hub_panel(lv_obj_t * parent, lv_event_cb_t action_cb,
                                 lv_obj_t ** out_status, lv_obj_t ** out_actions);
lv_obj_t * build_confirm_popup_with_labels(const char * title_text, lv_label_long_mode_t title_long_mode,
                                           lv_obj_t ** out_title, const char * body_text, lv_obj_t ** out_body,
                                           const char * confirm_text, lv_obj_t ** out_confirm_label,
                                           lv_color_t confirm_color, lv_event_cb_t confirm_cb,
                                           lv_obj_t ** out_confirm_row, const char * cancel_text,
                                           lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                                           lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb,
                                           lv_obj_t ** out_backdrop);

typedef struct {
    lv_obj_t * popup;
    lv_obj_t * backdrop;
    bool visible;
} gui_popup_t;

void gui_popup_show(gui_popup_t * p);
void gui_popup_hide(gui_popup_t * p);
void gui_popup_teardown(gui_popup_t * p);
bool gui_popup_is_visible(const gui_popup_t * p);
unsigned gui_popup_visible_count(void);
/* Full-width tappable action (Install, Remove, Cancel) at the end of a popup. */
lv_obj_t * add_popup_action_row(lv_obj_t * popup, const char * text, lv_color_t color,
                                lv_event_cb_t cb, void * user_data);
/* A popup surface with a centered title and, when body_out is given, a
 * scrolling description under it. Action rows are added by the caller. */
void build_titled_popup(gui_popup_t * popup, lv_event_cb_t backdrop_cb, lv_obj_t ** title_out,
                        lv_obj_t ** body_out);

lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * label_text);
lv_obj_t * add_pill_summary_label(lv_obj_t * row);
lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * label_text, lv_event_cb_t on_click);
lv_obj_t * add_pill_option_row(lv_obj_t * parent, const char * label_text, bool selected,
                              lv_event_cb_t on_click, void * user_data);
lv_obj_t * add_section_header(lv_obj_t * parent, const char * text);
/* Accent-bordered 24 px square, filled with a check mark when checked; not clickable. */
lv_obj_t * build_check_box(lv_obj_t * parent, bool checked);

/* Radial accent glow behind an icon -- same technique the Home screen's
 * tiles use (add_icon_glow's own doc comment there has the full mechanics):
 * bakes a small ARGB8888 blob once and draws it centered on `wrapper`
 * during `tile`'s own LV_EVENT_DRAW_MAIN, so it composites correctly with
 * screen transitions like an ordinary image. `color` is 0xRRGGBB (0 is a
 * no-op); `diameter` is the glow bitmap's own size in pixels, not the
 * icon's -- make it comfortably larger than the icon for it to read as a
 * glow rather than a tight outline. `tile` owns the glow's lifetime (freed
 * on the tile's own LV_EVENT_DELETE), so it doesn't need a separate
 * teardown call as long as `tile` itself gets deleted normally. */
void add_icon_glow(lv_obj_t * tile, lv_obj_t * wrapper, uint32_t color, int32_t diameter);

int find_nearest_step_index(const int * steps, int count, int value);

/* Builds the shared "rounded card + slider + centered value label" widget
 * tree used by several settings screens (screen timeout, screen dimming,
 * startup volume, sleep timer, idle shutdown), including the gesture-bubble
 * removal and swipe dead zone registration every one of them repeats.
 * track_top_offset lets a caller that adds its own caption label above the
 * slider (idle shutdown) push the slider down within the card. Does NOT set
 * the value label's initial text (callers use different formats) or the
 * card's hidden flag (callers use different enabled conditions) -- set both
 * via the returned card / *out_value_label after the call. */
lv_obj_t * build_setting_slider_card(lv_obj_t * parent, lv_obj_t * align_target, int32_t card_height,
                                      int32_t track_top_offset, int32_t min_range, int32_t max_range,
                                      int32_t initial_val, lv_event_cb_t slider_cb,
                                      lv_obj_t ** out_slider, lv_obj_t ** out_value_label);

lv_obj_t * build_font_change_mask(int target);


/* Plain native wide row; chevron is used by saved-list navigation rows. */
lv_obj_t * build_wide_list_row(lv_obj_t * parent, const char * label_text, bool chevron);

#endif /* SCREEN_BUILDERS_H */
