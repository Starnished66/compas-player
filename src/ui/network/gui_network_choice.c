#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "../settings/gui_settings.h"
#include "../widgets/builders/screen_builders.h"
#include "gui_network_internal.h"

static lv_obj_t * resume_mode_screen;
static lv_obj_t * resume_mode_list;
static lv_obj_t * font_size_screen;
static lv_obj_t * replaygain_mode_screen;

/* Single-select option table shared by the Resume / Font Size / ReplayGain
 * screens. The row callback receives the table index. */
typedef struct {
    int value;
    const char * label; /* untranslated, wrapped in N_() */
} int_option_t;
#define INT_OPTION_COUNT(table) (sizeof(table) / sizeof((table)[0]))

static void populate_int_options(lv_obj_t * list, const int_option_t * options, size_t count,
                                 int current, lv_event_cb_t cb) {
    lv_obj_clean(list);
    for (size_t i = 0; i < count; i++)
        add_pill_option_row(list, TR(options[i].label), current == options[i].value,
                            cb, (void *) (intptr_t) i);
}

/* ---- Resume Last Track selection screen (Settings > Music Settings >
 * Playback > Resume Last Track) -- same accent-colored-border single-select shape as Font Size
 * just below, but takes effect on the NEXT cold boot (there's nothing to
 * apply live -- this only ever matters at startup), so no reboot popup is
 * needed, just a toast confirming the choice. See gui_init()'s own resume
 * path for what each option actually does and the Subsonic-cache guard it
 * shares with Car Mode's separate, always-on resume mechanism. ---- */

/* Value (matches player_settings_t.resume_mode) and untranslated label. */
static const int_option_t resume_mode_options[] = {
    { 0, N_("Off") }, { 1, N_("Resume and Play") }, { 2, N_("Resume, but Paused") },
};

static void resume_mode_option_row_cb(lv_event_t * e);

static void populate_resume_mode_screen(void) {
    populate_int_options(resume_mode_list, resume_mode_options, INT_OPTION_COUNT(resume_mode_options),
                         current_settings.resume_mode, resume_mode_option_row_cb);
}

static void resume_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.resume_mode = resume_mode_options[index].value;
    settings_save_async(&current_settings);
    populate_resume_mode_screen();
    show_info_toast(TR("Applies next time you launch the app"));
}

void resume_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_resume_mode_screen();
    nav_push(resume_mode_screen);
}

/* ---- Font size selection screen (Settings > Font Size).  A changed tier
 * is applied behind one rendered black frame, then navigation is reset
 * directly to Home without disturbing playback or background services. ---- */

/* Value matches player_settings_t.font_size_tier. */
static const int_option_t font_size_options[] = {
    { 0, N_("Small") }, { 1, N_("Medium") }, { 2, N_("Large") },
};

static lv_obj_t * font_size_list;

static void font_size_option_row_cb(lv_event_t * e);

static void populate_font_size_screen(void) {
    populate_int_options(font_size_list, font_size_options, INT_OPTION_COUNT(font_size_options),
                         current_settings.font_size_tier, font_size_option_row_cb);
}

static void font_size_apply_timer_cb(lv_timer_t * timer) {
    lv_obj_t * mask = (lv_obj_t *)lv_timer_get_user_data(timer);
    int target = (int)(intptr_t)lv_obj_get_user_data(mask) - 1;
    lv_timer_delete(timer);
    if (!fallback_font_apply_size_tier(target)) {
        lv_obj_delete(mask);
        show_error_toast(TR("Could not apply font size"));
        return;
    }

    /* Stable font addresses mean existing object styles now see the new
     * descriptors.  Notify LVGL once, refresh only cached pixel geometry,
     * and discard every bitmap captured with the old metrics.  All of this
     * is one-shot work inside the mask; no timer or extra allocation remains
     * afterward. */
    gui_navigation_invalidate_font_snapshots();
    current_settings.font_size_tier = target;
    settings_save_async(&current_settings);
    lv_obj_report_style_change(NULL);
    /* Every live screen, not only the Font Size nav stack. Music Settings
     * and the other unused Settings submenus keep the previous tier's
     * USER_3 label boxes unless they are in this walk. */
    screen_builders_refresh_all_font_geometry();
    gui_settings_refresh_font_geometry();
    gui_player_refresh_font_geometry();
    compact_list_refresh_all();
    quick_drawer_mark_snapshot_dirty();
    nav_reset_to_home();
    screen_builders_refresh_font_geometry(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());

    lv_obj_delete(mask);
}

static void font_size_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    int target = font_size_options[index].value;
    if (target == current_settings.font_size_tier) return;

    lv_obj_t * mask = build_font_change_mask(target);

    /* Leave enough time for the normal refresh timer to paint the mask;
     * synchronous lv_refr_now() here would re-enter rendering from an input
     * callback and was the source of earlier transition flicker. */
    lv_timer_create(font_size_apply_timer_cb, 35, mask);
}

void font_size_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_font_size_screen();
    nav_push(font_size_screen);
}



/* ---- ReplayGain mode (Settings -> Playback) -- 3-way option (Off, Per Track,
 * Per Album). Applied immediately to take effect on the next started or queued
 * track without loudness jumps during current playback. ---- */

/* Value matches player_settings_t.replaygain_mode. */
static const int_option_t replaygain_mode_options[] = {
    { 0, N_("Off") }, { 1, N_("Per Track") }, { 2, N_("Per Album") },
};

static lv_obj_t * replaygain_mode_list;

static void replaygain_mode_option_row_cb(lv_event_t * e);

static void populate_replaygain_mode_screen(void) {
    populate_int_options(replaygain_mode_list, replaygain_mode_options,
                         INT_OPTION_COUNT(replaygain_mode_options),
                         current_settings.replaygain_mode, replaygain_mode_option_row_cb);
}

static void replaygain_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.replaygain_mode = replaygain_mode_options[index].value;
    settings_save_async(&current_settings);
    populate_replaygain_mode_screen();
    if (gui_player_has_active_track()) arm_next_track_for_audio(gui_player_get_playlist_index());
}

void replaygain_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_replaygain_mode_screen();
    nav_push(replaygain_mode_screen);
}

void gui_network_choice_settings_build(void) {
    font_size_screen = network_list_screen(TR("Font Size"), &font_size_list);
    replaygain_mode_screen = network_list_screen(TR("ReplayGain"), &replaygain_mode_list);
    resume_mode_screen = network_list_screen(TR("Resume Last Track"), &resume_mode_list);
}

void gui_network_choice_settings_teardown(void) {
    network_delete_screen(&font_size_screen);
    network_delete_screen(&replaygain_mode_screen);
    network_delete_screen(&resume_mode_screen);
    font_size_list = NULL;
    replaygain_mode_list = NULL;
    resume_mode_list = NULL;
}
