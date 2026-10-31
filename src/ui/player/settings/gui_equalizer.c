#include "gui_equalizer.h"
#include "../../settings/gui_settings.h"
#include "../../application/gui.h" /* Legacy declaration of basename_of(). */
#include "../../widgets/builders/screen_builders.h"
#include "../../text/rendering/gui_text_input.h"
#include "../../shell/gui_notifications.h"
#include "../../shell/gui_navigation.h"
#include "../../appearance/gui_theme.h"
#include "../../text/i18n/i18n.h"
#include "../../text/rendering/fallback_font.h"
#include "../../assets/assets.h"
#include "../../../audio/effects/peq.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PEQ_PROFILES_DIR MUSIC_ROOT_DIR "/PEQ_Profiles"

static lv_event_cb_t eq_profiles_download_cb;
static bool (*eq_profiles_download_available)(void);
static lv_obj_t * settings_crossfeed_screen;

static lv_obj_t * build_eq_screen(void);
static lv_obj_t * build_eq_band_options_screen(void);
static lv_obj_t * build_eq_profiles_screen(void);
static lv_obj_t * build_crossfeed_screen(void);
static void build_eq_profile_delete_popup(void);
static void build_eq_save_choice_popup(void);

static lv_obj_t * eq_screen;
static lv_obj_t * eq_profiles_screen;
static char eq_current_profile_name[256];
static void eq_profile_display_name(const char * name, char * out, size_t size) {
    if (!name || !name[0]) name = TR("Custom");
    if (strncmp(name, "AutoEQ - ", 9) == 0) {
        snprintf(out, size, "%s", name + 9);
        char * suffix = strrchr(out, ' ');
        if (suffix && suffix >= out + 2 && suffix[-1] == '-' && suffix[-2] == ' ' &&
            strlen(suffix + 1) == 10 && strspn(suffix + 1, "0123456789abcdefABCDEF") == 10)
            suffix[-2] = '\0';
    } else snprintf(out, size, "%s", name);
}

static lv_obj_t * eq_bypass_switch;
static lv_obj_t * eq_band_enabled_switch;
static lv_obj_t * eq_preamp_slider;
static lv_obj_t * eq_preamp_value_label;
static lv_obj_t * eq_crossfeed_slider;
static lv_obj_t * eq_crossfeed_value_label;
static lv_obj_t * eq_crossfeed_angle_dropdown;
static lv_obj_t * eq_freq_slider;
static lv_obj_t * eq_gain_slider;
static lv_obj_t * eq_q_slider;
static lv_obj_t * eq_type_dropdown;
static lv_obj_t * eq_freq_value_label;
static lv_obj_t * eq_gain_value_label;
static lv_obj_t * eq_q_value_label;
static int current_eq_band = 0;
static lv_obj_t * eq_band_options_screen;
static lv_obj_t * eq_band_number_label;
static lv_obj_t * eq_profile_button_label;
static lv_obj_t * eq_main_content;
static lv_obj_t * eq_footer;
static lv_obj_t * eq_bypass_state_label;
static lv_obj_t * eq_title_label;
static bool eq_graph_visible;
static lv_obj_t * eq_graph_panel, *eq_graph_chart, *eq_graph_caption;
static lv_obj_t * eq_graph_reference;
static lv_chart_series_t * eq_graph_series;
static lv_obj_t * eq_graph_min_label, *eq_graph_low_label, *eq_graph_zero_label;
static lv_obj_t * eq_graph_high_label, *eq_graph_max_label;
static lv_obj_t * eq_band_options_row, *eq_preamp_card, *eq_freq_card, *eq_gain_card, *eq_q_card;
static void refresh_all_eq_widgets(void);
static void eq_refresh_graph(void);

void gui_settings_refresh_font_geometry(void) {
    if (eq_profile_button_label)
        lv_obj_set_height(eq_profile_button_label, lv_font_get_line_height(lv_obj_get_style_text_font(eq_profile_button_label, 0)));
    if (eq_graph_chart) lv_obj_set_style_pad_ver(eq_graph_chart,
        lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) / 2, 0);
    if (eq_screen) lv_obj_update_layout(eq_screen);
    if (eq_band_options_screen) lv_obj_update_layout(eq_band_options_screen);
    if (settings_crossfeed_screen) lv_obj_update_layout(settings_crossfeed_screen);
    if (eq_title_label) lv_obj_set_style_text_font(eq_title_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
}

#define EQ_FREQ_MIN_HZ 20.0
#define EQ_FREQ_MAX_HZ 20000.0
#define EQ_FREQ_SLIDER_MAX 1000
#define EQ_GRAPH_X_MAX 1000000
#define EQ_GRAPH_BASE_POINTS 512
#define EQ_GRAPH_MAX_POINTS (EQ_GRAPH_BASE_POINTS + PEQ_NUM_BANDS)

static int eq_compare_freq(const void * a, const void * b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Both sample positions and grid ticks use the scatter chart's integer X
 * coordinates. Let LVGL do the same pixel rounding for each. */
static int32_t eq_graph_frequency_x(double frequency) {
    return (int32_t)lround(EQ_GRAPH_X_MAX * log(frequency / EQ_FREQ_MIN_HZ) /
                          log(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ));
}

static int32_t freq_to_slider(double freq_hz) {
    if (freq_hz < EQ_FREQ_MIN_HZ) freq_hz = EQ_FREQ_MIN_HZ;
    if (freq_hz > EQ_FREQ_MAX_HZ) freq_hz = EQ_FREQ_MAX_HZ;
    double ratio = log(freq_hz / EQ_FREQ_MIN_HZ) / log(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ);
    return (int32_t) (ratio * EQ_FREQ_SLIDER_MAX);
}

static double slider_to_freq(int32_t slider_val) {
    double ratio = (double) slider_val / (double) EQ_FREQ_SLIDER_MAX;
    return EQ_FREQ_MIN_HZ * pow(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ, ratio);
}

static void eq_format_gain(char * out, size_t out_size, double value) {
    double tenths = round(value * 10.0);
    if (fabs(value * 10.0 - tenths) < 0.0001) snprintf(out, out_size, "%+.1f dB", value);
    else snprintf(out, out_size, "%+.2f dB", value);
}

static void eq_format_q(char * out, size_t out_size, double value) {
    double tenths = round(value * 10.0);
    if (fabs(value * 10.0 - tenths) < 0.0001) snprintf(out, out_size, "%.1f", value);
    else snprintf(out, out_size, "%.2f", value);
}

/* Which numeric field a tap-to-edit label represents -- passed through
 * show_text_entry()'s user_data so one shared done-callback can update the
 * right slider/label/peq setter instead of needing four near-identical
 * callbacks. */
typedef enum {
    EQ_FIELD_PREAMP,
    EQ_FIELD_FREQ,
    EQ_FIELD_GAIN,
    EQ_FIELD_Q,
    EQ_FIELD_CROSSFEED,
} eq_field_t;

static void eq_format_crossfeed(char * out, size_t out_size, double strength) {
    if (strength <= 0.0) snprintf(out, out_size, "%s", TR("Crossfeed: Off"));
    else snprintf(out, out_size, TR("Crossfeed: %d%%"), (int) lround(strength * 100.0));
}

static void eq_numeric_entry_done_cb(const char * text, void * user_data) {
    eq_field_t field = (eq_field_t) (intptr_t) user_data;
    double val = atof(text);
    char formatted[32];
    const peq_band_t * band;

    switch (field) {
        case EQ_FIELD_PREAMP:
            if (val < -12.0) val = -12.0;
            if (val > 12.0) val = 12.0;
            peq_set_preamp_db(val);
            if (eq_preamp_slider) lv_slider_set_value(eq_preamp_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            if (eq_preamp_value_label) lv_label_set_text_fmt(eq_preamp_value_label, TR("%+.2f dB"), val);
            if (eq_graph_visible) eq_refresh_graph();
            peq_save();
            break;
        case EQ_FIELD_CROSSFEED:
            if (val < 0.0) val = 0.0;
            if (val > 100.0) val = 100.0;
            peq_set_crossfeed(val / 100.0);
            if (eq_crossfeed_slider) lv_slider_set_value(eq_crossfeed_slider, (int32_t) lround(val), LV_ANIM_OFF);
            eq_format_crossfeed(formatted, sizeof(formatted), peq_get_crossfeed());
            if (eq_crossfeed_value_label) lv_label_set_text(eq_crossfeed_value_label, formatted);
            peq_save();
            break;
        case EQ_FIELD_FREQ:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < EQ_FREQ_MIN_HZ) val = EQ_FREQ_MIN_HZ;
            if (val > EQ_FREQ_MAX_HZ) val = EQ_FREQ_MAX_HZ;
            peq_set_band(current_eq_band, val, band->gain_db, band->q);
            if (eq_freq_slider) lv_slider_set_value(eq_freq_slider, freq_to_slider(val), LV_ANIM_OFF);
            if (eq_freq_value_label) lv_label_set_text_fmt(eq_freq_value_label, TR("%.0f Hz"), val);
            peq_save();
            break;
        case EQ_FIELD_GAIN:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < -12.0) val = -12.0;
            if (val > 12.0) val = 12.0;
            peq_set_band(current_eq_band, band->freq_hz, val, band->q);
            if (eq_gain_slider) lv_slider_set_value(eq_gain_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            eq_format_gain(formatted, sizeof(formatted), val);
            if (eq_gain_value_label) lv_label_set_text(eq_gain_value_label, formatted);
            peq_save();
            break;
        case EQ_FIELD_Q:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < 0.1) val = 0.1;
            if (val > 10.0) val = 10.0;
            peq_set_band(current_eq_band, band->freq_hz, band->gain_db, val);
            if (eq_q_slider) lv_slider_set_value(eq_q_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            eq_format_q(formatted, sizeof(formatted), val);
            if (eq_q_value_label) lv_label_set_text(eq_q_value_label, formatted);
            peq_save();
            break;
    }
}

/* Tap-to-edit: attached to each slider card's value label (see
 * create_eq_slider_card() / eq_make_slider_card()) -- opens the shared numeric keypad pre-filled
 * with the field's current exact value, for typing a precise number
 * instead of only dragging a slider. */
static void eq_field_label_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_field_t field = (eq_field_t) (intptr_t) lv_event_get_user_data(e);
    const peq_band_t * band;
    char buf[32];
    const char * title;

    switch (field) {
        case EQ_FIELD_PREAMP:
            title = TR("Pre-Amp (dB, -12 to 12)");
            snprintf(buf, sizeof(buf), "%.2f", peq_get_preamp_db());
            break;
        case EQ_FIELD_CROSSFEED:
            title = TR("Crossfeed (0 to 100)");
            snprintf(buf, sizeof(buf), "%.0f", peq_get_crossfeed() * 100.0);
            break;
        case EQ_FIELD_FREQ:
            title = TR("Frequency (Hz, 20 to 20000)");
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.0f", band ? band->freq_hz : 0.0);
            break;
        case EQ_FIELD_GAIN:
            title = TR("Gain (dB, -12 to 12)");
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.2f", band ? band->gain_db : 0.0);
            break;
        case EQ_FIELD_Q:
        default:
            title = "Q (0.1 to 10)";
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.2f", band ? band->q : 0.0);
            break;
    }

    show_text_entry(title, buf, false, true, eq_numeric_entry_done_cb, (void *) (intptr_t) field);
}

static void eq_screen_loaded_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) refresh_all_eq_widgets();
}

static void crossfeed_screen_loaded_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) refresh_all_eq_widgets();
}

void gui_equalizer_eq_row_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(eq_screen);
}

void gui_equalizer_crossfeed_row_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (settings_crossfeed_screen) nav_push(settings_crossfeed_screen);
}

static void eq_bypass_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Switch shows "EQ Enabled" -- checked means NOT bypassed. */
    peq_set_bypass(!lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    peq_save();
    if (eq_bypass_state_label)
        lv_label_set_text(eq_bypass_state_label, peq_get_bypass() ? TR("OFF") : TR("ON"));
    refresh_all_eq_widgets();
}

static void refresh_eq_band_widgets(void) {
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    char formatted[32];

    if (eq_band_enabled_switch) {
        if (band->enabled) lv_obj_add_state(eq_band_enabled_switch, LV_STATE_CHECKED);
        else lv_obj_clear_state(eq_band_enabled_switch, LV_STATE_CHECKED);
    }

    if (eq_type_dropdown) lv_dropdown_set_selected(eq_type_dropdown, (uint32_t) band->type);

    if (eq_freq_slider) lv_slider_set_value(eq_freq_slider, freq_to_slider(band->freq_hz), LV_ANIM_OFF);
    if (eq_gain_slider) lv_slider_set_value(eq_gain_slider, (int32_t) (band->gain_db * 10.0), LV_ANIM_OFF);
    if (eq_q_slider) lv_slider_set_value(eq_q_slider, (int32_t) (band->q * 10.0), LV_ANIM_OFF);

    if (eq_freq_value_label) lv_label_set_text_fmt(eq_freq_value_label, TR("%.0f Hz"), band->freq_hz);
    eq_format_gain(formatted, sizeof(formatted), band->gain_db);
    if (eq_gain_value_label) lv_label_set_text(eq_gain_value_label, formatted);
    eq_format_q(formatted, sizeof(formatted), band->q);
    if (eq_q_value_label) lv_label_set_text(eq_q_value_label, formatted);
    if (eq_band_number_label) {
        if (eq_graph_visible) lv_label_set_text(eq_band_number_label, TR("EQ curve"));
        else lv_label_set_text_fmt(eq_band_number_label, TR("Band %d / %d"), current_eq_band + 1, PEQ_NUM_BANDS);
    }
}

static void eq_band_step_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int delta = (int) (intptr_t) lv_event_get_user_data(e);
    int stop = (eq_graph_visible ? PEQ_NUM_BANDS : current_eq_band) + (delta < 0 ? -1 : 1);
    if (stop < 0) stop = PEQ_NUM_BANDS;
    if (stop > PEQ_NUM_BANDS) stop = 0;
    eq_graph_visible = (stop == PEQ_NUM_BANDS);
    if (!eq_graph_visible) current_eq_band = stop;
    lv_obj_t * band_widgets[] = {eq_band_options_row, eq_preamp_card, eq_freq_card, eq_gain_card, eq_q_card};
    for (size_t i = 0; i < sizeof(band_widgets) / sizeof(band_widgets[0]); ++i) {
        if (!band_widgets[i]) continue;
        if (eq_graph_visible) lv_obj_add_flag(band_widgets[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(band_widgets[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (eq_graph_panel) {
        if (eq_graph_visible) lv_obj_clear_flag(eq_graph_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(eq_graph_panel, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_all_eq_widgets();
}

static void eq_band_options_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && !eq_graph_visible && eq_band_options_screen) nav_push(eq_band_options_screen);
}

static void eq_type_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    peq_set_band_type(current_eq_band, (peq_band_type_t) lv_dropdown_get_selected(lv_event_get_target(e)));
    peq_save();
}

static void eq_preamp_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    double db = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_preamp_db(db);
        lv_label_set_text_fmt(eq_preamp_value_label, TR("%+.2f dB"), db);
        if (eq_graph_visible) eq_refresh_graph();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        peq_save();
    }
}

static void eq_crossfeed_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    double strength = (double) lv_slider_get_value(lv_event_get_target(e)) / 100.0;
    if (code == LV_EVENT_VALUE_CHANGED) {
        char formatted[32];
        peq_set_crossfeed(strength);
        eq_format_crossfeed(formatted, sizeof(formatted), strength);
        lv_label_set_text(eq_crossfeed_value_label, formatted);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        peq_save();
    }
}

static void eq_band_enabled_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    peq_set_band_enabled(current_eq_band, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    peq_save();
}

static void eq_freq_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double freq = slider_to_freq(lv_slider_get_value(lv_event_get_target(e)));

    /* Format string with label prefix consistently during active dragging
     * and when released. */
    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, freq, band->gain_db, band->q);
        lv_label_set_text_fmt(eq_freq_value_label, TR("%.0f Hz"), freq);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        peq_save();
    }
}

static void eq_gain_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double gain = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        char formatted[32];
        peq_set_band(current_eq_band, band->freq_hz, gain, band->q);
        eq_format_gain(formatted, sizeof(formatted), gain);
        lv_label_set_text(eq_gain_value_label, formatted);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        peq_save();
    }
}

static void eq_q_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double q = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        char formatted[32];
        peq_set_band(current_eq_band, band->freq_hz, band->gain_db, q);
        eq_format_q(formatted, sizeof(formatted), q);
        lv_label_set_text(eq_q_value_label, formatted);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        peq_save();
    }
}

void gui_settings_open_eq(void) {
    if (!eq_screen) return;
    nav_push(eq_screen);
}

/* EQ slider card with value label on top and slider track below.
 * Card uses flex-column flow with content-based sizing so card height
 * dynamically adjusts to the active font tier without overlapping. */
static lv_obj_t * create_eq_slider_card(lv_obj_t * parent, eq_field_t field, lv_obj_t ** out_value_label,
                                        lv_obj_t ** out_slider, int32_t range_min, int32_t range_max) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(96));
    /* Flex column with content height accommodates varying font heights. */
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_all(card, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(8), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Clickable (tap-to-edit, see eq_field_label_click_cb()) -- the whole
     * label area is the tap target, not just the text glyphs, via
     * lv_obj_set_ext_click_area() below. */
    lv_obj_t * value_label = lv_label_create(card);
    lv_obj_set_width(value_label, lv_pct(100));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_flag(value_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(value_label, 16);
    lv_obj_add_event_cb(value_label, eq_field_label_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) field);

    /* Size the rail from the knob diameter, so its outer edges line up
     * with the text inset at both ends, at every display resolution. */
    lv_obj_t * slider_row = lv_obj_create(card);
    lv_obj_set_size(slider_row, lv_pct(100), SLIDER_KNOB_SIZE + BOARD_SCALE_PX(8));
    lv_obj_set_style_bg_opa(slider_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(slider_row, 0, 0);
    lv_obj_set_style_pad_all(slider_row, 0, 0);
    /* LVGL places the knob on the indicator end, one pixel left of the rail. */
    lv_obj_set_style_pad_left(slider_row, SLIDER_TRACK_HEIGHT / 2 + SLIDER_KNOB_PAD + 1, 0);
    lv_obj_set_style_pad_right(slider_row, SLIDER_TRACK_HEIGHT - 1 - SLIDER_TRACK_HEIGHT / 2 + SLIDER_KNOB_PAD, 0);
    lv_obj_remove_flag(slider_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * slider = lv_slider_create(slider_row);
    lv_obj_set_width(slider, lv_pct(100));
    lv_obj_center(slider);
    lv_obj_set_height(slider, SLIDER_TRACK_HEIGHT);
    lv_slider_set_range(slider, range_min, range_max);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_knob_style(), LV_PART_KNOB);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(slider, SLIDER_KNOB_PAD + BOARD_SCALE_PX(4));

    *out_value_label = value_label;
    *out_slider = slider;
    return card;
}

/* ---- PEQ named profiles (save/load to/from the SD card) ----
 *
 * peq.c's own peq_load()/peq_save() always target one fixed, always-current
 * file (loaded at startup, saved on every change) -- these profiles are a
 * separate, explicit "keep this exact setup around under a name" action,
 * living in their own SD card folder rather than mixed in with music. */
static lv_obj_t * eq_profiles_list;
static lv_obj_t * eq_profiles_title_label;
/* Name of the named profile whose values are currently loaded.  The PEQ
 * engine deliberately knows only about values and its always-current
 * autosave file, so the UI owns this bit of presentation state.  Saving
 * with this unchanged overwrites that profile; editing it creates a new
 * profile and makes the new name current. */

static void eq_set_current_profile_from_path(const char * path) {
    const char * name = basename_of(path);
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", name ? name : "");
    char * dot = strrchr(eq_current_profile_name, '.');
    if (dot && strcmp(dot, ".peq") == 0) *dot = '\0';
}

/* Refreshes every widget on the EQ screen from peq.c's current state --
 * shared by the initial build and by "Load Profile" (which changes
 * everything at once, unlike the individual per-field setters that only
 * ever touch one widget). */
static void refresh_all_eq_widgets(void) {
    if (eq_bypass_switch) {
        if (peq_get_bypass()) lv_obj_clear_state(eq_bypass_switch, LV_STATE_CHECKED);
        else lv_obj_add_state(eq_bypass_switch, LV_STATE_CHECKED);
    }
    if (eq_bypass_state_label) lv_label_set_text(eq_bypass_state_label, peq_get_bypass() ? TR("OFF") : TR("ON"));
    if (eq_preamp_slider) lv_slider_set_value(eq_preamp_slider, (int32_t) (peq_get_preamp_db() * 10.0), LV_ANIM_OFF);
    if (eq_preamp_value_label) lv_label_set_text_fmt(eq_preamp_value_label, TR("%+.2f dB"), peq_get_preamp_db());
    if (eq_crossfeed_slider) lv_slider_set_value(eq_crossfeed_slider, (int32_t) lround(peq_get_crossfeed() * 100.0), LV_ANIM_OFF);
    if (eq_crossfeed_value_label) {
        char formatted[32];
        eq_format_crossfeed(formatted, sizeof(formatted), peq_get_crossfeed());
        lv_label_set_text(eq_crossfeed_value_label, formatted);
    }
    if (eq_crossfeed_angle_dropdown) {
        int angle = peq_get_crossfeed_angle();
        uint32_t selected = 0;
        if (angle == 30) selected = 1;
        else if (angle == 45) selected = 2;
        else if (angle == 60) selected = 3;
        else if (angle == 90) selected = 4;
        lv_dropdown_set_selected(eq_crossfeed_angle_dropdown, selected);
    }
    refresh_eq_band_widgets();
    if (eq_profile_button_label) {
        char name[256];
        eq_profile_display_name(eq_current_profile_name, name, sizeof(name));
        lv_label_set_text(eq_profile_button_label, name);
    }
    eq_refresh_graph();
}

static void eq_refresh_graph(void) {
    if (eq_graph_visible && eq_graph_chart && eq_graph_series) {
        double f[EQ_GRAPH_MAX_POINTS], db[EQ_GRAPH_MAX_POINTS], peak = 0.0;
        size_t count = EQ_GRAPH_BASE_POINTS;
        for (size_t i = 0; i < EQ_GRAPH_BASE_POINTS; ++i)
            f[i] = EQ_FREQ_MIN_HZ * pow(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ, (double)i / (EQ_GRAPH_BASE_POINTS - 1));
        for (int i = 0; i < PEQ_NUM_BANDS; ++i) {
            const peq_band_t * band = peq_get_band(i);
            if (band && band->enabled && isfinite(band->freq_hz) && band->freq_hz >= EQ_FREQ_MIN_HZ && band->freq_hz <= EQ_FREQ_MAX_HZ)
                f[count++] = band->freq_hz;
        }
        qsort(f, count, sizeof(f[0]), eq_compare_freq);
        size_t unique = 0;
        for (size_t i = 0; i < count; ++i)
            if (!unique || f[i] != f[unique - 1]) f[unique++] = f[i];
        count = unique;
        peq_get_filter_response_db(f, db, count, 48000);
        for (size_t i = 0; i < count; ++i) if (isfinite(db[i]) && fabs(db[i]) > peak) peak = fabs(db[i]);
        /* Match the centi-dB samples, so coefficient rounding just above an
         * exact 6 dB step does not unnecessarily expand the vertical scale. */
        int range = (((int)lround(peak * 100.0) + 599) / 600) * 6;
        if (range < 12) range = 12;
        if (range > 144) range = 144;
        if (eq_graph_min_label) lv_label_set_text_fmt(eq_graph_min_label, "-%d", range);
        if (eq_graph_low_label) lv_label_set_text_fmt(eq_graph_low_label, "-%d", range / 2);
        if (eq_graph_zero_label) lv_label_set_text(eq_graph_zero_label, TR("0 dB"));
        if (eq_graph_high_label) lv_label_set_text_fmt(eq_graph_high_label, "+%d", range / 2);
        if (eq_graph_max_label) lv_label_set_text_fmt(eq_graph_max_label, "+%d", range);
        lv_chart_set_range(eq_graph_chart, LV_CHART_AXIS_PRIMARY_Y, -range * 100, range * 100);
        lv_chart_set_axis_range(eq_graph_chart, LV_CHART_AXIS_PRIMARY_X, 0, EQ_GRAPH_X_MAX);
        lv_chart_set_point_count(eq_graph_chart, (uint32_t)count);
        for (size_t i = 0; i < count; ++i) {
            int value = isfinite(db[i]) ? (int)lround(db[i] * 100.0) : 0;
            if (value < -range * 100) value = -range * 100;
            if (value > range * 100) value = range * 100;
            int32_t x = eq_graph_frequency_x(f[i]);
            lv_chart_set_series_value_by_id2(eq_graph_chart, eq_graph_series, (uint32_t)i, x, value);
        }
        lv_chart_refresh(eq_graph_chart);
        if (eq_graph_caption) lv_label_set_text(eq_graph_caption, peq_get_bypass() ? TR("EQ correction (dB) · EQ off") : TR("EQ correction (dB)"));
        if (eq_graph_reference) {
            double preamp = peq_get_preamp_db();
            if (!isfinite(preamp)) preamp = 0.0;
            lv_label_set_text_fmt(eq_graph_reference, TR("48 kHz · Preamp excluded (%+.1f dB)"), preamp);
        }
    }
}


static char ** eq_profile_paths = NULL;
static int eq_profile_count = 0;
static bool eq_profiles_edit_mode = false;
/* When true, populate_eq_profiles_screen() wires rows to overwrite the
 * tapped profile with the CURRENT PEQ values instead of loading it --
 * entered only via the Save flow's "Replace Existing" choice (see
 * eq_open_save_choice_popup()), never a standalone navigation target. */
static bool eq_profiles_replace_mode = false;
static lv_obj_t * eq_profiles_edit_btn = NULL;
static char eq_profile_pending_path[512];
static gui_popup_t eq_profile_delete_popup;

static void eq_profiles_free_paths(void) {
    for (int i = 0; i < eq_profile_count; i++) free(eq_profile_paths[i]);
    free(eq_profile_paths);
    eq_profile_paths = NULL;
    eq_profile_count = 0;
}

static bool eq_profile_name_is_valid(const char * text) {
    if (!text || text[0] == '\0') return false;
    if (strchr(text, '/') || strchr(text, '\\')) return false;
    return true;
}

static void eq_profile_path_from_name(char * out, size_t out_size, const char * name) {
    snprintf(out, out_size, "%s/%s.peq", PEQ_PROFILES_DIR, name);
}

static bool eq_save_named_profile(const char * name) {
    if (!eq_profile_name_is_valid(name)) {
        show_error_toast(TR("Invalid profile name"));
        return false;
    }
    mkdir(PEQ_PROFILES_DIR, 0755);
    char path[512];
    eq_profile_path_from_name(path, sizeof(path), name);
    if (!peq_save_to_path(path)) {
        show_error_toast(TR("Failed to save profile"));
        return false;
    }
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", name);
    refresh_all_eq_widgets();
    show_info_toast(TR("Profile saved"));
    return true;
}

static void eq_profile_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    if (!peq_load_from_path(path)) {
        show_error_toast(TR("Failed to load profile"));
        return;
    }
    eq_set_current_profile_from_path(path);
    refresh_all_eq_widgets();
    peq_save();
    nav_pop();
    show_info_toast(TR("Profile loaded"));
}

/* Replace-mode row tap: overwrites the tapped profile's file with the
 * CURRENT PEQ values (the opposite direction of eq_profile_row_cb's load) --
 * only reachable via the Save flow's "Replace Existing" choice, so tapping a
 * row here is already the user's one deliberate confirming action, same as
 * a normal in-place Save never asking again.
 * Writes to `path` directly (already a real, scanned, existing file -- no
 * need to re-validate a name or reconstruct it via eq_profile_path_from_name())
 * and only commits eq_current_profile_name/leaves the screen on success --
 * committing identity before the write could succeed would leave a FAILED
 * overwrite (SD full, unmount mid-write) pointing the next in-place Save at
 * the tapped file instead of whatever was actually current before this tap. */
static void eq_profile_replace_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    if (!peq_save_to_path(path)) {
        show_error_toast(TR("Failed to save profile"));
        return;
    }
    eq_set_current_profile_from_path(path);
    refresh_all_eq_widgets();
    show_info_toast(TR("Profile saved"));
    nav_pop();
}

static void hide_eq_profile_delete_popup(void) {
    gui_popup_hide(&eq_profile_delete_popup);
}

static void eq_profile_delete_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
}

static void eq_profile_delete_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
}

static void populate_eq_profiles_screen(void);

static void eq_profile_delete_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
    if (eq_profile_pending_path[0] == '\0') return;
    if (unlink(eq_profile_pending_path) != 0) {
        show_error_toast(TR("Failed to delete profile"));
        return;
    }
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", eq_current_profile_name);
    eq_set_current_profile_from_path(eq_profile_pending_path);
    if (strcmp(eq_current_profile_name, stem) == 0) eq_current_profile_name[0] = '\0';
    else snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", stem);
    eq_profile_pending_path[0] = '\0';
    populate_eq_profiles_screen();
    refresh_all_eq_widgets();
    show_info_toast(TR("Profile deleted"));
}

static void eq_profile_delete_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    snprintf(eq_profile_pending_path, sizeof(eq_profile_pending_path), "%s", path);
    gui_popup_show(&eq_profile_delete_popup);
}

static void eq_profile_rename_done_cb(const char * text, void * user_data) {
    const char * old_path = (const char *) user_data;
    if (!eq_profile_name_is_valid(text)) {
        show_error_toast(TR("Invalid profile name"));
        return;
    }
    char new_path[512];
    eq_profile_path_from_name(new_path, sizeof(new_path), text);
    if (strcmp(old_path, new_path) == 0) return;
    if (rename(old_path, new_path) != 0) {
        show_error_toast(TR("Failed to rename profile"));
        return;
    }
    char previous[256];
    snprintf(previous, sizeof(previous), "%s", eq_current_profile_name);
    eq_set_current_profile_from_path(old_path);
    if (strcmp(eq_current_profile_name, previous) == 0)
        snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", text);
    else
        snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", previous);
    populate_eq_profiles_screen();
    refresh_all_eq_widgets();
    show_info_toast(TR("Profile renamed"));
}

static void eq_profile_rename_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    char display[256];
    snprintf(display, sizeof(display), "%s", basename_of(path));
    char * dot = strrchr(display, '.');
    if (dot && strcmp(dot, ".peq") == 0) *dot = '\0';
    show_text_entry(TR("Rename Profile"), display, false, false, eq_profile_rename_done_cb, (void *) path);
}

static void populate_eq_profiles_screen(void) {
    lv_obj_clean(eq_profiles_list);
    eq_profiles_free_paths();

    if (eq_profiles_title_label)
        lv_label_set_text(eq_profiles_title_label, eq_profiles_replace_mode ? TR("Replace Profile") : TR("Profiles"));
    if (eq_profiles_edit_btn) {
        lv_label_set_text(eq_profiles_edit_btn, eq_profiles_edit_mode ? TR("Done") : TR("Edit"));
        /* Rename/delete don't make sense mid-replace -- hide the entry point
         * entirely rather than let a mode switch there leave replace_mode
         * armed underneath a "Done" tap that returns to it unexpectedly. */
        if (eq_profiles_replace_mode) lv_obj_add_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (!peq_scan_profiles(PEQ_PROFILES_DIR, &eq_profile_paths, &eq_profile_count)) {
        lv_obj_t * label = lv_label_create(eq_profiles_list);
        lv_label_set_text(label, TR("No saved profiles"));
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        /* Preserve the default 16 px primary face and add multilingual fallback.
         * Constrain long translations to the list content width. */
        lv_obj_set_style_text_font(label, fallback_font_fixed(16), 0);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        if (!eq_profiles_edit_mode && !eq_profiles_replace_mode &&
            eq_profiles_download_available())
            add_pill_chevron_row(eq_profiles_list, TR("Download profiles"), eq_profiles_download_cb);
        return;
    }

    for (int i = 0; i < eq_profile_count; i++) {
        lv_obj_t * row = lv_obj_create(eq_profiles_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, ui_list_row_height());
        lv_obj_add_style(row, &native_row_min_style, 0);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_add_style(row, &pill_row_bg_style, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char display[256], stem[256];
        snprintf(stem, sizeof(stem), "%s", basename_of(eq_profile_paths[i]));
        char * dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        eq_profile_display_name(stem, display, sizeof(display));

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, display);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
        const int32_t name_w = LIST_ROW_WIDTH - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(64);
        lv_obj_set_width(label, name_w);
        const bool tall = ui_row_fit_label(row, label, gui_theme_font(GUI_FONT_ROLE_ROW),
                                           LIST_ROW_WIDTH, LIST_ROW_LABEL_INSET, name_w);

        if (eq_profiles_edit_mode) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_rename_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
            lv_obj_t * delete_icon = lv_image_create(row);
            if (tall) lv_obj_add_flag(delete_icon, LV_OBJ_FLAG_FLOATING);
            lv_image_set_src(delete_icon, asset_path("touch_list/del.png"));
#if defined(BOARD_TEMPOTEC_V1)
            lv_obj_set_size(delete_icon, BOARD_SCALE_PX(34), BOARD_SCALE_PX(34));
            lv_image_set_inner_align(delete_icon, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_align(delete_icon, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(20), 0);
            lv_obj_set_ext_click_area(delete_icon, BOARD_SCALE_PX(6));
#else
            lv_obj_align(delete_icon, LV_ALIGN_RIGHT_MID, -20, 0);
#endif
            lv_obj_add_flag(delete_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(delete_icon, eq_profile_delete_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        } else if (eq_profiles_replace_mode) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_replace_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        }
    }
    if (!eq_profiles_edit_mode && !eq_profiles_replace_mode &&
        eq_profiles_download_available())
        add_pill_chevron_row(eq_profiles_list, TR("Download profiles"), eq_profiles_download_cb);
}

/* Catches every way this screen can be left (back button, back-swipe, Home
 * swipe) -- not just eq_profile_replace_row_cb's own success-path nav_pop()
 * -- so replace mode can never stay armed into a later, unrelated visit to
 * this same screen via "Load Profile". */
static void eq_profiles_screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    eq_profiles_replace_mode = false;
}

static void eq_profiles_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    if (!eq_profiles_edit_mode && !eq_profiles_replace_mode) populate_eq_profiles_screen();
}

static void eq_profiles_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_profiles_edit_mode = !eq_profiles_edit_mode;
    populate_eq_profiles_screen();
}

static void build_eq_profile_delete_popup(void) {
    eq_profile_delete_popup.popup = build_confirm_popup(TR("Delete this profile?"), LV_LABEL_LONG_WRAP, NULL, NULL, TR("Delete"),
                                                         lv_color_hex(GUI_BREEZE_NEGATIVE), eq_profile_delete_confirm_cb, NULL,
                                                         TR("Cancel"), accent_lv_color(), eq_profile_delete_cancel_cb, NULL,
                                                         eq_profile_delete_popup_backdrop_cb, &eq_profile_delete_popup.backdrop);
}

static lv_obj_t * build_eq_profiles_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen(TR("Profiles"), &eq_profiles_title_label, &eq_profiles_list);
    lv_obj_add_event_cb(scr, eq_profiles_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr, eq_profiles_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    eq_profiles_edit_btn = lv_label_create(scr);
    lv_label_set_text(eq_profiles_edit_btn, TR("Edit"));
    lv_obj_add_style(eq_profiles_edit_btn, gui_theme_accent_style(), 0);
    lv_obj_set_ext_click_area(eq_profiles_edit_btn, BOARD_SCALE_PX(16));
    lv_obj_set_style_text_font(eq_profiles_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(eq_profiles_edit_btn, 20);
    lv_obj_add_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eq_profiles_edit_btn, eq_profiles_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    if (eq_profiles_title_label) reserve_title_width_before(eq_profiles_title_label, eq_profiles_edit_btn);
    return scr;
}

static void eq_load_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_profiles_edit_mode = false;
    eq_profiles_replace_mode = false;
    populate_eq_profiles_screen();
    nav_push(eq_profiles_screen);
}

/* Entered only from the Save flow's "Replace Existing" choice -- see
 * eq_open_save_choice_popup(). */
static void eq_open_profiles_for_replace(void) {
    eq_profiles_edit_mode = false;
    eq_profiles_replace_mode = true;
    populate_eq_profiles_screen();
    nav_push(eq_profiles_screen);
}

static void eq_save_profile_name_done_cb(const char * text, void * user_data) {
    (void) user_data;
    eq_save_named_profile(text);
}

/* Save-choice popup: offered whenever a Save action would otherwise go
 * straight to a blank/prefilled text entry (no current profile, or a
 * deliberate "Save As" hold) AND at least one profile already exists to
 * replace -- reported gap: users had no way to pick an EXISTING preset to
 * overwrite, only a text box to name a new one. `eq_save_choice_prefill_current`
 * remembers which of those two triggered it, so "New Profile" opens the
 * right text-entry variant once the choice is made. */
static gui_popup_t eq_save_choice_popup;
static bool eq_save_choice_prefill_current = false;

static void eq_hide_save_choice_popup(void) {
    gui_popup_hide(&eq_save_choice_popup);
}

static void eq_save_choice_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
}

static void eq_save_choice_new_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
    const char * prefill = eq_save_choice_prefill_current ? eq_current_profile_name : "";
    const char * title = eq_save_choice_prefill_current ? TR("Save Profile As") : TR("Profile Name");
    show_text_entry(title, prefill, false, false, eq_save_profile_name_done_cb, NULL);
}

static void eq_save_choice_replace_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
    eq_open_profiles_for_replace();
}

static void build_eq_save_choice_popup(void) {
    eq_save_choice_popup.popup = build_confirm_popup(
        TR("Save Profile"), LV_LABEL_LONG_WRAP, NULL,
        TR("Save as a new profile, or replace one that already exists?"),
        TR("Replace Existing"), lv_color_hex(GUI_BREEZE_NEGATIVE), eq_save_choice_replace_cb, NULL,
        TR("New Profile"), accent_lv_color(), eq_save_choice_new_cb, NULL,
        eq_save_choice_backdrop_cb, &eq_save_choice_popup.backdrop);
}

/* Shows the choice popup when there's at least one existing profile to
 * offer replacing; otherwise (first-ever save) goes straight to the same
 * text-entry a plain "New Profile" choice would have opened, since there's
 * nothing yet to replace. */
static void eq_open_save_choice_popup(bool prefill_current_name) {
    char ** existing_paths = NULL;
    int existing_count = 0;
    bool have_existing = peq_scan_profiles(PEQ_PROFILES_DIR, &existing_paths, &existing_count) && existing_count > 0;
    for (int i = 0; i < existing_count; i++) free(existing_paths[i]);
    free(existing_paths);

    if (!have_existing) {
        const char * prefill = prefill_current_name ? eq_current_profile_name : "";
        const char * title = prefill_current_name ? TR("Save Profile As") : TR("Profile Name");
        show_text_entry(title, prefill, false, false, eq_save_profile_name_done_cb, NULL);
        return;
    }

    eq_save_choice_prefill_current = prefill_current_name;
    gui_popup_show(&eq_save_choice_popup);
}

/* Save Profile: a quick tap saves in place; a deliberate hold opens the save
 * choice above (prefilled with the current name if choosing "New Profile").
 * Tracked locally via our own PRESSED/CLICKED timestamps rather than LVGL's
 * global LV_EVENT_LONG_PRESSED (LV_INDEV_DEF_LONG_PRESS_TIME, 400ms, shared
 * by every long-press in the app) -- 400ms was too easy to trip on an
 * ordinary deliberate tap here, especially right after a run of slider
 * drags, so this button alone uses a longer local hold. */
#define EQ_SAVE_PROFILE_HOLD_MS 700
static uint32_t eq_save_profile_press_start_ms = 0;

static void eq_save_profile_pressed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    eq_save_profile_press_start_ms = lv_tick_get();
}

static void eq_save_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool held = lv_tick_elaps(eq_save_profile_press_start_ms) >= EQ_SAVE_PROFILE_HOLD_MS;
    if (held) {
        eq_open_save_choice_popup(true);
        return;
    }
    if (eq_current_profile_name[0]) {
        eq_save_named_profile(eq_current_profile_name);
        return;
    }
    eq_open_save_choice_popup(false);
}

static lv_obj_t * eq_make_slider_card(lv_obj_t * parent, eq_field_t field, const char * title,
                                       lv_obj_t ** value_out, lv_obj_t ** slider_out,
                                       int32_t minimum, int32_t maximum) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, ui_list_row_width());
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    /* Same Breeze frame as the menu rows. */
    lv_obj_add_style(card, &pill_row_bg_style, 0);
    lv_obj_set_style_pad_hor(card, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_pad_ver(card, BOARD_SCALE_PX(3), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(1), 0);
    lv_obj_t * top_row = lv_obj_create(card);
    lv_obj_set_width(top_row, lv_pct(96));
    lv_obj_set_height(top_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_row, 0, 0);
    lv_obj_set_style_pad_all(top_row, 0, 0);
    lv_obj_set_style_pad_column(top_row, BOARD_SCALE_PX(8), 0);
    lv_obj_remove_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * heading = lv_label_create(top_row);
    lv_label_set_text(heading, title);
    lv_obj_set_width(heading, 0);
    lv_obj_set_flex_grow(heading, 1);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(heading, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(heading, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_t * value = lv_label_create(top_row);
    lv_obj_add_style(value, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    /* Leave room between the value's tap target and the slider's expanded
     * touch target so numeric entry remains independently tappable. */
    lv_obj_set_style_margin_bottom(top_row, BOARD_SCALE_PX(16), 0);
    lv_obj_add_flag(top_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(top_row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_add_event_cb(top_row, eq_field_label_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)field);
    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_width(slider, lv_pct(96));
    lv_obj_set_height(slider, BOARD_SCALE_PX(6));
    lv_slider_set_range(slider, minimum, maximum);
    /* Breeze groove and handle: the switches' neutral track and knob, with
     * the accent only on the filled part. */
    lv_obj_add_style(slider, &style_theme_switch_track, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, &style_theme_switch_knob, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, BOARD_SCALE_PX(9), LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    /* Keep the thin rail while accepting touches above and below it.
     * Disable knob-only hit testing so any point along the rail can drag. */
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(slider, BOARD_SCALE_PX(14));
    lv_obj_set_style_margin_bottom(slider, BOARD_SCALE_PX(10), 0);
    *value_out = value;
    *slider_out = slider;
    lv_obj_set_flex_grow(card, 1);
    return card;
}

static void eq_flat_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    peq_reset_equalizer();
    eq_current_profile_name[0] = '\0';
    refresh_all_eq_widgets();
    peq_save();
}

static void eq_crossfeed_angle_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    static const int angles[] = { 0, 30, 45, 60, 90 };
    uint32_t selected = lv_dropdown_get_selected(lv_event_get_target(e));
    if (selected >= sizeof(angles) / sizeof(angles[0])) selected = 0;
    peq_set_crossfeed_angle(angles[selected]);
    peq_save();
}

static lv_obj_t * build_eq_band_options_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    build_screen_header(scr, TR("Band options"), generic_back_cb, NULL, NULL);
    lv_obj_t * content = lv_obj_create(scr);
    int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t content_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100), screen_h - content_top);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, content_top);
    lv_obj_add_style(content, &style_theme_screen_bg, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, BOARD_SCALE_PY(16), 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    /* Keep the last action clear of the bottom swipe gesture area. */
    lv_obj_set_style_pad_bottom(content, BOARD_SCALE_PY(64), 0);
    lv_obj_t * title = lv_label_create(content);
    lv_label_set_text(title, TR("Filter type"));
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    eq_type_dropdown = lv_dropdown_create(content);
    lv_dropdown_set_options(eq_type_dropdown, TR("Peaking\nLow Shelf\nHigh Shelf"));
    lv_obj_set_width(eq_type_dropdown, lv_pct(100));
    gui_style_settings_dropdown(eq_type_dropdown);
    lv_obj_set_height(eq_type_dropdown, GUI_SETTINGS_ROW_HEIGHT);
    lv_obj_t * enable_row = lv_obj_create(content);
    lv_obj_set_width(enable_row, lv_pct(100));
    lv_obj_set_height(enable_row, GUI_SETTINGS_ROW_HEIGHT);
    lv_obj_add_style(enable_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_set_style_radius(enable_row, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_all(enable_row, BOARD_SCALE_PX(16), 0);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t * enable = lv_label_create(enable_row);
    lv_label_set_text(enable, TR("Enable band"));
    lv_obj_add_style(enable, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    eq_band_enabled_switch = lv_switch_create(enable_row);
    lv_obj_add_style(eq_band_enabled_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    refresh_all_eq_widgets();
    lv_obj_add_event_cb(eq_band_enabled_switch, eq_band_enabled_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_type_dropdown, eq_type_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_crossfeed_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    build_screen_header(scr, TR("Crossfeed"), generic_back_cb, NULL, NULL);
    lv_obj_t * content = lv_obj_create(scr);
    int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t content_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100), screen_h - content_top);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, content_top);
    lv_obj_add_style(content, &style_theme_screen_bg, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, BOARD_SCALE_PY(16), 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(content, BOARD_SCALE_PY(64), 0);
    lv_obj_t * crossfeed = create_eq_slider_card(content, EQ_FIELD_CROSSFEED, &eq_crossfeed_value_label, &eq_crossfeed_slider, 0, 100);
    lv_obj_set_width(crossfeed, lv_pct(100));
    lv_obj_t * angle_title = lv_label_create(content);
    lv_label_set_text(angle_title, TR("Crossfeed angle"));
    lv_obj_add_style(angle_title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(angle_title, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    eq_crossfeed_angle_dropdown = lv_dropdown_create(content);
    lv_dropdown_set_options(eq_crossfeed_angle_dropdown, TR("No delay\n30°\n45°\n60°\n90°"));
    lv_obj_set_width(eq_crossfeed_angle_dropdown, lv_pct(100));
    gui_style_settings_dropdown(eq_crossfeed_angle_dropdown);
    lv_obj_set_height(eq_crossfeed_angle_dropdown, GUI_SETTINGS_ROW_HEIGHT);
    lv_obj_t * help = lv_label_create(content);
    lv_label_set_text(help, TR("Crossfeed blends a filtered signal from the opposite channel and works independently of PEQ. With No delay, identical left and right signals are affected very little."));
    lv_obj_set_width(help, lv_pct(100));
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(help, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(help, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_event_cb(eq_crossfeed_slider, eq_crossfeed_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_crossfeed_angle_dropdown, eq_crossfeed_angle_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(scr, crossfeed_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    lv_obj_remove_flag(crossfeed, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(crossfeed);
    return scr;
}

/* Use the same logarithmic mapping as the response samples. Drawing before
 * the chart keeps the neutral reference visible without hiding a flat EQ. */
static void eq_graph_grid_draw_cb(lv_event_t * e) {
    lv_obj_t * chart = lv_event_get_target(e);
    lv_area_t area;
    lv_obj_get_content_coords(chart, &area);
    int32_t width = lv_area_get_width(&area);
    int32_t height = lv_area_get_height(&area);
    if (width <= 0 || height <= 0) return;
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_obj_get_style_line_color(chart, LV_PART_MAIN);
    line.width = 1;
    static const double ticks[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    line.opa = lv_obj_get_style_line_opa(chart, LV_PART_MAIN);
    for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); ++i) {
        line.p1.x = line.p2.x = area.x1 + lv_map(eq_graph_frequency_x(ticks[i]), 0, EQ_GRAPH_X_MAX, 0, width);
        if (line.p1.x > area.x2) line.p1.x = line.p2.x = area.x2;
        line.p1.y = area.y1;
        line.p2.y = area.y1 + height;
        lv_draw_line(layer, &line);
    }
    for (int i = 0; i < 5; ++i) {
        line.p1.x = area.x1;
        line.p2.x = area.x1 + width;
        line.p1.y = line.p2.y = area.y1 + height - height * (4 - i) / 4;
        line.opa = i == 2 ? LV_OPA_70 : lv_obj_get_style_line_opa(chart, LV_PART_MAIN);
        line.width = i == 2 ? 2 : 1;
        lv_draw_line(layer, &line);
    }
}

static lv_obj_t * build_eq_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_t * title = build_screen_header(scr, TR("PEQ"), generic_back_cb, NULL, NULL);
    eq_bypass_switch = lv_switch_create(scr);
    align_screen_header_action(eq_bypass_switch, 16);
    lv_obj_add_style(eq_bypass_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    eq_bypass_state_label = lv_label_create(scr);
    lv_label_set_text(eq_bypass_state_label, peq_get_bypass() ? TR("OFF") : TR("ON"));
    lv_obj_add_style(eq_bypass_state_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(eq_bypass_state_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(eq_bypass_state_label, BOARD_SCALE_PX(64));
    lv_label_set_long_mode(eq_bypass_state_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(eq_bypass_state_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(eq_bypass_state_label, eq_bypass_switch, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    eq_title_label = title;
    reserve_title_width_before(title, eq_bypass_state_label);
    lv_obj_t * content = lv_obj_create(scr);
    eq_main_content = content;
    int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t footer_h = BOARD_SCALE_PX(112);
    int32_t content_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100), screen_h - content_top - footer_h);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, content_top);
    lv_obj_set_style_bg_opa(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, GUI_ROW_GAP, 0);
    lv_obj_t * bandnav = lv_obj_create(content);
    lv_obj_set_size(bandnav, ui_list_row_width(), BOARD_SCALE_PX(50));
    lv_obj_set_style_bg_opa(bandnav, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(bandnav, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_border_width(bandnav, 0, 0);
    lv_obj_set_style_pad_hor(bandnav, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_pad_ver(bandnav, BOARD_SCALE_PX(4), 0);
    lv_obj_set_flex_flow(bandnav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bandnav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bandnav, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * prev = lv_label_create(bandnav);
    lv_label_set_text(prev, LV_SYMBOL_LEFT);
    lv_obj_add_style(prev, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(prev, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_add_flag(prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(prev, 14);
    lv_obj_add_event_cb(prev, eq_band_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    eq_band_number_label = lv_label_create(bandnav);
    lv_obj_add_style(eq_band_number_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(eq_band_number_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_add_flag(eq_band_number_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eq_band_number_label, eq_band_options_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * next = lv_label_create(bandnav);
    lv_label_set_text(next, LV_SYMBOL_RIGHT);
    lv_obj_add_style(next, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(next, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(next, 14);
    lv_obj_add_event_cb(next, eq_band_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    eq_band_options_row = lv_label_create(content);
    lv_obj_t * options = eq_band_options_row;
    lv_label_set_text_fmt(options, "%s  " LV_SYMBOL_RIGHT, TR("Band options"));
    lv_obj_add_style(options, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(options, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_ver(options, BOARD_SCALE_PX(2), 0);
    lv_obj_add_flag(options, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(options, eq_band_options_open_cb, LV_EVENT_CLICKED, NULL);
    eq_freq_card = eq_make_slider_card(content, EQ_FIELD_FREQ, TR("Frequency"), &eq_freq_value_label, &eq_freq_slider, 0, EQ_FREQ_SLIDER_MAX);
    eq_gain_card = eq_make_slider_card(content, EQ_FIELD_GAIN, TR("Gain"), &eq_gain_value_label, &eq_gain_slider, -120, 120);
    eq_q_card = eq_make_slider_card(content, EQ_FIELD_Q, "Q", &eq_q_value_label, &eq_q_slider, 1, 100);
    lv_obj_t * below_q_spacer = lv_obj_create(content);
    lv_obj_set_size(below_q_spacer, 1, BOARD_SCALE_PX(8));
    lv_obj_set_style_bg_opa(below_q_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(below_q_spacer, 0, 0);
    lv_obj_remove_flag(below_q_spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    eq_preamp_card = eq_make_slider_card(content, EQ_FIELD_PREAMP, TR("Pre-Amp"), &eq_preamp_value_label, &eq_preamp_slider, -120, 120);
    eq_graph_panel = lv_obj_create(content);
    lv_obj_set_width(eq_graph_panel, ui_list_row_width());
    lv_obj_set_flex_grow(eq_graph_panel, 1);
    lv_obj_set_style_min_height(eq_graph_panel, BOARD_SCALE_PX(240), 0);
    lv_obj_add_style(eq_graph_panel, &pill_row_bg_style, 0);
    lv_obj_set_style_pad_all(eq_graph_panel, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(eq_graph_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(eq_graph_panel, BOARD_SCALE_PX(4), 0);
    lv_obj_remove_flag(eq_graph_panel, LV_OBJ_FLAG_SCROLLABLE);
    eq_graph_caption = lv_label_create(eq_graph_panel);
    lv_label_set_text(eq_graph_caption, TR("EQ correction (dB)"));
    lv_obj_set_width(eq_graph_caption, lv_pct(100));
    lv_label_set_long_mode(eq_graph_caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(eq_graph_caption, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_style(eq_graph_caption, &style_theme_text_primary, 0);
    eq_graph_reference = lv_label_create(eq_graph_panel);
    lv_label_set_text(eq_graph_reference, TR("48 kHz · Preamp excluded (+0.0 dB)"));
    lv_obj_set_width(eq_graph_reference, lv_pct(100));
    lv_label_set_long_mode(eq_graph_reference, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(eq_graph_reference, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_style(eq_graph_reference, &style_theme_text_muted, 0);
    lv_obj_t * graph_plot_row = lv_obj_create(eq_graph_panel);
    lv_obj_set_width(graph_plot_row, lv_pct(100));
    lv_obj_set_flex_grow(graph_plot_row, 1);
    lv_obj_set_style_bg_opa(graph_plot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(graph_plot_row, 0, 0);
    lv_obj_set_style_pad_all(graph_plot_row, 0, 0);
    lv_obj_set_style_pad_column(graph_plot_row, BOARD_SCALE_PX(4), 0);
    lv_obj_set_flex_flow(graph_plot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(graph_plot_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(graph_plot_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * graph_y_axis = lv_obj_create(graph_plot_row);
    lv_obj_set_width(graph_y_axis, BOARD_SCALE_PX(60));
    lv_obj_set_height(graph_y_axis, lv_pct(100));
    lv_obj_set_style_bg_opa(graph_y_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(graph_y_axis, 0, 0);
    lv_obj_set_style_pad_all(graph_y_axis, 0, 0);
    lv_obj_set_flex_flow(graph_y_axis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(graph_y_axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    eq_graph_max_label = lv_label_create(graph_y_axis);
    eq_graph_high_label = lv_label_create(graph_y_axis);
    eq_graph_zero_label = lv_label_create(graph_y_axis);
    eq_graph_low_label = lv_label_create(graph_y_axis);
    eq_graph_min_label = lv_label_create(graph_y_axis);
    lv_obj_set_style_text_font(eq_graph_max_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_font(eq_graph_high_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_font(eq_graph_zero_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_font(eq_graph_low_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_font(eq_graph_min_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_style(eq_graph_max_label, &style_theme_text_muted, 0);
    lv_obj_add_style(eq_graph_high_label, &style_theme_text_muted, 0);
    lv_obj_add_style(eq_graph_zero_label, &style_theme_text_primary, 0);
    lv_obj_add_style(eq_graph_low_label, &style_theme_text_muted, 0);
    lv_obj_add_style(eq_graph_min_label, &style_theme_text_muted, 0);
    eq_graph_chart = lv_chart_create(graph_plot_row);
    lv_obj_set_height(eq_graph_chart, lv_pct(100));
    lv_obj_set_style_pad_all(eq_graph_chart, 0, 0);
    lv_obj_set_style_pad_ver(eq_graph_chart,
        lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) / 2, 0);
    lv_obj_set_flex_grow(eq_graph_chart, 1);
    lv_chart_set_type(eq_graph_chart, LV_CHART_TYPE_SCATTER);
    lv_chart_set_point_count(eq_graph_chart, EQ_GRAPH_BASE_POINTS);
    /* Draw the frequency grid in logarithmic coordinates behind the curve. */
    lv_chart_set_div_line_count(eq_graph_chart, 0, 0);
    lv_chart_set_range(eq_graph_chart, LV_CHART_AXIS_PRIMARY_Y, -1200, 1200);
    lv_obj_set_style_bg_opa(eq_graph_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eq_graph_chart, 0, 0);
    lv_obj_set_style_line_color(eq_graph_chart, lv_color_make(128, 128, 128), LV_PART_MAIN);
    lv_obj_set_style_line_opa(eq_graph_chart, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_line_color(eq_graph_chart, accent_lv_color(), LV_PART_ITEMS);
    lv_obj_set_style_line_width(eq_graph_chart, BOARD_SCALE_PX(3), LV_PART_ITEMS);
    lv_obj_set_style_size(eq_graph_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_add_event_cb(eq_graph_chart, eq_graph_grid_draw_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    eq_graph_series = lv_chart_add_series(eq_graph_chart, accent_lv_color(), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_t * graph_x_axis = lv_obj_create(eq_graph_panel);
    lv_obj_set_width(graph_x_axis, lv_pct(100));
    lv_obj_set_height(graph_x_axis, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)));
    lv_obj_set_style_bg_opa(graph_x_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(graph_x_axis, 0, 0);
    lv_obj_set_style_pad_all(graph_x_axis, 0, 0);
    lv_obj_set_style_pad_left(graph_x_axis, BOARD_SCALE_PX(60) + BOARD_SCALE_PX(4), 0);
    lv_obj_remove_flag(graph_x_axis, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(graph_x_axis, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(graph_x_axis, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    /* Tick positions follow the same logarithmic mapping as the samples. */
    lv_obj_t * xlab = lv_label_create(graph_x_axis);
    lv_label_set_text(xlab, "20");
    lv_obj_align(xlab, LV_ALIGN_TOP_LEFT, 0, 0);
    xlab = lv_label_create(graph_x_axis);
    lv_label_set_text(xlab, "100");
    lv_obj_align(xlab, LV_ALIGN_TOP_MID, lv_pct(-27), 0);
    xlab = lv_label_create(graph_x_axis);
    lv_label_set_text(xlab, "1k");
    lv_obj_align(xlab, LV_ALIGN_TOP_MID, lv_pct(7), 0);
    xlab = lv_label_create(graph_x_axis);
    lv_label_set_text(xlab, "20k Hz");
    lv_obj_align(xlab, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(eq_graph_panel, LV_OBJ_FLAG_HIDDEN);
    eq_footer = lv_obj_create(scr);
    lv_obj_set_size(eq_footer, lv_pct(100), footer_h);
    lv_obj_align(eq_footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(eq_footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eq_footer, 0, 0);
    lv_obj_set_style_pad_hor(eq_footer, GUI_LIST_SIDE_MARGIN, 0);
    lv_obj_set_style_pad_ver(eq_footer, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_column(eq_footer, GUI_ROW_GAP, 0);
    lv_obj_set_flex_flow(eq_footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(eq_footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * flat = lv_btn_create(eq_footer);
    lv_obj_set_flex_grow(flat, 1);
    lv_obj_set_height(flat, BOARD_SCALE_PX(66));
    lv_obj_add_style(flat, &pill_row_bg_style, 0);
    lv_obj_add_style(flat, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(flat, 0, 0);
    lv_obj_add_event_cb(flat, eq_flat_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * fl = lv_label_create(flat);
    lv_label_set_text(fl, TR("Flat"));
    lv_obj_add_style(fl, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(fl, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(fl);
    lv_obj_t * profiles = lv_btn_create(eq_footer);
    lv_obj_set_flex_grow(profiles, 2);
    lv_obj_set_height(profiles, BOARD_SCALE_PX(66));
    /* A Breeze combo box: a plain framed button, not an accent one. */
    lv_obj_add_style(profiles, &pill_row_bg_style, 0);
    lv_obj_add_style(profiles, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(profiles, 0, 0);
    lv_obj_add_event_cb(profiles, eq_load_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * profile_inner = lv_obj_create(profiles);
    lv_obj_set_size(profile_inner, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(profile_inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(profile_inner, 0, 0);
    lv_obj_set_style_pad_all(profile_inner, 0, 0);
    lv_obj_set_flex_flow(profile_inner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(profile_inner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(profile_inner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(profile_inner, LV_OBJ_FLAG_SCROLLABLE);
    eq_profile_button_label = lv_label_create(profile_inner);
    lv_obj_set_style_text_font(eq_profile_button_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_style(eq_profile_button_label, &style_theme_text_primary, 0);
    lv_label_set_long_mode(eq_profile_button_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(eq_profile_button_label, lv_pct(82));
    lv_obj_set_height(eq_profile_button_label, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)));
    lv_obj_set_style_text_align(eq_profile_button_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t * profile_chevron = lv_label_create(profile_inner);
    lv_label_set_text(profile_chevron, LV_SYMBOL_DOWN);
    lv_obj_add_style(profile_chevron, &style_theme_text_primary, 0);
    lv_obj_t * save = lv_btn_create(eq_footer);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_set_height(save, BOARD_SCALE_PX(66));
    lv_obj_add_style(save, &pill_row_bg_style, 0);
    lv_obj_add_style(save, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, eq_save_profile_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(save, eq_save_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save);
    lv_label_set_text(sl, TR("Save"));
    lv_obj_add_style(sl, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(sl, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(sl);
    refresh_all_eq_widgets();
    lv_obj_add_event_cb(scr, eq_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(eq_bypass_switch, eq_bypass_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_preamp_slider, eq_preamp_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_freq_slider, eq_freq_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_gain_slider, eq_gain_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_q_slider, eq_q_slider_event_cb, LV_EVENT_ALL, NULL);
    finalize_screen_navigation(scr);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(eq_preamp_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(eq_freq_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(eq_gain_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(eq_q_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_preamp_card);
    register_swipe_dead_zone(eq_freq_card);
    register_swipe_dead_zone(eq_gain_card);
    register_swipe_dead_zone(eq_q_card);
    return scr;
}

void gui_equalizer_build_screens(lv_event_cb_t profiles_download_cb,
                                 bool (*profiles_download_available)(void)) {
    eq_profiles_download_cb = profiles_download_cb;
    eq_profiles_download_available = profiles_download_available;
    eq_screen = build_eq_screen();
    eq_band_options_screen = build_eq_band_options_screen();
    settings_crossfeed_screen = build_crossfeed_screen();
    eq_profiles_screen = build_eq_profiles_screen();
}

void gui_equalizer_build_popups(void) {
    build_eq_profile_delete_popup();
    build_eq_save_choice_popup();
}

void gui_equalizer_teardown_popups(void) {
    gui_popup_teardown(&eq_profile_delete_popup);
    gui_popup_teardown(&eq_save_choice_popup);
}

void gui_equalizer_teardown_screens(void) {
    if (eq_screen) { lv_obj_delete(eq_screen); eq_screen = NULL; }
    if (eq_band_options_screen) { lv_obj_delete(eq_band_options_screen); eq_band_options_screen = NULL; }
    if (settings_crossfeed_screen) { lv_obj_delete(settings_crossfeed_screen); settings_crossfeed_screen = NULL; }
    eq_bypass_switch = eq_band_enabled_switch = NULL;
    eq_preamp_slider = eq_preamp_value_label = NULL;
    eq_crossfeed_slider = eq_crossfeed_value_label = NULL;
    eq_crossfeed_angle_dropdown = NULL;
    eq_freq_slider = eq_gain_slider = eq_q_slider = NULL;
    eq_type_dropdown = eq_freq_value_label = eq_gain_value_label = eq_q_value_label = NULL;
    eq_band_number_label = eq_profile_button_label = eq_main_content = eq_footer = eq_bypass_state_label = NULL;
    eq_graph_panel = eq_graph_chart = eq_graph_caption = eq_graph_reference = NULL;
    eq_graph_series = NULL;
    eq_graph_min_label = eq_graph_low_label = eq_graph_zero_label = NULL;
    eq_graph_high_label = eq_graph_max_label = NULL;
    eq_band_options_row = eq_preamp_card = eq_freq_card = eq_gain_card = eq_q_card = NULL;
    eq_graph_visible = false;
    eq_title_label = NULL;
    if (eq_profiles_screen) { lv_obj_delete(eq_profiles_screen); eq_profiles_screen = NULL; }
    eq_profiles_edit_btn = NULL;
    eq_profiles_title_label = NULL;
    eq_profiles_free_paths();
}

void gui_equalizer_update_summary(lv_obj_t * summary_label) {
    char name[256];
    eq_profile_display_name(eq_current_profile_name, name, sizeof(name));
    lv_label_set_text_fmt(summary_label, "%s · %s", peq_get_bypass() ? TR("Off") : TR("On"), name);
}

void gui_equalizer_accent_changed(void) {
    if (eq_graph_series) lv_chart_set_series_color(eq_graph_chart, eq_graph_series, accent_lv_color());
    if (eq_graph_chart) lv_obj_set_style_line_color(eq_graph_chart, accent_lv_color(), LV_PART_ITEMS);
}

lv_obj_t * gui_settings_get_eq_screen(void) { return eq_screen; }
