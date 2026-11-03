#include "gui_shell_internal.h"


/* Normalizes input codec string by stripping non-alphanumeric chars and lowercasing,
 * then maps to a known codec type enum. */
static bt_codec_type_t bt_codec_identify(const char * codec) {
    if (!codec) return BT_CODEC_TYPE_NONE;

    char norm[16];
    int n = 0;
    for (int i = 0; codec[i] != '\0' && n < (int)sizeof(norm) - 1; i++) {
        unsigned char c = (unsigned char)codec[i];
        if (c >= 'A' && c <= 'Z') {
            norm[n++] = (char)(c + ('a' - 'A'));
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            norm[n++] = (char)c;
        }
    }
    norm[n] = '\0';

    if (n == 0) return BT_CODEC_TYPE_NONE;

    if (strcmp(norm, "sbc") == 0 || strcmp(norm, "sbcxq") == 0) return BT_CODEC_TYPE_SBC;
    if (strcmp(norm, "aac") == 0) return BT_CODEC_TYPE_AAC;
    if (strcmp(norm, "aptx") == 0) return BT_CODEC_TYPE_APTX;
    if (strcmp(norm, "aptxhd") == 0) return BT_CODEC_TYPE_APTX_HD;
    if (strcmp(norm, "ldac") == 0) return BT_CODEC_TYPE_LDAC;
    if (strcmp(norm, "ssc") == 0 || strcmp(norm, "samsungsc") == 0) return BT_CODEC_TYPE_SSC;
    if (strcmp(norm, "uat") == 0) return BT_CODEC_TYPE_UAT;

    return BT_CODEC_TYPE_NONE;
}

/* Returns persistent asset path for a codec type, resolving asset_path()
 * exactly once across the entire process lifetime to prevent unbounded memory leaks. */
const char * bt_codec_get_asset(bt_codec_type_t type) {
    static const char * assets[BT_CODEC_TYPE_COUNT] = { NULL };
    static bool initialized = false;
    if (!initialized) {
        assets[BT_CODEC_TYPE_SBC]     = asset_path("topbar/sbc.png");
        assets[BT_CODEC_TYPE_AAC]     = asset_path("topbar/aac.png");
        assets[BT_CODEC_TYPE_APTX]    = asset_path("topbar/aptx.png");
        assets[BT_CODEC_TYPE_APTX_HD] = asset_path("topbar/aptx_hd.png");
        assets[BT_CODEC_TYPE_LDAC]    = asset_path("topbar/ldac.png");
        assets[BT_CODEC_TYPE_SSC]     = asset_path("topbar/ssc.png");
        assets[BT_CODEC_TYPE_UAT]     = asset_path("topbar/uat.png");
        initialized = true;
    }
    if (type > BT_CODEC_TYPE_NONE && type < BT_CODEC_TYPE_COUNT) {
        return assets[type];
    }
    return NULL;
}
void style_topbar_text(lv_obj_t * label, const lv_font_t * font) {
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
}

/* Keep the persistent status strip readable while using larger faces on
 * the 480px boards. These faces deliberately ignore the accessibility tier. */
const lv_font_t * topbar_digit_font(void) {
#if defined(BOARD_TEMPOTEC_V1)
    return fallback_font_fixed(16);
#elif BOARD_COMPACT_UI
    return fallback_font_fixed(16);
#else
    return fallback_font_fixed(28);
#endif
}
const lv_font_t * topbar_ampm_font(void) {
#if defined(BOARD_TEMPOTEC_V1)
    return fallback_font_fixed(12);
#elif BOARD_COMPACT_UI || defined(BOARD_R1)
    return fallback_font_fixed(12);
#else
    return fallback_font_fixed(18);
#endif
}
#if defined(BOARD_TEMPOTEC_V1)

/* Preferred V1 artwork starts at 20px (28px Breeze canvas) and may step down
 * only when the actual content combination needs more horizontal room. */
int32_t topbar_v1_art_scale_percent = 100;
static uint32_t topbar_v1_last_layout_signature;
bool topbar_v1_layout_signature_valid;
static bool topbar_v1_wifi_connected_art;
#endif

/* Shared final geometry of every status glyph: a top-left anchored, scaled
 * image in a slot STATUS_BAR_CLEARANCE tall. */
static void topbar_apply_art(lv_obj_t * image, int scale, int width,
                             int off_x, int off_y) {
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_image_set_pivot(image, 0, 0);
    lv_image_set_scale(image, scale);
    lv_obj_set_size(image, width, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_translate_y(image, 0, 0);
    lv_image_set_offset_x(image, (int16_t) off_x);
    lv_image_set_offset_y(image, (int16_t) off_y);
}

/* Normalize Breeze art by its visible source bounds so icon ink shares the
 * text's centerline and target height. Two pixels of padding around the
 * visible bounds keeps neighboring status items legible without crowding. */
static void layout_glyph_topbar_image_sized(lv_obj_t * image, int left, int top,
                                             int width, int height,
                                             int target_px) {
#if defined(BOARD_TEMPOTEC_V1)
    int preferred_target = target_px < 20 ? target_px : 20;
    int target = (preferred_target * topbar_v1_art_scale_percent + 50) / 100;
    int padding = 0;
#else
    int target = BOARD_SCALE_PX(target_px);
    int padding = BOARD_SCALE_PX(2);
#endif
    int scale = (target * LV_SCALE_NONE + height / 2) / height;
    int visible_width = (width * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE;
    topbar_apply_art(image, scale, visible_width + 2 * padding,
                     padding - (left * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE,
                     (STATUS_BAR_CLEARANCE - target) / 2
                         - (top * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE);
}

/* Status glyphs are Breeze. Firmware images that predate it only ship the
 * Lucide set, so fall back to those names and their measured bounds when
 * the Breeze art cannot be found. */
typedef struct {
    int left, top, width, height, target_px;
} topbar_bounds_t;
#define BOUNDS_SIZED(l, t, w, h, px) ((topbar_bounds_t){ (l), (t), (w), (h), (px) })
#define BOUNDS(l, t, w, h) BOUNDS_SIZED(l, t, w, h, 20)
/* Breeze art is rasterized from its 22 px SVGs straight to the 28 px it is
 * shown at, so it is drawn unscaled and only needs its horizontal ink span
 * for the slot width. */
#define NATIVE_SIZED(l, w, px) BOUNDS_SIZED(l, 0, w, px, 0)
#define NATIVE(l, w) NATIVE_SIZED(l, w, BREEZE_ICON_PX)
#define BREEZE_ICON_PX 28
static bool topbar_breeze_icons(void) {
    static int available = -1;
    if (available < 0) {
        const char * probe = asset_path_plain("topbar/breeze_play.png");
        available = probe && access(probe, R_OK) == 0;
    }
    return available;
}
const char * topbar_icon(const char * name) {
    char relative[64];
    snprintf(relative, sizeof(relative), "topbar/%s_%s.png",
             topbar_breeze_icons() ? "breeze" : "lucide", name);
    return asset_path(relative);
}
static void layout_native_topbar_image(lv_obj_t * image, int left, int width,
                                       int canvas_px) {
#if defined(BOARD_TEMPOTEC_V1)
    /* Scale the actual Breeze raster and measured ink bounds together. The
     * preferred 28px status canvas becomes 20px; battery art uses a 25px
     * canvas so its short, wide silhouette stays legible. */
    int preferred_canvas = canvas_px == 35 ? 25 : 20;
    int scale = (preferred_canvas * topbar_v1_art_scale_percent * LV_SCALE_NONE +
                 canvas_px * 50) / (canvas_px * 100);
    const int padding = 0;
    const int scaled_width = (width * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE;
    const int scaled_left = (left * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE;
    const int scaled_canvas = (canvas_px * scale + LV_SCALE_NONE / 2) / LV_SCALE_NONE;
    topbar_apply_art(image, scale, scaled_width + 2 * padding, padding - scaled_left,
                     (STATUS_BAR_CLEARANCE - scaled_canvas) / 2);
#else
    int padding = BOARD_SCALE_PX(2);
    topbar_apply_art(image, LV_SCALE_NONE, width + 2 * padding, padding - left,
                     (STATUS_BAR_CLEARANCE - canvas_px) / 2);
#endif
}
static void layout_topbar_glyph(lv_obj_t * image, topbar_bounds_t breeze,
                                topbar_bounds_t lucide) {
    if (topbar_breeze_icons()) {
        layout_native_topbar_image(image, breeze.left, breeze.width, breeze.height);
        return;
    }
    layout_glyph_topbar_image_sized(image, lucide.left, lucide.top, lucide.width,
                                    lucide.height, lucide.target_px);
}

/* All battery states use the same measured family envelope. The resulting
 * fixed-width slot keeps the percentage group anchored through state swaps. */
void layout_battery_topbar_image(lv_obj_t * image) {
    /* The Breeze battery is short and wide, so it is rasterized at 35 px to
     * give its body the same ink height as the other glyphs. */
    layout_topbar_glyph(image, NATIVE_SIZED(1, 33, 35), BOUNDS(1, 5, 22, 14));
}
void layout_volume_topbar_image(lv_obj_t * image) {
    layout_topbar_glyph(image, NATIVE(3, 22), BOUNDS(1, 3, 22, 18));
}
void layout_headphone_topbar_image(lv_obj_t * image) {
    layout_topbar_glyph(image, NATIVE(3, 22), BOUNDS(2, 2, 20, 20));
}
void layout_a2dp_topbar_image(lv_obj_t * image) {
    layout_topbar_glyph(image, NATIVE(2, 22), BOUNDS(1, 2, 22, 20));
}
void layout_usb_audio_topbar_image(lv_obj_t * image) {
    layout_topbar_glyph(image, NATIVE(7, 14), BOUNDS(2, 2, 20, 20));
}
void layout_play_pause_topbar_image(lv_obj_t * image) {
    layout_topbar_glyph(image, NATIVE(3, 22), BOUNDS(2, 2, 20, 20));
}
void layout_radio_topbar_image(lv_obj_t * image) {
#if defined(BOARD_TEMPOTEC_V1)
    topbar_v1_wifi_connected_art = false;
#endif
    /* All Wi-Fi strengths/off states share these family bounds. Keeping
     * them identical preserves partial-state art. */
    layout_topbar_glyph(image, NATIVE(0, 28), BOUNDS(1, 1, 22, 22));
}
void layout_bluetooth_topbar_image(lv_obj_t * image) {
    /* Both states share one envelope so the slot does not shift. Lucide's
     * connected glyph is much narrower, so that set is enlarged to 24 px. */
    layout_topbar_glyph(image, NATIVE(0, 28), BOUNDS_SIZED(1, 1, 22, 22, 24));
}

/* Breeze strengths share the off glyph's bounds; Lucide's connected arcs
 * leave the top of the canvas empty, so they use the full-signal bounds. */
void layout_wifi_connected_topbar_image(lv_obj_t * image) {
#if defined(BOARD_TEMPOTEC_V1)
    topbar_v1_wifi_connected_art = true;
#endif
    layout_topbar_glyph(image, NATIVE(0, 28), BOUNDS(1, 4, 22, 17));
}

/* Codec badges share these measured solid alpha bounds (alpha >= 128) on a
 * 48x30 canvas. Scaling the 40x24 mark directly gives a clear, consistent
 * 24 px badge height while preserving two pixels of horizontal slot padding. */
void layout_codec_topbar_image(lv_obj_t * image) {
#if defined(BOARD_TEMPOTEC_V1)
    layout_glyph_topbar_image_sized(image, 4, 3, 40, 24, 16);
#else
    layout_glyph_topbar_image_sized(image, 4, 3, 40, 24, 24);
#endif
}

/* Defined further down (near wifi_icon/bt_status_icon's own setup) --
 * forward-declared here so refresh_battery_topbar() (gui_shell_topbar.c) can re-run it
 * whenever battery_topbar_group's own visibility might have changed
 * (unknown percent, or Settings > Power > "Battery Percentage" toggling),
 * since that group is one of the two anchors that logic positions the
 * wifi/bt topbar icons against. */

/* Derives topbar icon order (wifi_icon and bt_status_icon) relative to
 * battery_topbar_group/battery_icon_frame. Tracks which icon occupies the
 * inner slot (closer to battery) versus the outer slot, avoiding gaps
 * when one of the radios is disabled. Whichever icon was already visible
 * keeps the inner slot; newly-visible icons take any remaining free slot.
 * The two slots are positioned relative to
 * battery_topbar_group/battery_icon_frame. */
typedef enum {
    TOPBAR_STATUS_ICON_NONE = 0,
    TOPBAR_STATUS_ICON_WIFI,
    TOPBAR_STATUS_ICON_BT,
} topbar_status_icon_t;
static topbar_status_icon_t topbar_status_icon_order[2] = { TOPBAR_STATUS_ICON_NONE, TOPBAR_STATUS_ICON_NONE };
static bool topbar_element_hidden(uint32_t bit) {
    return (current_settings.topbar_hidden_elements & bit) != 0;
}
static void topbar_set_opa(lv_obj_t * obj, lv_opa_t opa) {
    if (!topbar_local_style_is(obj, LV_STYLE_OPA, opa)) lv_obj_set_style_opa(obj, opa, 0);
}
static void topbar_set_align(lv_obj_t * obj, lv_align_t align) {
    if (!topbar_local_style_is(obj, LV_STYLE_ALIGN, align)) lv_obj_set_style_align(obj, align, 0);
}

/* lv_obj_align() with the guarded align write; lv_obj_set_pos() already
 * skips unchanged coordinates. */
static void topbar_align(lv_obj_t * obj, lv_align_t align, int32_t x_ofs, int32_t y_ofs) {
    topbar_set_align(obj, align);
    lv_obj_set_pos(obj, x_ofs, y_ofs);
}

/* lv_obj_align_to() for LV_ALIGN_OUT_LEFT_MID / LV_ALIGN_OUT_RIGHT_MID, with
 * the same coordinate and RTL arithmetic but the guarded align write. */
static void topbar_align_beside(lv_obj_t * obj, lv_obj_t * base, bool left_of, int32_t x_ofs) {
    lv_obj_update_layout(obj);
    lv_obj_t * parent = lv_obj_get_parent(obj);
    int32_t pleft = lv_obj_get_style_space_left(parent, LV_PART_MAIN);
    int32_t ptop = lv_obj_get_style_space_top(parent, LV_PART_MAIN);
    int32_t x = left_of ? -lv_obj_get_width(obj) : lv_obj_get_width(base);
    int32_t y = lv_obj_get_height(base) / 2 - lv_obj_get_height(obj) / 2;
    int32_t scroll_x = lv_obj_get_style_base_dir(parent, LV_PART_MAIN) == LV_BASE_DIR_RTL
                           ? lv_obj_get_scroll_right(parent) : lv_obj_get_scroll_left(parent);
    lv_area_t base_area, parent_area;
    lv_obj_get_coords(base, &base_area);
    lv_obj_get_coords(parent, &parent_area);
    x += x_ofs + base_area.x1 - parent_area.x1 + scroll_x - pleft;
    y += base_area.y1 - parent_area.y1 + lv_obj_get_scroll_top(parent) - ptop;
    topbar_set_align(obj, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(obj, x, y);
}
static void apply_topbar_element_visibility(lv_obj_t * obj, uint32_t bit, bool flex_child) {
    if (!obj) return;
    bool hidden = topbar_element_hidden(bit);
    topbar_set_opa(obj, hidden ? LV_OPA_TRANSP : LV_OPA_COVER);
    if (flex_child) {
        if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
        else lv_obj_remove_flag(obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
    }
}
void apply_topbar_visibility_preferences(void) {
    apply_topbar_element_visibility(volume_topbar_icon, TOPBAR_HIDE_VOLUME, true);
    for (int i = 0; i < 3; i++)
        apply_topbar_element_visibility(volume_topbar_digit[i], TOPBAR_HIDE_VOLUME, true);
    apply_topbar_element_visibility(volume_topbar_headphone, TOPBAR_HIDE_HEADPHONES, true);
    apply_topbar_element_visibility(a2dp_status_icon, TOPBAR_HIDE_A2DP, true);
    apply_topbar_element_visibility(usb_audio_status_icon, TOPBAR_HIDE_USB_AUDIO, true);
    apply_topbar_element_visibility(play_pause_status_icon, TOPBAR_HIDE_PLAYBACK, true);
    apply_topbar_element_visibility(bt_codec_status_icon, TOPBAR_HIDE_CODEC, true);
    apply_topbar_element_visibility(clock_topbar_group, TOPBAR_HIDE_CLOCK, false);
    for (int i = 0; i < 5; i++)
        apply_topbar_element_visibility(clock_topbar_digit[i], TOPBAR_HIDE_CLOCK, false);
    apply_topbar_element_visibility(clock_topbar_ampm, TOPBAR_HIDE_CLOCK, false);
    apply_topbar_element_visibility(battery_icon_frame, TOPBAR_HIDE_BATTERY, false);
    apply_topbar_element_visibility(wifi_icon, TOPBAR_HIDE_WIFI, false);
    apply_topbar_element_visibility(bt_status_icon, TOPBAR_HIDE_BLUETOOTH, false);
}
static bool topbar_object_naturally_visible(lv_obj_t * obj, uint32_t bit) {
    return obj && !topbar_element_hidden(bit) && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* Configured clock position, with out-of-range values treated as centered. */
int topbar_clock_position(void) {
    int position = current_settings.topbar_clock_position;
    if (position < TOPBAR_CLOCK_CENTER || position > TOPBAR_CLOCK_RIGHT)
        return TOPBAR_CLOCK_CENTER;
    return position;
}

/* Visible wifi/bluetooth icons in slot order (inner slot first). Returns the
 * count, at most 2. */
static int topbar_ordered_status_icons(lv_obj_t * out[2]) {
    bool wifi_visible = topbar_object_naturally_visible(wifi_icon, TOPBAR_HIDE_WIFI);
    bool bt_visible = topbar_object_naturally_visible(bt_status_icon, TOPBAR_HIDE_BLUETOOTH);
    int count = 0;
    for (int i = 0; i < 2; i++) {
        if (topbar_status_icon_order[i] == TOPBAR_STATUS_ICON_WIFI && wifi_visible)
            out[count++] = wifi_icon;
        else if (topbar_status_icon_order[i] == TOPBAR_STATUS_ICON_BT && bt_visible)
            out[count++] = bt_status_icon;
    }
    return count;
}
static void topbar_align_left_of(lv_obj_t * obj, lv_obj_t * anchor, int gap_px) {
    if (!obj || !anchor) return;
    topbar_align_beside(obj, anchor, true, -BOARD_SCALE_PX(gap_px));
}
#if defined(BOARD_TEMPOTEC_V1)
static int32_t topbar_v1_visible_width(lv_obj_t * obj, bool visible) {
    if (!obj || !visible) return 0;
    lv_obj_update_layout(obj);
    return lv_obj_get_width(obj);
}
static void topbar_v1_place(lv_obj_t * obj, int32_t x) {
    if (obj) topbar_align(obj, LV_ALIGN_TOP_LEFT, x, 0);
}
static void topbar_v1_apply_art_scale(int32_t percent) {
    topbar_v1_art_scale_percent = percent;
    if (volume_topbar_icon)
        layout_volume_topbar_image(volume_topbar_icon);
    if (volume_topbar_headphone)
        layout_headphone_topbar_image(volume_topbar_headphone);
    if (a2dp_status_icon)
        layout_a2dp_topbar_image(a2dp_status_icon);
    if (usb_audio_status_icon)
        layout_usb_audio_topbar_image(usb_audio_status_icon);
    if (play_pause_status_icon)
        layout_play_pause_topbar_image(play_pause_status_icon);
    if (bt_codec_status_icon) layout_codec_topbar_image(bt_codec_status_icon);
    if (battery_icon_frame) layout_battery_topbar_image(battery_icon_frame);
    if (wifi_icon) {
        if (topbar_v1_wifi_connected_art) layout_wifi_connected_topbar_image(wifi_icon);
        else layout_radio_topbar_image(wifi_icon);
    }
    if (bt_status_icon) layout_bluetooth_topbar_image(bt_status_icon);
    lv_obj_update_layout(volume_topbar_group);
    lv_obj_update_layout(battery_icon_frame);
    lv_obj_update_layout(battery_topbar_group);
}
static uint32_t topbar_v1_hash_bytes(uint32_t hash, const char * value) {
    if (!value) return (hash ^ 0xffu) * 16777619u;
    while (*value) hash = (hash ^ (uint8_t)*value++) * 16777619u;
    return (hash ^ 0u) * 16777619u;
}
static uint32_t topbar_v1_hash_tree(uint32_t hash, lv_obj_t * obj) {
    if (!obj) return (hash ^ 0xfeu) * 16777619u;
    hash = (hash ^ (uint8_t)lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) * 16777619u;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        hash = topbar_v1_hash_bytes(hash, lv_label_get_text(obj));
    } else if (lv_obj_check_type(obj, &lv_image_class)) {
        const void * source = lv_image_get_src(obj);
        lv_image_src_t source_type = lv_image_src_get_type(source);
        hash = (hash ^ (uint32_t)source_type) * 16777619u;
        if (source_type == LV_IMAGE_SRC_FILE || source_type == LV_IMAGE_SRC_SYMBOL) {
            hash = topbar_v1_hash_bytes(hash, (const char *)source);
        } else {
            uintptr_t source_value = (uintptr_t)source;
            for (size_t i = 0; i < sizeof(source_value); i++) {
                hash = (hash ^ (uint8_t)(source_value & 0xffu)) * 16777619u;
                source_value >>= 8;
            }
        }
    }
    uint32_t children = lv_obj_get_child_count(obj);
    hash = (hash ^ (uint8_t)children) * 16777619u;
    for (uint32_t i = 0; i < children; i++)
        hash = topbar_v1_hash_tree(hash, lv_obj_get_child(obj, (int32_t)i));
    return hash;
}
static uint32_t topbar_v1_current_layout_signature(void) {
    uint32_t hash = 2166136261u;
    hash = (hash ^ current_settings.topbar_hidden_elements) * 16777619u;
    hash = (hash ^ (uint32_t)current_settings.show_battery_percent) * 16777619u;
    hash = (hash ^ (uint32_t)current_settings.topbar_clock_position) * 16777619u;
    hash = (hash ^ (uint32_t)topbar_v1_wifi_connected_art) * 16777619u;
    hash = (hash ^ (uint32_t)lv_display_get_horizontal_resolution(lv_display_get_default())) * 16777619u;
    hash = (hash ^ (uint32_t)topbar_breeze_icons()) * 16777619u;
    return topbar_v1_hash_tree(hash, status_bar_band);
}

/* V1 keeps its 20px band and readable 16px clock. Scale only icon art when
 * all currently visible content would otherwise exceed the panel width. */
static void reflow_topbar_layout_v1(void) {
    int32_t screen_width = lv_display_get_horizontal_resolution(lv_display_get_default());
    if (screen_width <= 0) screen_width = BOARD_SCREEN_WIDTH;

    bool clock_visible = !topbar_element_hidden(TOPBAR_HIDE_CLOCK);
    int clock_position = topbar_clock_position();
    bool battery_visible = !topbar_element_hidden(TOPBAR_HIDE_BATTERY);
    bool percent_visible = current_settings.show_battery_percent &&
                           !lv_obj_has_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);

    /* This flex group also carries independently enabled output indicators,
     * so its measured width remains authoritative even when volume is hidden. */
    int32_t volume_width = 0;
    bool output_group_visible = false;
    int32_t clock_width = topbar_v1_visible_width(clock_topbar_group, clock_visible);

    lv_obj_t * right_items[4];
    int32_t right_widths[4];
    int right_count = 0;
    if (battery_visible) right_items[right_count++] = battery_icon_frame;
    if (percent_visible) right_items[right_count++] = battery_topbar_group;
    lv_obj_t * status_icons[2];
    int status_count = topbar_ordered_status_icons(status_icons);
    for (int i = 0; i < status_count; i++) right_items[right_count++] = status_icons[i];

    int32_t margin = 0;
    int32_t gap = 0;
    int32_t right_cluster_width = 0;
    int32_t block_count = 0;
    int32_t required_width = 0;
    uint32_t layout_signature = topbar_v1_current_layout_signature();
    bool fit_needed = !topbar_v1_layout_signature_valid ||
                      layout_signature != topbar_v1_last_layout_signature;
    int32_t art_scale = fit_needed ? 100 : topbar_v1_art_scale_percent;
    for (;;) {
        if (fit_needed) topbar_v1_apply_art_scale(art_scale);
        volume_width = topbar_v1_visible_width(volume_topbar_group, true);
        output_group_visible = volume_width > 0;
        for (int i = 0; i < right_count; i++)
            right_widths[i] = topbar_v1_visible_width(right_items[i], true);

        margin = BOARD_SCALE_PX(4);
        gap = BOARD_SCALE_PX(2);
        right_cluster_width = 0;
        for (int i = 0; i < right_count; i++) right_cluster_width += right_widths[i];
        if (right_count > 1) right_cluster_width += gap * (right_count - 1);
        block_count = (output_group_visible ? 1 : 0) + (right_count > 0 ? 1 : 0) +
                      (clock_visible ? 1 : 0);
        required_width = margin * 2 + volume_width + right_cluster_width +
                         (clock_visible ? clock_width : 0) +
                         (block_count > 1 ? gap * (block_count - 1) : 0);
        if (!fit_needed || required_width <= screen_width || art_scale <= 40) break;
        art_scale -= 5;
    }

    /* At the minimum artwork tier remove the outer margins while retaining
     * a one-pixel gap between independent content groups. */
    if (required_width > screen_width) {
        margin = 0;
        gap = 1;
        right_cluster_width = 0;
        for (int i = 0; i < right_count; i++) right_cluster_width += right_widths[i];
        if (right_count > 1) right_cluster_width += gap * (right_count - 1);
        block_count = (output_group_visible ? 1 : 0) + (right_count > 0 ? 1 : 0) +
                      (clock_visible ? 1 : 0);
        required_width = volume_width + right_cluster_width +
                         (clock_visible ? clock_width : 0) +
                         (block_count > 1 ? gap * (block_count - 1) : 0);
    }

    topbar_v1_last_layout_signature = layout_signature;
    topbar_v1_layout_signature_valid = true;

    int32_t right_edge = screen_width - margin;
    int32_t clock_x = screen_width / 2 - clock_width / 2;
    if (clock_visible && clock_position == TOPBAR_CLOCK_RIGHT) {
        clock_x = screen_width - margin - clock_width;
        right_edge = clock_x - gap;
    }

    int32_t cursor = right_edge;
    for (int i = 0; i < right_count; i++) {
        cursor -= right_widths[i];
        topbar_v1_place(right_items[i], cursor);
        cursor -= gap;
    }
    int32_t right_start = right_count ? cursor + gap : right_edge;

    int32_t volume_x = margin;
    if (clock_visible && clock_position == TOPBAR_CLOCK_LEFT) {
        topbar_v1_place(clock_topbar_group, margin);
        volume_x = margin + clock_width + (output_group_visible ? gap : 0);
    } else if (clock_visible && clock_position == TOPBAR_CLOCK_CENTER) {
        int32_t left_edge = margin + (output_group_visible ? volume_width : 0);
        int32_t minimum_clock_x = left_edge + (output_group_visible ? gap : 0);
        int32_t maximum_clock_x = right_start - (right_count ? gap : 0) - clock_width;
        if (maximum_clock_x >= minimum_clock_x) {
            if (clock_x < minimum_clock_x) clock_x = minimum_clock_x;
            if (clock_x > maximum_clock_x) clock_x = maximum_clock_x;
        }
        topbar_v1_place(clock_topbar_group, clock_x);
    } else if (clock_visible && clock_position == TOPBAR_CLOCK_RIGHT) {
        topbar_v1_place(clock_topbar_group, clock_x);
    }

    if (output_group_visible) topbar_v1_place(volume_topbar_group, volume_x);

    if (required_width > screen_width) {
        LV_LOG_WARN("V1 topbar measured width %d exceeds display width %d",
                    (int)required_width, (int)screen_width);
    }
}
#endif
void reflow_topbar_layout(void) {
    if (!status_bar_band || !clock_topbar_group || !volume_topbar_group || !battery_icon_frame ||
        !battery_topbar_group || !wifi_icon || !bt_status_icon) return;

    apply_topbar_visibility_preferences();
    lv_obj_update_layout(status_bar_band);
    lv_obj_update_layout(clock_topbar_group);
    lv_obj_update_layout(volume_topbar_group);
    lv_obj_update_layout(battery_topbar_group);

#if defined(BOARD_TEMPOTEC_V1)
    reflow_topbar_layout_v1();
    return;
#endif

    bool clock_visible = !topbar_element_hidden(TOPBAR_HIDE_CLOCK);
    int clock_position = topbar_clock_position();
    if (clock_visible) {
        topbar_align(clock_topbar_group,
                     clock_position == TOPBAR_CLOCK_LEFT ? LV_ALIGN_LEFT_MID :
                     clock_position == TOPBAR_CLOCK_RIGHT ? LV_ALIGN_RIGHT_MID : LV_ALIGN_CENTER,
                     clock_position == TOPBAR_CLOCK_LEFT ? BOARD_SCALE_PX(4) :
                     clock_position == TOPBAR_CLOCK_RIGHT ? -BOARD_SCALE_PX(4) : 0, 0);
    }

    if (clock_visible && clock_position == TOPBAR_CLOCK_LEFT)
        topbar_align_beside(volume_topbar_group, clock_topbar_group, false, BOARD_SCALE_PX(2));
    else
        topbar_align(volume_topbar_group, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(4), 0);

    lv_obj_t * right_anchor = clock_visible && clock_position == TOPBAR_CLOCK_RIGHT
                                  ? clock_topbar_group : NULL;
    bool battery_visible = !topbar_element_hidden(TOPBAR_HIDE_BATTERY);
    if (battery_visible) {
        if (right_anchor) topbar_align_left_of(battery_icon_frame, right_anchor, 2);
        else topbar_align(battery_icon_frame, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(4), 0);
        right_anchor = battery_icon_frame;
    }

    bool percent_visible = current_settings.show_battery_percent &&
                           !lv_obj_has_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
    if (percent_visible) {
        if (right_anchor) topbar_align_left_of(battery_topbar_group, right_anchor, 2);
        else topbar_align(battery_topbar_group, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(4), 0);
        right_anchor = battery_topbar_group;
    }

    lv_obj_t * status_icons[2];
    int status_count = topbar_ordered_status_icons(status_icons);
    for (int i = 0; i < status_count; i++) {
        lv_obj_t * widget = status_icons[i];
        if (right_anchor) topbar_align_left_of(widget, right_anchor, 2);
        else topbar_align(widget, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(4), 0);
        right_anchor = widget;
    }
}
void sync_topbar_status_icon_positions(void) {
    bool wifi_visible = topbar_object_naturally_visible(wifi_icon, TOPBAR_HIDE_WIFI);
    bool bt_visible = topbar_object_naturally_visible(bt_status_icon, TOPBAR_HIDE_BLUETOOTH);

    topbar_status_icon_t new_order[2] = { TOPBAR_STATUS_ICON_NONE, TOPBAR_STATUS_ICON_NONE };
    int slot = 0;
    /* Existing occupants first, in their current order, so an icon that's
     * still visible never moves slots just because the other one's
     * visibility also happened to change on this same call. */
    for (int i = 0; i < 2 && slot < 2; i++) {
        topbar_status_icon_t icon = topbar_status_icon_order[i];
        if ((icon == TOPBAR_STATUS_ICON_WIFI && wifi_visible) || (icon == TOPBAR_STATUS_ICON_BT && bt_visible)) {
            new_order[slot++] = icon;
        }
    }
    /* Then any newly-visible icon not already placed above, oldest-checked
     * (wifi) first -- only matters when both go from hidden to visible on
     * the exact same call, an arbitrary but stable tiebreak. */
    if (wifi_visible && new_order[0] != TOPBAR_STATUS_ICON_WIFI && new_order[1] != TOPBAR_STATUS_ICON_WIFI && slot < 2) {
        new_order[slot++] = TOPBAR_STATUS_ICON_WIFI;
    }
    if (bt_visible && new_order[0] != TOPBAR_STATUS_ICON_BT && new_order[1] != TOPBAR_STATUS_ICON_BT && slot < 2) {
        new_order[slot++] = TOPBAR_STATUS_ICON_BT;
    }
    topbar_status_icon_order[0] = new_order[0];
    topbar_status_icon_order[1] = new_order[1];

    reflow_topbar_layout();
    sync_bt_codec_status_icon();
}
static bool last_codec_eligible = false;
static bt_codec_type_t last_codec_type = BT_CODEC_TYPE_NONE;
static uint32_t last_codec_layout_sig = 0;
static bool hidden_due_to_overlap = false;
void invalidate_bt_codec_status_cache(void) {
    last_codec_eligible = false;
    last_codec_type = BT_CODEC_TYPE_NONE;
    last_codec_layout_sig = 0;
    hidden_due_to_overlap = false;
}


/* Updates the negotiated Bluetooth A2DP codec badge in the topbar.
 * Fully edge-triggered: caches eligibility, codec type, and neighboring
 * layout visibility to avoid redundant lv_image_set_src() or layout recomputations
 * during periodic polling when state is unchanged. */
static uint32_t topbar_codec_geometry_signature(uint32_t signature) {
    if (!status_bar_band || !volume_topbar_group || !clock_topbar_group || !battery_topbar_group)
        return signature;
    lv_obj_update_layout(volume_topbar_group);
    lv_obj_update_layout(clock_topbar_group);
    lv_obj_update_layout(battery_topbar_group);
    signature = signature * 33u + (uint32_t)lv_obj_get_width(status_bar_band);
    signature = signature * 33u + (uint32_t)lv_obj_get_width(clock_topbar_group);
    signature = signature * 33u + (uint32_t)lv_obj_get_x(clock_topbar_group);
    signature = signature * 33u + (uint32_t)lv_obj_get_width(battery_topbar_group);
    signature = signature * 33u + (uint32_t)lv_obj_get_x(battery_topbar_group);
    int32_t left_width = lv_obj_get_width(volume_topbar_group);
    if (bt_codec_status_icon && !lv_obj_has_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN))
        left_width -= lv_obj_get_width(bt_codec_status_icon);
    signature = signature * 33u + (uint32_t)left_width;
    signature = signature * 33u + (uint32_t)lv_obj_get_x(volume_topbar_group);
    signature = signature * 33u + (uint32_t)current_settings.topbar_hidden_elements;
    signature = signature * 33u + (uint32_t)current_settings.topbar_clock_position;
    signature = signature * 33u + (current_settings.show_battery_percent ? 1u : 0u);
    signature = signature * 33u +
        ((topbar_object_naturally_visible(wifi_icon, TOPBAR_HIDE_WIFI) ? 1u : 0u) |
         (topbar_object_naturally_visible(bt_status_icon, TOPBAR_HIDE_BLUETOOTH) ? 2u : 0u));
    return signature;
}
void sync_bt_codec_status_icon(void) {
    if (!bt_codec_status_icon) return;

    apply_topbar_visibility_preferences();
    if (topbar_element_hidden(TOPBAR_HIDE_CODEC)) {
        reflow_topbar_layout();
        return;
    }

    bt_codec_type_t codec_type = bt_codec_identify(bt_connected_codec_cached);
    bool eligible = bt_is_powered_cached && bt_is_a2dp_connected_ui &&
                    (codec_type != BT_CODEC_TYPE_NONE);

    bool currently_hidden = lv_obj_has_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
    reflow_topbar_layout();

    if (!eligible) {
        if (last_codec_eligible || !currently_hidden || hidden_due_to_overlap) {
            lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
            invalidate_bt_codec_status_cache();
            reflow_topbar_layout();
        }
        return;
    }

    /* Compute layout signature of all neighboring elements that affect horizontal width */
    bool hp_vis = volume_topbar_headphone && !lv_obj_has_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    bool a2dp_vis = a2dp_status_icon && !lv_obj_has_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    bool usb_vis = usb_audio_status_icon && !lv_obj_has_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    bool pp_vis = play_pause_status_icon && !lv_obj_has_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
    bool ampm_vis = clock_topbar_ampm && !lv_obj_has_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    int vol_digits_vis = 0;
    for (int i = 0; i < 3; i++) {
        if (volume_topbar_digit[i] && !lv_obj_has_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN)) {
            vol_digits_vis++;
        }
    }

    uint32_t layout_base_sig = (hp_vis ? 1 : 0) |
                          (a2dp_vis ? 2 : 0) |
                          (usb_vis ? 4 : 0) |
                          (pp_vis ? 8 : 0) |
                          (ampm_vis ? 16 : 0) |
                          ((uint32_t)(vol_digits_vis & 0x7) << 5);
    uint32_t layout_sig = topbar_codec_geometry_signature(layout_base_sig);

    /* If eligibility, codec type, and layout factors haven't changed, and the badge is in its steady state
     * (either visible or already known to be hidden due to right-side overlap), do nothing */
    if (last_codec_eligible && last_codec_type == codec_type && last_codec_layout_sig == layout_sig) {
        if (hidden_due_to_overlap || !currently_hidden) {
            return;
        }
    }

    last_codec_eligible = true;

    if (last_codec_type != codec_type) {
        last_codec_type = codec_type;
        const char * asset = bt_codec_get_asset(codec_type);
        if (asset) lv_image_set_src(bt_codec_status_icon, asset);
    }

    lv_obj_remove_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
    apply_topbar_visibility_preferences();
    reflow_topbar_layout();

    /* Hide the optional codec badge when the left row would reach the
     * centered clock or the right-side indicators. */
    hidden_due_to_overlap = false;
    if (volume_topbar_group) {
        lv_obj_update_layout(volume_topbar_group);
        int32_t left_right = lv_obj_get_x(volume_topbar_group) + lv_obj_get_width(volume_topbar_group);
        int32_t right_boundary = LV_COORD_MAX;
        if (clock_topbar_group && !topbar_element_hidden(TOPBAR_HIDE_CLOCK) &&
            current_settings.topbar_clock_position != TOPBAR_CLOCK_LEFT) {
            lv_obj_update_layout(clock_topbar_group);
            right_boundary = lv_obj_get_x(clock_topbar_group);
        }
        if (bt_status_icon && topbar_object_naturally_visible(bt_status_icon, TOPBAR_HIDE_BLUETOOTH)) {
            int32_t x = lv_obj_get_x(bt_status_icon);
            if (x < right_boundary) right_boundary = x;
        }
        if (wifi_icon && topbar_object_naturally_visible(wifi_icon, TOPBAR_HIDE_WIFI)) {
            int32_t x = lv_obj_get_x(wifi_icon);
            if (x < right_boundary) right_boundary = x;
        }
        if (current_settings.show_battery_percent &&
            battery_topbar_group && !lv_obj_has_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_update_layout(battery_topbar_group);
            int32_t x = lv_obj_get_x(battery_topbar_group);
            if (x < right_boundary) right_boundary = x;
        }
        if (topbar_object_naturally_visible(battery_icon_frame, TOPBAR_HIDE_BATTERY)) {
            int32_t x = lv_obj_get_x(battery_icon_frame);
            if (x < right_boundary) right_boundary = x;
        }
        if (right_boundary != LV_COORD_MAX && left_right + BOARD_SCALE_PX(2) > right_boundary) {
            lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);
            reflow_topbar_layout();
            hidden_due_to_overlap = true;
        }
    }
    last_codec_layout_sig = topbar_codec_geometry_signature(layout_base_sig);
}
void topbar_layout_teardown(void) {
    topbar_status_icon_order[0] = TOPBAR_STATUS_ICON_NONE;
    topbar_status_icon_order[1] = TOPBAR_STATUS_ICON_NONE;
}
