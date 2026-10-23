#pragma once
#include <lvgl/lvgl.h>
#include "board_config.h"
#include <stdint.h>
#include <stdbool.h>

#define ACCENT_PALETTE_COUNT 16
/* Native defaults only: live surfaces/text continue to use mutable styles
 * so plugin palettes (including light themes) remain authoritative.
 * KDE Breeze Dark (Plasma 6): View background and alternate, Button
 * background, Normal and Inactive text. Pressed is the text mixed 12.5% over
 * a row; border is Breeze's frame, the text mixed 20% over a card. */
#define GUI_COLOR_SCREEN 0x141618
#define GUI_COLOR_ROW 0x1D1F22
#define GUI_COLOR_PANEL 0x292C30
#define GUI_COLOR_PRESSED 0x393B3D
#define GUI_COLOR_PRIMARY 0xFCFCFC
#define GUI_COLOR_SECONDARY 0xA1A9B1
#define GUI_COLOR_BORDER 0x535659
#define GUI_ROW_GAP BOARD_SCALE_PX(8)
/* List rows are cards inset this far from each screen edge (as Home's). */
#define GUI_LIST_SIDE_MARGIN BOARD_SCALE_PX(16)
#define GUI_TEXT_INSET BOARD_SCALE_PX(24)
#define GUI_ROW_HEIGHT BOARD_SCALE_PX(96)
/* Compatibility names for call sites that need to express row content, not
 * a separate ordinary-row density. Font/content minimums may still expand. */
#define GUI_SETTINGS_ROW_HEIGHT GUI_ROW_HEIGHT
#define GUI_MUSIC_ROW_HEIGHT GUI_ROW_HEIGHT
/* Shared native-painted track thickness -- every slider except Player's
 * own progress_slider, which stays at a hardcoded 440x12 to match its
 * fixed-size progress_bg.png/progress.png art (gui_player.c's own comment
 * at that call site explains why; confirmed via the real asset files,
 * both exactly 440x12 -- LVGL doesn't stretch a bg_image to fit). */
#define SLIDER_TRACK_HEIGHT BOARD_SCALE_PX(16)
/* Knob diameter is track height plus pad on both sides -- lv_slider.c's
 * position_knob() ignores LV_PART_KNOB width/height. 32px sits just above
 * the 16px track; 4px inward white border leaves a 24px accent disc.
 * (Both scale together with SLIDER_TRACK_HEIGHT so the knob keeps the same
 * proportional relationship to the track on narrower boards.) */
#define SLIDER_KNOB_SIZE BOARD_SCALE_PX(32)
#define SLIDER_KNOB_BORDER_WIDTH BOARD_SCALE_PX(4)
#define SLIDER_KNOB_PAD ((SLIDER_KNOB_SIZE - SLIDER_TRACK_HEIGHT) / 2)

typedef enum {
    GUI_FONT_ROLE_TITLE,
    GUI_FONT_ROLE_ROW,
    GUI_FONT_ROLE_BODY,
    GUI_FONT_ROLE_SUBTEXT,
    GUI_FONT_ROLE_STATUS
} gui_font_role_t;

extern const uint32_t accent_palette[ACCENT_PALETTE_COUNT];


void gui_theme_init(void);
/* For gui_reload.c's in-process UI reload -- see its own comment in
 * gui_theme.c for why this must never call fallback_font_init_early(). */
void gui_theme_reload_styles(void);
lv_style_t * gui_theme_accent_style(void);
lv_style_t * gui_theme_accent_knob_style(void);
lv_style_t * gui_theme_accent_outline_style(void);
lv_style_t * gui_theme_muted_text_style(void);
const lv_font_t * gui_theme_font(gui_font_role_t role);
/* The accent in use: the saved pick, or the playing cover's color while
 * "Match album art" is on and a cover is showing. */
lv_color_t accent_lv_color(void);
uint32_t gui_theme_accent_rgb(void);
/* Scales a UI animation duration by the user's Animation Speed setting.
 * Never returns 0 for a nonzero base, so lv_anim always completes. */
uint32_t gui_anim_ms(uint32_t base_ms);
/* True when Animation Speed is Off. */
bool gui_anims_off(void);
/* Saves rgb as the user's accent and turns "Match album art" off. */
void gui_theme_apply_accent(uint32_t rgb);
void gui_theme_set_accent_dynamic(bool on);
/* The player reports each applied cover's color; valid is false while no
 * cover art is showing. Never saved. */
void gui_theme_set_cover_accent(bool valid, uint32_t rgb);
bool gui_theme_cover_accent(uint32_t * out_rgb);
void gui_theme_update_surface_contrast(void);

/* KDE Breeze foregrounds (Breeze Dark and Breeze Light "Normal" and
 * "Inactive" text) for surfaces whose background the theme does not paint,
 * such as the Now Playing artwork backdrop. */
#define GUI_BREEZE_TEXT_ON_DARK   0xFCFCFC
#define GUI_BREEZE_MUTED_ON_DARK  0xA1A9B1
#define GUI_BREEZE_TEXT_ON_LIGHT  0x232629
#define GUI_BREEZE_MUTED_ON_LIGHT 0x707D8A
/* Breeze "Negative" text, for destructive actions on either theme. */
#define GUI_BREEZE_NEGATIVE       0xDA4453
/* WCAG 2 thresholds: body text, and secondary or large text. */
#define GUI_CONTRAST_TEXT  4.5f
#define GUI_CONTRAST_MUTED 3.0f
/* WCAG 2 contrast ratio between two colors, 1.0 to 21.0. */
float gui_theme_contrast_ratio(lv_color_t a, lv_color_t b);
/* True when light text reads better than dark text on bg. */
bool gui_theme_prefers_light_text(lv_color_t bg);
/* True for a color closer to black than white, e.g. dark text. */
bool gui_theme_color_is_dark(lv_color_t color);
