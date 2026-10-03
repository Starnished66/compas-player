#include "gui_theme.h"
#include "gui.h"
#include "settings.h"
#include "screen_builders.h"
#include "fallback_font.h"
#include <stdio.h>

static lv_style_t style_accent;
static lv_style_t style_accent_knob;
static lv_style_t style_accent_outline;

lv_style_t * gui_theme_accent_style(void) { return &style_accent; }
lv_style_t * gui_theme_accent_knob_style(void) { return &style_accent_knob; }
lv_style_t * gui_theme_accent_outline_style(void) { return &style_accent_outline; }
lv_style_t * gui_theme_muted_text_style(void) { return &style_theme_text_muted; }

const uint32_t accent_palette[ACCENT_PALETTE_COUNT] = {
    0x2196F3, /* blue */
    0x4CAF50, /* green */
    0xF44336, /* red */
    0xFF9800, /* orange */
    0x9C27B0, /* purple */
    0x009688, /* teal */
    0xE91E63, /* pink */
    0xE0E0E0, /* light gray */
    DEFAULT_ACCENT_COLOR, /* light yellow (default) */
    0x00BCD4, /* cyan */
    0x3F51B5, /* indigo */
    0xFFC107, /* amber */
    0xCDDC39, /* lime */
    0x795548, /* brown */
    0x607D8B, /* blue gray */
    0xFFFFFF, /* white */
};

/* Effective accent (see accent_lv_color()) and the last cover's color. */
static uint32_t accent_rgb = DEFAULT_ACCENT_COLOR;
static bool cover_accent_valid;
static uint32_t cover_accent;

extern player_settings_t current_settings;
extern void settings_save(const player_settings_t * s);
extern void player_transition_mark_dirty(void);
extern void refresh_play_btn_icon(void);
extern void gui_shell_refresh_quick_drawer_toggle_accent(void);
extern void gui_settings_accent_changed(void);

const lv_font_t * gui_theme_font(gui_font_role_t role) {
    switch (role) {
        case GUI_FONT_ROLE_TITLE:   return &app_font_28;
        case GUI_FONT_ROLE_ROW:     return &app_font_22;
        case GUI_FONT_ROLE_BODY:    return &app_font_20;
        case GUI_FONT_ROLE_SUBTEXT: return &app_font_16;
        case GUI_FONT_ROLE_STATUS:  return &app_font_16;
        default:                    return &app_font_20;
    }
}

lv_color_t accent_lv_color(void) {
    return lv_color_hex(accent_rgb);
}

uint32_t gui_theme_accent_rgb(void) {
    return accent_rgb;
}

bool gui_anims_off(void) {
    return current_settings.animation_scale == 0;
}

uint32_t gui_anim_ms(uint32_t base_ms) {
    int percent = current_settings.animation_scale;
    if (percent < 0 || percent > 100) percent = 100;
    uint32_t ms = (uint32_t) (((uint64_t) base_ms * (uint32_t) percent) / 100U);
    /* LVGL's lv_anim_speed_clamped compresses times by dividing by 10 internally,
     * so an animation < 5ms truncates to 0ms. LVGL fails to fire completed 
     * callbacks on 0ms animations, leaving objects stranded out of bounds. 
     * 10ms minimum survives the division and safely acts as a 1-frame instant snap. */
    return ms < 10 && base_ms > 0 ? 10 : ms;
}

static uint32_t resolve_accent(void) {
    if (current_settings.accent_dynamic && cover_accent_valid) return cover_accent;
    return current_settings.accent_color & 0xFFFFFF;
}

void gui_theme_update_surface_contrast(void) {
    lv_style_value_t text, row, card;
    if (lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &text) != LV_STYLE_RES_FOUND ||
        lv_style_get_prop(&list_row_style, LV_STYLE_BG_COLOR, &row) != LV_STYLE_RES_FOUND ||
        lv_style_get_prop(&style_theme_card_bg, LV_STYLE_BG_COLOR, &card) != LV_STYLE_RES_FOUND) return;
    /* Contrast follows the plugin's own foreground, so a light palette
     * darkens on press instead of flashing a hardcoded charcoal surface. */
    lv_style_set_bg_color(&list_row_pressed_style, lv_color_mix(text.color, row.color, 32));
    lv_style_set_border_color(&style_theme_card_bg, lv_color_mix(text.color, card.color, 24));
    lv_obj_report_style_change(&list_row_pressed_style);
    lv_obj_report_style_change(&style_theme_card_bg);
}

/* Recolors the shared styles and the decoded accent art. Skipped when the
 * color did not change, so a new track from the same album costs nothing. */
static void refresh_accent(void) {
    uint32_t rgb = resolve_accent();
    if (rgb != accent_rgb) {
        accent_rgb = rgb;
        lv_color_t c = lv_color_hex(rgb);
        lv_style_set_bg_color(&style_accent, c);
        lv_style_set_arc_color(&style_accent, c);
        lv_style_set_text_color(&style_accent, c);
        lv_style_set_bg_image_recolor(&style_accent, c);
        lv_style_set_bg_image_recolor_opa(&style_accent, LV_OPA_COVER);
        lv_style_set_image_recolor(&style_accent, c);
        lv_style_set_image_recolor_opa(&style_accent, LV_OPA_80);
        lv_obj_report_style_change(&style_accent);

        lv_style_set_bg_color(&style_accent_knob, c);
        lv_obj_report_style_change(&style_accent_knob);

        lv_style_set_border_color(&style_accent_outline, c);
        lv_obj_report_style_change(&style_accent_outline);

        player_transition_mark_dirty();
        /* Play/pause art is a white disc with a baked-in cyan glyph -- LVGL
         * image_recolor would tint the disc too, so the glyph is rewritten in
         * decoded pixels (see refresh_play_btn_icon()). */
        refresh_play_btn_icon();
        /* Same deal for the quick drawer's "on" toggle icons: a baked-in
         * #009FF6 circle under a near-white glyph. */
        gui_shell_refresh_quick_drawer_toggle_accent();
    }
    gui_settings_accent_changed();
}

void gui_theme_apply_accent(uint32_t rgb) {
    current_settings.accent_color = rgb & 0xFFFFFF;
    current_settings.accent_dynamic = false;
    settings_save(&current_settings);
    refresh_accent();
}

void gui_theme_set_accent_dynamic(bool on) {
    current_settings.accent_dynamic = on;
    settings_save(&current_settings);
    refresh_accent();
}

void gui_theme_set_cover_accent(bool valid, uint32_t rgb) {
    if (valid == cover_accent_valid && (!valid || rgb == cover_accent)) return;
    cover_accent_valid = valid;
    cover_accent = valid ? (rgb & 0xFFFFFF) : 0;
    if (current_settings.accent_dynamic) refresh_accent();
}

bool gui_theme_cover_accent(uint32_t * out_rgb) {
    if (cover_accent_valid && out_rgb) *out_rgb = cover_accent;
    return cover_accent_valid;
}

/* Shared by gui_theme_init() (real boot) and gui_theme_reload_styles()
 * (gui_reload.c's in-process UI reload) -- everything EXCEPT
 * fallback_font_init_early(), which must never run a second time (see
 * gui_theme_reload_styles()'s own comment). */
static void init_style_objects(void) {
    /* gui_reload.c's in-process UI reload calls this a second (or Nth) time,
     * and lv_style_init() on a style that already has properties leaks the
     * old values_and_props allocation instead of freeing it (LVGL's own
     * header comment on lv_style_init()). Skip the reset on the very first
     * call, when these are still freshly zero-initialized statics. */
    static bool already_initialized = false;
    if (already_initialized) {
        lv_style_reset(&style_accent);
        lv_style_reset(&style_accent_knob);
        lv_style_reset(&style_accent_outline);
    }
    already_initialized = true;
    accent_rgb = resolve_accent();

    lv_style_init(&style_accent);
    lv_style_set_bg_color(&style_accent, accent_lv_color());
    lv_style_set_arc_color(&style_accent, accent_lv_color());
    lv_style_set_text_color(&style_accent, accent_lv_color());
    lv_style_set_bg_image_recolor(&style_accent, accent_lv_color());
    lv_style_set_bg_image_recolor_opa(&style_accent, LV_OPA_COVER);
    lv_style_set_image_recolor(&style_accent, accent_lv_color());
    lv_style_set_image_recolor_opa(&style_accent, LV_OPA_80);

    lv_style_init(&style_accent_knob);
    lv_style_set_bg_color(&style_accent_knob, accent_lv_color());
    lv_style_set_bg_opa(&style_accent_knob, LV_OPA_COVER);
    lv_style_set_border_color(&style_accent_knob, lv_color_white());
    lv_style_set_border_width(&style_accent_knob, SLIDER_KNOB_BORDER_WIDTH);
    lv_style_set_border_opa(&style_accent_knob, LV_OPA_COVER);
    lv_style_set_radius(&style_accent_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&style_accent_knob, SLIDER_KNOB_PAD);

    lv_style_init(&style_accent_outline);
    lv_style_set_border_color(&style_accent_outline, accent_lv_color());
    lv_style_set_border_width(&style_accent_outline, 2);
    lv_style_set_border_opa(&style_accent_outline, LV_OPA_COVER);
    lv_style_set_bg_opa(&style_accent_outline, 0); /* outline only, no fill -- callers that want a fill set their own bg_color/bg_opa separately */

    screen_builders_init_list_row_style();
}

void gui_theme_init(void) {
    init_style_objects();
    fallback_font_init_early(current_settings.font_size_tier, current_settings.lyrics_font_size_tier);
    screen_builders_refresh_font_geometry(NULL);
}

/* For gui_reload.c's in-process UI reload -- everything gui_theme_init()
 * does EXCEPT fallback_font_init_early(). That function is boot-only, by
 * design: it always rebuilds every app_font_* slot with include_fallbacks
 * = false (src/ui/fallback_font.c), deferring the CJK/Korean/Thai fallback
 * face load to run later, once, in the background
 * (fallback_font_schedule_deferred_load()/fallback_font_load_now()). A
 * reload re-running it would silently drop that already-loaded fallback
 * chain (breaking non-Latin glyphs for the rest of the session) AND leak
 * the fallback faces already in s_loaded_faces[], since fallback_font_init_
 * early() replaces that table without freeing what it's replacing -- real
 * memory, not just a stale pointer, since these are loaded TTF/OTF font
 * files. A theme/icon reload has no reason to touch fonts at all, so this
 * just skips that call entirely rather than trying to reconstruct
 * fallback_font.c's own s_fallback_loaded state from outside it. */
void gui_theme_reload_styles(void) {
    init_style_objects();
    screen_builders_refresh_font_geometry(NULL);
}

