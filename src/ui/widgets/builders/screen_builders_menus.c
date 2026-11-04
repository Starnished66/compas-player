#include "screen_builders.h"
#include "screen_builders_internal.h"
#include "../../plugins/gui_plugins.h"
#include "../../shell/gui_navigation.h"
#include "../../assets/assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Menu screens: the Home icon grid, pill lists, launcher/category/Subsonic
 * lists and their row decoration. */

#define ICON_GRID_ICON_LABEL_GAP 4
#define ICON_GRID_CARD_GAP BOARD_SCALE_PX(16)

/* Tile inner padding in px -- a named constant since the manual
 * absolute-positioning math below needs the exact same number used by the
 * tile's own pad_all style. */
#define ICON_GRID_TILE_PAD BOARD_SCALE_PX(8)

/* Positions a tile's icon wrapper and caption label, centred as one block in
 * `available_h`. A one-line caption (a scrolling label, the V1's) sits a
 * larger gap below the icon; otherwise the caption is bounded at two lines
 * so a wrapping caption cannot run into the tile below. */
static void icon_tile_place_caption(lv_obj_t * img_wrap, lv_obj_t * label, int32_t icon_h,
                                    int32_t available_h) {
    bool one_line = lv_label_get_long_mode(label) == LV_LABEL_LONG_SCROLL_CIRCULAR;
    int32_t line_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY));
    int32_t label_h = one_line ? line_h : line_h * 2;
    int32_t label_gap = one_line ? BOARD_SCALE_PX(12) : ICON_GRID_ICON_LABEL_GAP;
    int32_t content_h = icon_h + label_gap + label_h;
    int32_t top = (available_h - content_h) / 2;
    if (top < 0) top = 0;
    lv_obj_set_height(label, label_h);
    lv_obj_align(img_wrap, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, top + icon_h + label_gap);
}

/* An icon-grid tile (USER_1): re-places its icon wrapper and caption. */
void icon_grid_refresh_tile(lv_obj_t * tile) {
    if (lv_obj_get_child_count(tile) < 2) return;
    lv_obj_t * img_wrap = lv_obj_get_child(tile, 0);
    lv_obj_t * label = lv_obj_get_child(tile, 1);
    if (lv_obj_check_type(label, &lv_label_class) &&
        lv_obj_get_child_count(img_wrap) == 1 &&
        lv_obj_check_type(lv_obj_get_child(img_wrap, 0), &lv_image_class)) {
        int32_t icon_h = lv_obj_get_height(img_wrap);
        /* The same area the build centred in: the tile's height inside its padding, not its
         * content height, which would also drop the card border and shift the block up. */
        icon_tile_place_caption(img_wrap, label, icon_h,
                                lv_obj_get_height(tile) - 2 * ICON_GRID_TILE_PAD);
    }
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

/* The grid container of a header-less icon grid: 2 equal columns, `row_count`
 * rows of `row_h` px, at `top` below the screen top. NULL when the track
 * descriptor cannot be allocated. */
static lv_obj_t * icon_grid_container(lv_obj_t * scr, int row_count, int32_t row_h, int32_t top,
                                      int32_t tile_gap) {
    /* Allocated per-grid because lv_obj_set_grid_dsc_array() stores the pointer.
     * The owning grid frees this context on LV_EVENT_DELETE so soft reloads do not leak. */
    icon_grid_dsc_ctx_t * grid_dsc = lv_malloc(sizeof(*grid_dsc));
    if (!grid_dsc) return NULL;
    int32_t * col_dsc = grid_dsc->cols;
    col_dsc[0] = LV_GRID_FR(1);
    col_dsc[1] = LV_GRID_FR(1);
    col_dsc[2] = LV_GRID_TEMPLATE_LAST;

    int32_t * row_dsc = grid_dsc->rows;
    for (int r = 0; r < row_count && r < ICON_GRID_MAX_ROWS; r++) row_dsc[r] = row_h;
    row_dsc[row_count < ICON_GRID_MAX_ROWS ? row_count : ICON_GRID_MAX_ROWS] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t * grid = lv_obj_create(scr);
    lv_obj_add_event_cb(grid, lv_heap_ctx_delete_cb, LV_EVENT_DELETE, grid_dsc);
    lv_obj_set_size(grid, lv_pct(100), row_count * row_h);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, top);
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
    return grid;
}

/* A tile's surface: the theme card, or the item's own color; its press
 * feedback, padding and tap target. */
static void icon_grid_style_tile(lv_obj_t * tile, const icon_grid_item_t * item) {
    if (!item->has_bg_color) {
        /* Native tiles are theme cards. */
        lv_obj_add_style(tile, &style_theme_tile, 0);
        lv_obj_add_style(tile, &style_theme_tile_pressed, LV_STATE_PRESSED);
    } else {
        /* A tile a theme paints itself keeps its own opaque color, with a 25%
         * press tint. */
        const lv_opa_t press_opa = 64; /* 25% of 255 */
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(item->bg_color), 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(tile, lv_color_mix(lv_color_hex(GUI_COLOR_PRESSED),
                                                     lv_color_hex(item->bg_color), press_opa),
                                  LV_STATE_PRESSED);
    }
    if (item->has_radius) lv_obj_set_style_radius(tile, item->radius, 0);
    lv_obj_set_style_pad_all(tile, ICON_GRID_TILE_PAD, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
}

/* A tile's icon and caption, positioned with explicit pixel offsets rather
 * than flex auto-centering for stable rendering across layout passes, and the
 * pressed-icon swap. `available_h` is the tile's height inside its padding. */
static void icon_grid_tile_content(lv_obj_t * tile, const icon_grid_item_t * item,
                                   int32_t target_icon_px, int32_t available_h) {
    /* Category icons are natively 100px, the fallback when the header
     * cannot be read. */
    int32_t target_icon_h;
    lv_obj_t * img_wrap = scaled_icon_create(tile, asset_path(item->icon_asset), target_icon_px,
                                             100, &target_icon_h);
    lv_obj_t * img = lv_obj_get_child(img_wrap, 0);

    lv_obj_t * label = lv_label_create(tile);
    lv_label_set_text(label, item->label);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    if (item->has_text_color) lv_obj_set_style_text_color(label, lv_color_hex(item->text_color), 0);
    /* Wrap label text within the tile width to prevent overflow into adjacent tiles. */
    lv_obj_set_width(label, lv_pct(100));
#if defined(BOARD_TEMPOTEC_V1)
    row_label_enable_marquee(label);
#else
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
#endif
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    icon_tile_place_caption(img_wrap, label, target_icon_h, available_h);

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
}

lv_obj_t * build_icon_grid_screen(const icon_grid_item_t * items, int item_count,
                                   int32_t icon_scale_percent, int32_t tile_gap) {
    /* Clamped here, not just left to whatever the caller passed --
     * plugin.set_home_layout()'s tile_gap (PLUGINS.md) is plugin-controlled
     * and otherwise unbounded all the way from the Lua boundary. Unlike
     * build_pill_list_screen()'s row_gap (plain pad_gap spacing), this one
     * is subtracted straight into `available_h` below (icon/label vertical
     * budget) -- a large enough value drives that negative and produces
     * broken tile content positioning, not just a wide gap, so it needs a
     * real ceiling rather than a cosmetic one. */
    if (tile_gap > BOARD_SCALE_PX(64)) tile_gap = BOARD_SCALE_PX(64);
    /* Floored at the card gap: cards need that much room around and between them. */
    if (tile_gap < ICON_GRID_CARD_GAP) tile_gap = ICON_GRID_CARD_GAP;

    int32_t target_icon_px = (BOARD_SCALE_PX(ICON_GRID_TARGET_ICON_PX) * icon_scale_percent) / 100;

    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* 2-column grid with equal-sized cells and divider lines between them. */
    int col_count = 2;
    int row_count = (item_count + col_count - 1) / col_count;

    /* Grid height is screen height minus the status bar clearance (the grid
     * has no title row). */
    int32_t scr_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t header_h = STATUS_BAR_CLEARANCE;

    /* Rows use fixed pixel height based on ICON_GRID_REFERENCE_ROWS rather than
     * fractional shares, so cell heights remain consistent regardless of row count. */
    int32_t row_h = (scr_h - header_h) / ICON_GRID_REFERENCE_ROWS;

    lv_obj_t * grid = icon_grid_container(scr, row_count, row_h, header_h, tile_gap);
    if (!grid) {
        fprintf(stderr, "[screen_builders] out of memory building icon grid\n");
        return scr; /* valid header-only screen is safer than a NULL screen */
    }

    /* tile_gap shrinks the tile's own STRETCH-computed height by that
     * much (its margin_top+margin_bottom) below row_h -- see the
     * tile_gap margin comment below -- so it has to come off here too,
     * or icon/label placement would overflow the tile's real (now
     * smaller) bottom edge. The refresh walk measures the same height from the
     * tile itself. Both centre in the padding box, which in card mode sits one
     * border width below the content centre -- the look the grid has always had. */
    int32_t available_h = row_h - 2 * (tile_gap / 2) - 2 * ICON_GRID_TILE_PAD;

    for (int i = 0; i < item_count; i++) {
        const icon_grid_item_t * item = &items[i];

        lv_obj_t * tile = lv_obj_create(grid);
        /* USER_1 is a zero-allocation marker used by the one-shot font
         * geometry refresh; unlike a per-tile context it adds no
         * steady-state heap or callback overhead. */
        lv_obj_add_flag(tile, LV_OBJ_FLAG_USER_1);
        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, i % col_count, 1,
                             LV_GRID_ALIGN_STRETCH, i / col_count, 1);
        /* Margins visually inset the tile within its stretched grid cell,
         * with tile_gap / 2 per side so adjacent tiles have a total gap of tile_gap. */
        lv_obj_set_style_margin_all(tile, tile_gap / 2, 0);
        icon_grid_style_tile(tile, item);
        icon_grid_tile_content(tile, item, target_icon_px, available_h);

        if (item->on_click) lv_obj_add_event_cb(tile, item->on_click, LV_EVENT_CLICKED, item->user_data);
    }

    return scr;
}

/* Row tap on a toggle row: flips the row's own lv_switch (the event's user
 * data), which is a real lv_switch, not an image. */
static void pill_toggle_row_event_cb(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_user_data(e);

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

/* The scrolling column under a header: a transparent container filling the
 * screen below `header_h` and above `bottom_inset`, rows centred across its
 * width with `row_gap` between them. The system home-indicator strip sits
 * above the bottom of every screen, so a small scrollable tail lets the last
 * row move fully clear of that overlay. */
static lv_obj_t * screen_list_body(lv_obj_t * scr, int32_t header_h, int32_t bottom_inset,
                                   int32_t row_gap) {
    int32_t display_h = lv_display_get_vertical_resolution(lv_display_get_default());
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
    lv_obj_set_style_pad_bottom(list, BOARD_SCALE_PX(8), 0);
    return list;
}

/* A row's size: the native density (font-aware) unless the item sets
 * explicit plugin dimensions, which are clamped to the shared plugin range. */
static void pill_row_size(const pill_list_item_t * item, int32_t * out_width, int32_t * out_height) {
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
    *out_width = width;
    *out_height = height;
}

/* A row's label, with its optional plugin icon, bounded to leave room for the
 * row's accessory. */
static void pill_row_add_label(lv_obj_t * row, const pill_list_item_t * item, int32_t width,
                               int32_t icon_px) {
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, item->label);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    /* text_size NULL (every native row) -> gui_theme_font(GUI_FONT_ROLE_BODY), unchanged from
     * before this field existed -- see pill_row_resolve_text_size()'s own comment. */
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
}

/* A row's accessory (toggle switch or chevron) and its tap handling. */
static void pill_row_add_accessory(lv_obj_t * row, const pill_list_item_t * item,
                                   lv_style_t * toggle_accent_style) {
    if (item->accessory == PILL_ACCESSORY_TOGGLE) {
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        /* Non-clickable switch visual driven by row tap (row handles the click event). */
        lv_obj_t * toggle_sw = lv_switch_create(row);
        lv_obj_remove_flag(toggle_sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(toggle_sw, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(20), 0);
        if (item->toggle_initial_state) lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
        if (toggle_accent_style) lv_obj_add_style(toggle_sw, toggle_accent_style, LV_PART_INDICATOR | LV_STATE_CHECKED);

        if (item->out_toggle_img) *item->out_toggle_img = toggle_sw;

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, pill_toggle_row_event_cb, LV_EVENT_CLICKED, toggle_sw);

        if (item->on_toggle_change) {
            lv_obj_add_event_cb(toggle_sw, item->on_toggle_change, LV_EVENT_VALUE_CHANGED, item->user_data);
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

/* One pill row of `list`; returns its height. */
static int32_t pill_list_add_row(lv_obj_t * list, const pill_list_item_t * item, int32_t icon_px,
                                 lv_style_t * toggle_accent_style) {
    int32_t width, height;
    pill_row_size(item, &width, &height);

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

    pill_row_add_label(row, item, width, icon_px);
    pill_row_add_accessory(row, item, toggle_accent_style);
    return height;
}

lv_obj_t * build_pill_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                   const pill_list_item_t * items, int item_count,
                                   lv_style_t * toggle_accent_style, int32_t row_gap,
                                   int32_t icon_scale_pct) {
    /* Same scale-to-target-px formula as the icon grid's own target_icon_px --
     * 100 reproduces PILL_ROW_ICON_PX_DEFAULT exactly, unchanged from
     * before this parameter existed. */
    int32_t icon_px = (BOARD_SCALE_PX(PILL_ROW_ICON_PX_DEFAULT) * icon_scale_pct) / 100;

    /* Clamped here, not just left to whatever the caller passed --
     * plugin.set_home_layout()'s row_gap (PLUGINS.md) is plugin-controlled
     * and otherwise unbounded all the way from the Lua boundary. A
     * pathological value here is only a layout-quality issue (pad_gap, not
     * subtracted from any available-space math the way tile_gap is in the
     * icon grid), but there's no legitimate reason to want more than a
     * native row's own height in gap, so bound it there. */
    if (row_gap < 0) row_gap = 0;
    if (row_gap > LIST_ROW_HEIGHT) row_gap = LIST_ROW_HEIGHT;

    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* NULL back_btn_cb omits the back arrow and NULL title the title label
     * (build_screen_header() skips each); Home's list mode passes neither. */
    build_screen_header(scr, title, back_btn_cb, NULL, NULL);

    /* Only reserve the title-row band when a title label or a back button is
     * drawn into it. */
    int32_t header_h = STATUS_BAR_CLEARANCE + ((title || back_btn_cb) ? TITLE_ROW_HEIGHT : 0);
    int32_t display_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t bottom_inset = (!title && !back_btn_cb) ? 0 : HOME_INDICATOR_CONTENT_INSET;

    lv_obj_t * list = screen_list_body(scr, header_h, bottom_inset, row_gap);

    int32_t content_h = item_count > 0 ? (item_count - 1) * row_gap : 0;
    for (int i = 0; i < item_count; i++)
        content_h += pill_list_add_row(list, &items[i], icon_px, toggle_accent_style);

    /* Headerless Home lists should occupy the actual Home viewport, ending
     * within its full viewport. Keep START and the native pads
     * for oversized lists so their first row remains reachable by scrolling. */
    int32_t list_h = display_h - header_h;
    if (!title && !back_btn_cb && content_h <= list_h) {
        lv_obj_set_style_pad_top(list, 0, 0);
        lv_obj_set_style_pad_bottom(list, 0, 0);
        lv_obj_set_flex_align(list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    return scr;
}

/* A menu item's own flag, else the layout's when it sets one, else `fallback`. */
static bool menu_flag(bool item_has, bool item_flag, bool layout_has, bool layout_flag, bool fallback) {
    return item_has ? item_flag : layout_has ? layout_flag : fallback;
}

static bool menu_item_shows_icon(const icon_grid_item_t * item, const launcher_menu_layout_t * layout,
                                 bool fallback) {
    return menu_flag(item->has_icon, item->icon, layout && layout->has_icon,
                     layout && layout->icon, fallback);
}

/* The pill row for a menu item: the item's own overrides, else the (optional)
 * layout's shared values. Icon and out_row are left to the caller. A
 * non-positive row height or width means "native". */
static pill_list_item_t menu_item_row(const icon_grid_item_t * item, const launcher_menu_layout_t * layout,
                                      bool default_accessory) {
    return (pill_list_item_t) {
        .label = item->label,
        .accessory = menu_flag(item->has_accessory, item->accessory, layout && layout->has_accessory,
                               layout && layout->accessory, default_accessory)
                         ? PILL_ACCESSORY_CHEVRON : PILL_ACCESSORY_NONE,
        .on_click = item->on_click,
        .user_data = item->user_data,
        .row_height = item->has_row_height ? item->row_height
                    : (layout && layout->height > 0 ? layout->height : 0),
        .row_width = item->has_row_width ? item->row_width : (layout ? layout->width : 0),
        .text_size = item->text_size ? item->text_size
                   : (layout && layout->text_size[0] ? layout->text_size : NULL),
        .has_bg_color = item->has_bg_color || (layout && layout->has_bg_color),
        .bg_color = item->has_bg_color ? item->bg_color : (layout ? layout->bg_color : 0),
        .has_text_color = item->has_text_color || (layout && layout->has_text_color),
        .text_color = item->has_text_color ? item->text_color : (layout ? layout->text_color : 0),
        .has_radius = item->has_radius || (layout && layout->has_radius),
        .radius = item->has_radius ? item->radius : (layout ? layout->radius : 0),
        .text_align = item->text_align ? item->text_align
                    : (layout && layout->align[0] ? layout->align : NULL),
    };
}

lv_obj_t * build_launcher_menu_screen(const icon_grid_item_t * items, int item_count,
                                      const launcher_menu_layout_t * layout) {
    /* A zero-length array is undefined: an empty menu still builds a valid, empty screen. */
    pill_list_item_t rows[item_count > 0 ? item_count : 1];
    for (int i = 0; i < item_count; i++) {
        rows[i] = menu_item_row(&items[i], layout, false);
        if (menu_item_shows_icon(&items[i], layout, false))
            rows[i].icon_asset = asset_path_plain(items[i].icon_asset);
    }
    return build_pill_list_screen(NULL, NULL, rows, item_count, gui_theme_accent_style(),
                                  layout->row_gap > 0 ? layout->row_gap : 6, 100);
}

typedef struct category_shared_image {
    char * relative_path;
    unsigned refcount;
    asset_decoded_image_t asset;
    struct category_shared_image * next;
} category_shared_image_t;

typedef struct {
    category_shared_image_t * shared;
} category_image_owner_t;

static category_shared_image_t * category_shared_images;

typedef struct {
    lv_obj_t * label;
    lv_obj_t * icon;
    bool plugin_icon; /* icon drawn by pill_row_apply_icon(), not retained here */
} category_row_context_t;

static category_shared_image_t * category_shared_image_acquire(const char * relative_path) {
    if (!relative_path) return NULL;
    for (category_shared_image_t * shared = category_shared_images; shared;
         shared = shared->next) {
        if (strcmp(shared->relative_path, relative_path) == 0) {
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
    if (!asset_decoded_image_open(&shared->asset, relative_path)) {
        free(shared->relative_path);
        free(shared);
        return NULL;
    }
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

static lv_obj_t * category_create_retained_image(lv_obj_t * row, const char * relative_path) {
    category_shared_image_t * shared = category_shared_image_acquire(relative_path);
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
    if ((context->icon || context->plugin_icon) && context->label) {
        int32_t left = BOARD_SCALE_PX(96);
        /* Same right reserve build_pill_list_screen() gives a chevron row. */
        int32_t available = width - left - BOARD_SCALE_PX(60);
        if (available < BOARD_SCALE_PX(40)) available = BOARD_SCALE_PX(40);
        if (lv_obj_get_width(context->label) != available) {
            lv_obj_set_width(context->label, available);
            /* The marquee was armed for the wider icon-less box; re-arm it
             * so the scroll is evaluated against the final width. */
            if (lv_label_get_long_mode(context->label) == LV_LABEL_LONG_SCROLL_CIRCULAR)
                row_label_enable_marquee(context->label);
        }
        lv_obj_align(context->label, LV_ALIGN_LEFT_MID, left, 0);
    }
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
}

static void decorate_category_row_impl(lv_obj_t * row, const char * icon_asset,
                                       const char * plugin_icon) {
    /* With neither icon there is nothing to lay out. */
    if (!row || (!icon_asset && !plugin_icon)) return;
    /* Stock row art gives way to pre-tinted Breeze art named after it,
     * breeze_rows/<folder>_<name>.png, unless the active theme brings its
     * own (see breeze_rows/BREEZE_SOURCES.txt). */
    char breeze_row[96];
    if (icon_asset && breeze_row_icon(icon_asset, breeze_row, sizeof(breeze_row))) icon_asset = breeze_row;
    category_row_context_t * context = calloc(1, sizeof(*context));
    if (!context) return;
    /* build_pill_list_screen() creates the primary label first. */
    context->label = lv_obj_get_child(row, 0);
    context->icon = category_create_retained_image(row, icon_asset);
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
    lv_obj_add_event_cb(row, category_row_event_cb, LV_EVENT_SIZE_CHANGED, context);
    lv_obj_add_event_cb(row, category_row_event_cb, LV_EVENT_DELETE, context);
    category_row_fit(row, context);
}

void decorate_category_row(lv_obj_t * row, const char * icon_asset) {
    decorate_category_row_impl(row, icon_asset, NULL);
}

void decorate_category_row_plugin_icon(lv_obj_t * row, const char * plugin_icon) {
    decorate_category_row_impl(row, NULL, plugin_icon);
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
        rows[i] = menu_item_row(&items[i], layout, true);
        rows[i].out_row = &row_objects[i];
    }
    int32_t row_gap = layout && layout->row_gap > 0 ? layout->row_gap : GUI_ROW_GAP;
    lv_obj_t * scr = build_pill_list_screen(title, back_btn_cb, rows, item_count,
                                            gui_theme_accent_style(), row_gap, 100);
    for (int i = 0; i < item_count; ++i) {
        if (row_objects[i] && menu_item_shows_icon(&items[i], layout, true)) {
            if (items[i].plugin_icon)
                decorate_category_row_plugin_icon(row_objects[i], items[i].plugin_icon);
            else
                decorate_category_row(row_objects[i], items[i].icon_asset);
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

lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * title_label = build_screen_header(scr, default_title, generic_back_cb, NULL, NULL);

    lv_obj_t * list = screen_list_body(scr, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT,
                                       HOME_INDICATOR_CONTENT_INSET, GUI_ROW_GAP);

    *out_title_label = title_label;
    *out_list = list;
    finalize_screen_navigation(scr);
    return scr;
}
