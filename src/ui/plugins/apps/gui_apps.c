#include "gui_apps.h"
#include "../../../core/system/runtime/board_config.h"

#include "../../../audio/engine/audio.h"
#include "../../../core/device/app_approval.h"
#include "../../../core/device/app_registry.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/process/app_session.h"
#include "../../../hardware/usb/usb_mode_control.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../network/store/catalog/emulator_store.h"
#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../network/gui_network.h"
#include "../../player/screen/gui_player.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern player_settings_t current_settings;

#define APP_LABEL_MAX 112
#define APP_ICON_PATH_MAX 320
#define APP_FALLBACK_ICON "submenu/plugins.png"
#define APPS_ROW_ICON "settings/system.png"

static lv_obj_t * apps_screen;
static lv_obj_t * apps_empty_message;
/* The pill rows copy their label text; the list data lives as long as the
 * screen shows it. */
static app_manifest_t * apps;
static int app_count;
static pill_list_item_t * app_items;
static char (*app_labels)[APP_LABEL_MAX];
static char (*app_icons)[APP_ICON_PATH_MAX];
static lv_obj_t ** app_rows;

/* The one launch waiting for the user's approval. */
static gui_popup_t approval_popup;
static char approval_app_id[sizeof(apps[0].id)];
static char approval_file[PATH_MAX];
static char approval_sha[65];

typedef struct {
    char app_id[sizeof(apps[0].id)];
    char file[PATH_MAX];
    bool has_file;
} launch_request_t;

#ifndef HOST_BUILD
static bool dac_mode_active(void) {
    return current_settings.wifi_dac_mode_enabled || current_settings.bt_dac_mode_enabled ||
           current_settings.usb_mode == USB_MODE_DAC;
}
#endif

static bool approval_show(const app_manifest_t * app, const char * file, const char * sha);

/* Runs the app with the emulators' guards: SD card, USB storage, DAC mode,
 * audio idle. The app is loaded, hashed and checked against the approval
 * right before the spawn, after the audio wait. */
static void launch_now(const char * app_id, const char * file) {
#ifdef HOST_BUILD
    (void) app_id;
    (void) file;
    show_info_toast(TR("Apps run on the player only."));
#else
    if (emulator_store_busy()) {
        show_info_toast(TR("Emulator store is busy."));
        return;
    }
    if (!emulator_store_sd_usable() || gui_network_usb_storage_session_active()) {
        show_info_toast(TR("Check the SD card and disconnect USB storage before running apps."));
        return;
    }
    if (dac_mode_active()) {
        show_info_toast(TR("Turn off DAC mode to run apps."));
        return;
    }
    if (!usb_mode_control_storage_write_begin()) {
        show_info_toast(TR("USB storage is busy."));
        return;
    }
    /* The app owns the audio device. Pausing only schedules a fade, so wait
     * until the audio worker confirms that it released the device. */
    gui_player_stop_for_external_session();
    const struct timespec audio_wait = { .tv_sec = 0, .tv_nsec = 20000000 };
    bool audio_idle = false;
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (audio_is_idle()) {
            audio_idle = true;
            break;
        }
        (void) nanosleep(&audio_wait, NULL);
    }
    if (!audio_idle && audio_is_idle()) audio_idle = true;
    if (!audio_idle) {
        usb_mode_control_storage_write_end();
        show_info_toast(TR("Audio is still active; cannot start the app."));
        return;
    }
    /* Stopping audio ran plugin handlers and took up to two seconds, so load
     * the app and check the approval only now, right before the spawn. */
    app_manifest_t app;
    char sha[65];
    if (!app_registry_find(APP_ROOT, app_board_id(), app_id, &app) || !app_exec_sha256(APP_ROOT, &app, sha)) {
        usb_mode_control_storage_write_end();
        show_error_toast(TR("Could not start the app."));
        return;
    }
    if (!app_approval_granted(APP_APPROVAL_FILE, app.id, sha)) {
        usb_mode_control_storage_write_end();
        if (!approval_show(&app, file, sha)) show_error_toast(TR("Could not start the app."));
        return;
    }
    if (app.free_player_memory) gui_release_memory_for_external_session();
    int rc = app_session_run(&app, file);
    gui_resume_after_external_session();
    usb_mode_control_storage_write_end();
    if (rc < 0) show_error_toast(TR("Could not start the app."));
    else if (rc > 0) show_error_toast(TR("The app closed unexpectedly."));
#endif
}

static void launch_async_cb(void * data);

static void approval_close(void) {
    gui_popup_teardown(&approval_popup);
}

static void approval_run_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    launch_request_t * request = calloc(1, sizeof(*request));
    bool granted = request && app_approval_grant(APP_APPROVAL_FILE, approval_app_id, approval_sha);
    if (granted) {
        snprintf(request->app_id, sizeof(request->app_id), "%s", approval_app_id);
        request->has_file = approval_file[0] != '\0';
        snprintf(request->file, sizeof(request->file), "%s", approval_file);
    }
    approval_close();
    if (granted) {
        /* After this event: the dialog must be gone before the app owns the screen. */
        lv_async_call(launch_async_cb, request);
    } else {
        free(request);
        show_error_toast(TR("Could not save the approval."));
    }
}

static void approval_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    approval_close();
}

static void approval_object_deleted_cb(lv_event_t * e) {
    lv_obj_t * target = lv_event_get_target(e);
    if (approval_popup.popup == target) approval_popup.popup = NULL;
    if (approval_popup.backdrop == target) approval_popup.backdrop = NULL;
}

/* Asks "Run <name>?" with Run and Cancel. False when a dialog is already up. */
static bool approval_show(const app_manifest_t * app, const char * file, const char * sha) {
    if (approval_popup.popup || approval_popup.backdrop || !lv_layer_top()) return false;
    snprintf(approval_app_id, sizeof(approval_app_id), "%s", app->id);
    snprintf(approval_file, sizeof(approval_file), "%s", file ? file : "");
    snprintf(approval_sha, sizeof(approval_sha), "%s", sha);
    char title[160];
    snprintf(title, sizeof(title), TR("Run %s?"), app->name);
    approval_popup.popup = build_confirm_popup(title, LV_LABEL_LONG_WRAP, NULL,
        TR("External apps have full access to the player."), TR("Run"), accent_lv_color(), approval_run_cb, NULL,
        TR("Cancel"), accent_lv_color(), approval_cancel_cb, NULL, approval_cancel_cb, &approval_popup.backdrop);
    if (!approval_popup.popup || !approval_popup.backdrop) {
        gui_popup_teardown(&approval_popup);
        return false;
    }
    lv_obj_add_event_cb(approval_popup.popup, approval_object_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(approval_popup.backdrop, approval_object_deleted_cb, LV_EVENT_DELETE, NULL);
    gui_popup_show(&approval_popup);
    return true;
}

/* Launches an installed app, asking first when the user has not approved this
 * executable. */
static void request_launch(const char * app_id, const char * file) {
    app_manifest_t app;
    char sha[65];
    if (!app_registry_find(APP_ROOT, app_board_id(), app_id, &app) || !app_exec_sha256(APP_ROOT, &app, sha)) {
        show_error_toast(TR("Could not start the app."));
        return;
    }
    if (app_approval_granted(APP_APPROVAL_FILE, app.id, sha)) {
        launch_now(app.id, file);
    } else if (!approval_show(&app, file, sha)) {
        show_info_toast(TR("Another app is waiting for approval."));
    }
}

static void launch_async_cb(void * data) {
    launch_request_t * request = data;
    if (!request) return;
    request_launch(request->app_id, request->has_file ? request->file : NULL);
    free(request);
}

bool gui_apps_request_launch(const char * app_id, const char * file, const char ** reason) {
    *reason = "";
    app_manifest_t app;
    if (!app_registry_find(APP_ROOT, app_board_id(), app_id, &app)) {
        *reason = "no such app";
        return false;
    }
    if (file) {
        const char * problem = app_launch_file_check(MUSIC_ROOT_DIR, &app, file);
        if (problem) {
            *reason = problem;
            return false;
        }
        if (strlen(file) >= sizeof(((launch_request_t *) 0)->file)) {
            *reason = "path is too long";
            return false;
        }
    }
    if (approval_popup.popup || approval_popup.backdrop) {
        *reason = "another app is waiting for approval";
        return false;
    }
    launch_request_t * request = calloc(1, sizeof(*request));
    if (!request) {
        *reason = "out of memory";
        return false;
    }
    snprintf(request->app_id, sizeof(request->app_id), "%s", app.id);
    request->has_file = file != NULL;
    if (file) snprintf(request->file, sizeof(request->file), "%s", file);
    lv_async_call(launch_async_cb, request);
    return true;
}

static void app_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= app_count) return;
    /* Copy first: the dialog and resume paths must not depend on list data. */
    char id[sizeof(apps[0].id)];
    snprintf(id, sizeof(id), "%s", apps[index].id);
    request_launch(id, NULL);
}

static void free_list_data(void) {
    free(apps);
    free(app_items);
    free(app_labels);
    free(app_icons);
    free(app_rows);
    apps = NULL;
    app_items = NULL;
    app_labels = NULL;
    app_icons = NULL;
    app_rows = NULL;
    app_count = 0;
}

/* The icon from the app's folder, else the stock art for an app without one. */
static void decorate_app_row(lv_obj_t * row, const char * icon_path) {
    if (icon_path[0]) decorate_category_row_plugin_icon(row, icon_path);
    else decorate_category_row(row, APP_FALLBACK_ICON, NULL);
}

static void apps_screen_delete_cb(lv_event_t * e) {
    (void) e;
    apps_screen = NULL;
    apps_empty_message = NULL;
}

static void build_apps_screen(void) {
    if (apps_screen && !gui_navigation_contains(apps_screen)) lv_obj_delete(apps_screen);
    free_list_data();
    int count = 0;
    apps = app_registry_scan(APP_ROOT, app_board_id(), &count);
    app_items = calloc((size_t) count + 1, sizeof(*app_items));
    app_labels = calloc((size_t) count + 1, sizeof(*app_labels));
    app_icons = calloc((size_t) count + 1, sizeof(*app_icons));
    app_rows = calloc((size_t) count + 1, sizeof(*app_rows));
    if (!app_items || !app_labels || !app_icons || !app_rows) {
        free_list_data();
        return;
    }
    app_count = count;
    for (int i = 0; i < count; i++) {
        snprintf(app_labels[i], sizeof(app_labels[i]), "%s · %s", apps[i].name, apps[i].version);
        app_icons[i][0] = '\0';
        if (apps[i].icon[0]) {
            /* The row code reads a path that starts with '/' as it is, so give it the real path. */
            char path[PATH_MAX], real[PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/%s/%s", APP_ROOT, apps[i].id, apps[i].icon) < (int) sizeof(path) &&
                realpath(path, real) && strlen(real) < sizeof(app_icons[i])) {
                snprintf(app_icons[i], sizeof(app_icons[i]), "%s", real);
            }
        }
        app_items[i] = (pill_list_item_t){ .label = app_labels[i], .accessory = PILL_ACCESSORY_NONE,
                                           .on_click = app_row_click_cb, .user_data = (void *) (intptr_t) i,
                                           .out_row = &app_rows[i] };
    }
    apps_screen = build_pill_list_screen(TR("External Apps"), generic_back_cb, app_items, count,
                                         gui_theme_accent_style(), GUI_ROW_GAP, 100);
    for (int i = 0; i < count; i++) {
        decorate_app_row(app_rows[i], app_icons[i]);
        app_items[i].out_row = NULL;
    }
    lv_obj_add_event_cb(apps_screen, apps_screen_delete_cb, LV_EVENT_DELETE, NULL);
    if (count == 0) {
        apps_empty_message = build_list_message(apps_screen, TR("No apps installed"),
            TR("Copy an app to the .apps folder on the SD card, one folder per app with an app.json file."));
        lv_obj_align(apps_empty_message, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT);
    }
    finalize_screen_navigation(apps_screen);
}

void gui_apps_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    build_apps_screen();
    if (apps_screen) nav_push(apps_screen);
}

void gui_apps_teardown(void) {
    approval_close();
    if (apps_screen) lv_obj_delete(apps_screen);
    apps_screen = NULL;
    apps_empty_message = NULL;
    free_list_data();
}
