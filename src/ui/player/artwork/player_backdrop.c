#include "player_backdrop.h"
#include "player_cover.h"
#include "player_artwork_pixels.h"
#include "../screen/gui_player_internal.h"
#include "../screen/gui_player.h"
#include "../layout/player_layout.h"
#include "../layout/player_layout_internal.h"
#include "../transport/player_transport_images.h"
#include "player_lyrics_backdrop.h"
#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_shell.h"
#include "../../../core/system/runtime/board_config.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t * current_reflection_bytes = NULL;
static lv_image_dsc_t current_reflection_dsc;
static struct {
    bool valid;
    const uint8_t * cover_bytes;
    uint64_t cover_generation;
    int blur_radius, blur_passes, darken_num, darken_den;
    uint32_t tint_rgb;
    int tint_weight;
} current_reflection_key;

/* Resolves effective frosted-glass layout parameters from player_layout_config,
 * applying defaults and defensive clamping. Reads the global layout config once
 * into a local snapshot to avoid data races with UI thread updates. */
player_frost_params_t resolve_player_frost_params(void) {
    player_layout_config_t cfg = player_layout_config;
    player_frost_params_t params = {
        .flat = false,
        .has_bg_color = false,
        .bg_color = 0,
        .blur_radius = REFLECTION_BLUR_RADIUS,
        .blur_passes = REFLECTION_BLUR_PASSES,
        .darken_num = REFLECTION_DARKEN_NUM,
        .darken_den = REFLECTION_DARKEN_DEN,
    };

    if (cfg.configured) {
        if (cfg.flat) {
            params.flat = true;
            params.has_bg_color = cfg.has_bg_color;
            params.bg_color = cfg.bg_color;
            return params;
        }
        if (cfg.has_blur_radius) {
            params.blur_radius = cfg.blur_radius;
            if (params.blur_radius < 0) params.blur_radius = 0;
            if (params.blur_radius > 64) params.blur_radius = 64;
        }
        if (cfg.has_blur_passes) {
            params.blur_passes = cfg.blur_passes;
            if (params.blur_passes < 0) params.blur_passes = 0;
            if (params.blur_passes > 16) params.blur_passes = 16;
        }
        if (cfg.has_darken) {
            params.darken_num = cfg.darken_num;
            params.darken_den = cfg.darken_den;
            if (params.darken_num < 0) params.darken_num = 0;
            if (params.darken_num > 64) params.darken_num = 64;
            if (params.darken_den < 1) params.darken_den = 1;
            if (params.darken_den > 64) params.darken_den = 64;
        }
    }

    /* Like the stock player, the frost follows the theme's lightness: a dark
     * theme keeps the darkened artwork, a light theme washes it out toward
     * its own screen color instead. */
    lv_style_value_t screen;
    if (lv_style_get_prop(&style_theme_screen_bg, LV_STYLE_BG_COLOR, &screen) == LV_STYLE_RES_FOUND &&
        !gui_theme_prefers_light_text(screen.color)) {
        params.darken_num = 1;
        params.darken_den = 1;
        params.tint_rgb = lv_color_to_u32(screen.color) & 0xFFFFFF;
        params.tint_weight = REFLECTION_LIGHT_TINT_WEIGHT;
    }
    return params;
}

static bool reflection_matches(const player_frost_params_t * params) {
    return current_reflection_bytes && current_reflection_key.valid &&
           current_reflection_key.cover_bytes == player_cover_bytes() &&
           current_reflection_key.cover_generation == player_cover_generation() &&
           current_reflection_key.blur_radius == params->blur_radius &&
           current_reflection_key.blur_passes == params->blur_passes &&
           current_reflection_key.darken_num == params->darken_num &&
           current_reflection_key.darken_den == params->darken_den &&
           current_reflection_key.tint_rgb == params->tint_rgb &&
           current_reflection_key.tint_weight == params->tint_weight;
}

/* Clear all LVGL references before releasing reflection storage. The quick
 * drawer can hold its own image source for this same descriptor. */
void drop_current_reflection(void) {
    bool had_state = current_reflection_bytes || current_reflection_dsc.data ||
                     current_reflection_key.valid;
    if (player_background_img) {
        lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(player_background_img, NULL);
    }
    lv_image_cache_drop(&current_reflection_dsc);
    uint8_t * old = current_reflection_bytes;
    current_reflection_bytes = NULL;
    memset(&current_reflection_dsc, 0, sizeof(current_reflection_dsc));
    memset(&current_reflection_key, 0, sizeof(current_reflection_key));
    if (had_state) gui_shell_refresh_quick_drawer_cover();
    free(old);
}

static void set_current_reflection_key(const player_frost_params_t * params) {
    current_reflection_key.valid = current_reflection_bytes != NULL;
    current_reflection_key.cover_bytes = player_cover_bytes();
    current_reflection_key.cover_generation = player_cover_generation();
    current_reflection_key.blur_radius = params->blur_radius;
    current_reflection_key.blur_passes = params->blur_passes;
    current_reflection_key.darken_num = params->darken_num;
    current_reflection_key.darken_den = params->darken_den;
    current_reflection_key.tint_rgb = params->tint_rgb;
    current_reflection_key.tint_weight = params->tint_weight;
}

static void attach_current_reflection(void) {
    if (!current_reflection_bytes || !player_background_img) return;
    memset(&current_reflection_dsc, 0, sizeof(current_reflection_dsc));
    current_reflection_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    current_reflection_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    current_reflection_dsc.header.w = REFLECTION_WIDTH;
    current_reflection_dsc.header.h = BOARD_SCREEN_HEIGHT;
    current_reflection_dsc.header.stride = REFLECTION_WIDTH * 2;
    current_reflection_dsc.data = current_reflection_bytes;
    current_reflection_dsc.data_size = (uint32_t) REFLECTION_WIDTH * BOARD_SCREEN_HEIGHT * 2;
    lv_image_set_src(player_background_img, &current_reflection_dsc);
    lv_obj_set_pos(player_background_img, 0, 0);
    lv_obj_set_size(player_background_img, REFLECTION_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_image_set_pivot(player_background_img, 0, 0);
    lv_image_set_scale_x(player_background_img, LV_SCALE_NONE);
    lv_image_set_scale_y(player_background_img, LV_SCALE_NONE);
    lv_obj_remove_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
    gui_player_update_backdrop_contrast();
}

/* Text on the Now Playing backdrop follows what is behind it, not the theme:
 * the backdrop is cover artwork (blurred and darkened by default) or a
 * layout's flat color, either of which a light theme's text can vanish on. */
static lv_style_t backdrop_text_primary_style;
static lv_style_t backdrop_text_muted_style;
static lv_style_t backdrop_icon_style;
/* Custom layouts' panels and chips take the theme's window color, and their
 * progress groove the backdrop text at low opacity, as Breeze draws them.
 * Both carry only colors, so a layout's own opacity still decides whether a
 * panel shows at all. */
static lv_style_t backdrop_surface_style;
static lv_style_t backdrop_track_style;
static lv_style_t backdrop_arc_style;
static bool backdrop_styles_ready;

void backdrop_styles_init(void) {
    if (backdrop_styles_ready) return;
    lv_style_init(&backdrop_text_primary_style);
    lv_style_init(&backdrop_text_muted_style);
    lv_style_init(&backdrop_icon_style);
    lv_style_init(&backdrop_surface_style);
    lv_style_init(&backdrop_track_style);
    lv_style_set_bg_color(&backdrop_surface_style, lv_color_hex(GUI_COLOR_SCREEN));
    lv_style_set_bg_color(&backdrop_track_style, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_bg_opa(&backdrop_track_style, LV_OPA_40);
    lv_style_init(&backdrop_arc_style);
    lv_style_set_arc_color(&backdrop_arc_style, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_arc_opa(&backdrop_arc_style, LV_OPA_40);
    lv_style_set_text_color(&backdrop_text_primary_style, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_text_color(&backdrop_text_muted_style, lv_color_hex(GUI_COLOR_SECONDARY));
    lv_style_set_image_recolor_opa(&backdrop_icon_style, LV_OPA_TRANSP);
    backdrop_styles_ready = true;
}

lv_style_t * gui_player_backdrop_text_style(bool muted) {
    backdrop_styles_init();
    return muted ? &backdrop_text_muted_style : &backdrop_text_primary_style;
}

lv_style_t * gui_player_backdrop_icon_style(void) {
    backdrop_styles_init();
    return &backdrop_icon_style;
}

/* Mean color of what is visible behind the player's text. */
static lv_color_t player_backdrop_color(void) {
    if (current_reflection_bytes && player_background_img &&
        !lv_obj_has_flag(player_background_img, LV_OBJ_FLAG_HIDDEN)) {
        const uint16_t * px = (const uint16_t *) current_reflection_bytes;
        uint32_t r = 0, g = 0, b = 0, n = 0;
        for (int32_t y = 0; y < BOARD_SCREEN_HEIGHT; y += 8) {
            for (int32_t x = 0; x < REFLECTION_WIDTH; x += 8) {
                uint16_t p = px[(size_t) y * REFLECTION_WIDTH + x];
                r += (p >> 11) << 3;
                g += ((p >> 5) & 63) << 2;
                b += (p & 31) << 3;
                n++;
            }
        }
        if (n) return lv_color_make((uint8_t) (r / n), (uint8_t) (g / n), (uint8_t) (b / n));
    }
    if (player_overlay_panel && lv_obj_get_style_bg_opa(player_overlay_panel, 0) > LV_OPA_50)
        return lv_obj_get_style_bg_color(player_overlay_panel, 0);
    lv_style_value_t screen;
    if (lv_style_get_prop(&style_theme_screen_bg, LV_STYLE_BG_COLOR, &screen) == LV_STYLE_RES_FOUND)
        return screen.color;
    return lv_color_hex(GUI_COLOR_SCREEN);
}

void gui_player_update_backdrop_contrast(void) {
    backdrop_styles_init();
    lv_style_value_t primary, muted;
    if (lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &primary) != LV_STYLE_RES_FOUND ||
        lv_style_get_prop(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, &muted) != LV_STYLE_RES_FOUND)
        return;
    lv_style_value_t screen;
    lv_color_t surface = lv_style_get_prop(&style_theme_screen_bg, LV_STYLE_BG_COLOR, &screen) ==
                         LV_STYLE_RES_FOUND ? screen.color : lv_color_hex(GUI_COLOR_SCREEN);
    lv_style_value_t current_surface;
    if (lv_style_get_prop(&backdrop_surface_style, LV_STYLE_BG_COLOR, &current_surface) != LV_STYLE_RES_FOUND ||
        !lv_color_eq(current_surface.color, surface)) {
        lv_style_set_bg_color(&backdrop_surface_style, surface);
        lv_obj_report_style_change(&backdrop_surface_style);
    }
    lv_color_t bg = player_backdrop_color();
    lv_color_t text = primary.color, secondary = muted.color;
    /* The theme's own colors stay wherever they read cleanly (the player's
     * title and metadata are large text, so WCAG's 3:1); otherwise the
     * matching Breeze pair. A dark theme's text on its darkened frost always
     * passes, so it keeps today's look. */
    bool neutral = gui_theme_contrast_ratio(text, bg) < GUI_CONTRAST_MUTED;
    if (neutral) {
        bool light = gui_theme_prefers_light_text(bg);
        text = lv_color_hex(light ? GUI_BREEZE_TEXT_ON_DARK : GUI_BREEZE_TEXT_ON_LIGHT);
        secondary = lv_color_hex(light ? GUI_BREEZE_MUTED_ON_DARK : GUI_BREEZE_MUTED_ON_LIGHT);
    }
    /* Transport glyphs come in a white set (theme2) and a dark one (theme1),
     * whichever the theme picked. Tint them with the text when the text is
     * dark, or when the theme's text was replaced: either way the stock
     * glyph color is not known to read on this backdrop. */
    bool tint_icons = neutral || gui_theme_color_is_dark(text);
    lv_style_value_t current;
    bool same = lv_style_get_prop(&backdrop_text_primary_style, LV_STYLE_TEXT_COLOR, &current) == LV_STYLE_RES_FOUND &&
                lv_color_eq(current.color, text) &&
                lv_style_get_prop(&backdrop_text_muted_style, LV_STYLE_TEXT_COLOR, &current) == LV_STYLE_RES_FOUND &&
                lv_color_eq(current.color, secondary) &&
                lv_style_get_prop(&backdrop_icon_style, LV_STYLE_IMAGE_RECOLOR_OPA, &current) == LV_STYLE_RES_FOUND &&
                (current.num == LV_OPA_COVER) == tint_icons;
    if (same) return;
    lv_style_set_bg_color(&backdrop_track_style, text);
    lv_style_set_arc_color(&backdrop_arc_style, text);
    lv_obj_report_style_change(&backdrop_track_style);
    lv_obj_report_style_change(&backdrop_arc_style);
    lv_style_set_text_color(&backdrop_text_primary_style, text);
    lv_style_set_text_color(&backdrop_text_muted_style, secondary);
    lv_style_set_image_recolor(&backdrop_icon_style, text);
    lv_style_set_image_recolor_opa(&backdrop_icon_style, tint_icons ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_obj_report_style_change(&backdrop_text_primary_style);
    lv_obj_report_style_change(&backdrop_text_muted_style);
    lv_obj_report_style_change(&backdrop_icon_style);
    bool glyph_on_light = gui_theme_contrast_ratio(lv_color_white(), bg) < GUI_CONTRAST_MUTED;
    if (player_transport_images_set_glyph_on_light(glyph_on_light)) {
        refresh_play_btn_icon();
    }
    gui_shell_refresh_quick_drawer_cover();
    /* The status bar over the player follows the same choice. */
    if (gui_player_get_screen() && lv_screen_active() == gui_player_get_screen())
        gui_shell_set_status_bar_screen_context(gui_player_get_screen());
    player_transition_mark_dirty();
}

void apply_player_flat_background(bool has_bg_color, uint32_t bg_color) {
    drop_current_reflection();

    if (!player_overlay_panel) return;

    if (has_bg_color) {
        lv_obj_set_style_bg_color(player_overlay_panel, lv_color_hex(bg_color), 0);
    } else {
        lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
    }
    lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_COVER, 0);
    gui_player_update_backdrop_contrast();
    player_transition_mark_dirty();
}

/* The frosted image covers the panel, so its own fill goes transparent. */
void player_panel_show_reflection(void) {
    if (!player_overlay_panel) return;
    lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_TRANSP, 0);
}

void gui_player_refresh_frosted_background(void) {
    player_lyrics_backdrop_invalidate();
    if (!player_overlay_panel) return;

    player_frost_params_t params = resolve_player_frost_params();
    if (params.flat) {
        /* Flat mode is a plain color fill -- doesn't need decoded cover
         * pixels, so it must apply even before any track has played or
         * after a failed decode, unlike the blur branch below. */
        apply_player_flat_background(params.has_bg_color, params.bg_color);
        return;
    }
    if (!player_cover_bytes()) {
        /* Nothing to blur yet -- next decode picks up the current config.
         * Still clear any flat-mode BG_COLOR left over from a previous
         * live switch away from flat, so a stale tint doesn't linger
         * behind the frosted image once a cover does decode. */
        drop_current_reflection();
        lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
        gui_player_update_backdrop_contrast();
        return;
    }

    if (reflection_matches(&params)) {
#ifdef UI_PERF_TRACE
        printf("PERF frost_cache reused=1 generation=%llu\n",
               (unsigned long long) player_cover_generation());
#endif
        player_panel_show_reflection();
        attach_current_reflection();
        player_transition_mark_dirty();
        return;
    }

#ifdef UI_PERF_TRACE
    printf("PERF frost_cache reused=0 generation=%llu blur=%d passes=%d darken=%d/%d\n",
           (unsigned long long) player_cover_generation(), params.blur_radius,
           params.blur_passes, params.darken_num, params.darken_den);
#endif
    drop_current_reflection();
    current_reflection_bytes = player_reflection_cache_get(player_cover_file(), &params);
    if (!current_reflection_bytes)
        current_reflection_bytes = player_compute_reflection_bytes_cancelable(player_cover_bytes(), player_cover_edge(),
                                                                               params.blur_radius,
                                                                               params.blur_passes,
                                                                               params.darken_num,
                                                                               params.darken_den,
                                                                               params.tint_rgb,
                                                                               params.tint_weight,
                                                                               NULL, NULL);
    set_current_reflection_key(&params);

    /* TRANSP, not COVER -- player_background_img (the frosted image, about
     * to be shown) fully covers this panel; keeping the panel's own paint
     * opaque underneath it forces LVGL to composite two full-screen opaque
     * layers every frame for no visible benefit. The lyrics screen's own
     * analogous wrapper (dark_overlay in gui_lyrics.c) is deliberately kept
     * transparent for exactly this reason -- its own comment explains
     * darkness is already baked into its backdrop image. */
    player_panel_show_reflection();

    attach_current_reflection();
    gui_shell_refresh_quick_drawer_cover();
    player_transition_mark_dirty();
}

/* Takes ownership of a reflection computed by the cover decode worker and
 * shows it: the tail of the cover poll's result application. */
void player_backdrop_adopt_reflection(uint8_t * reflection, const player_frost_params_t * params) {
    drop_current_reflection();
    current_reflection_bytes = reflection;
    set_current_reflection_key(params);

    /* TRANSP, not COVER -- see gui_player_refresh_frosted_background()'s
     * own comment on this exact line for why. */
    player_panel_show_reflection();

    attach_current_reflection();
}

lv_style_t * player_backdrop_progress_style(bool arc) {
    backdrop_styles_init();
    return arc ? &backdrop_arc_style : &backdrop_track_style;
}

lv_style_t * player_backdrop_surface_style(void) {
    backdrop_styles_init();
    return &backdrop_surface_style;
}

const lv_image_dsc_t * gui_player_get_current_reflection_dsc(void) {
    return current_reflection_bytes ? &current_reflection_dsc : NULL;
}
