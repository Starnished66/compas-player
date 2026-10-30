#include "gui_updates.h"
#include "../shell/gui_notifications.h"
#include "../widgets/builders/screen_builders.h"
#include "../appearance/gui_theme.h"
#include "../network/gui_network.h"
#include "../shell/gui_shell.h"
#include "../../core/updates/firmware_update.h"
#include "../../network/firmware/firmware_ota.h"
#include "../../core/updates/player_update.h"
#include "../../core/system/lifecycle/idle_shutdown.h"
#include "../../core/system/runtime/app_version.h"
#include "../text/i18n/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gui_popup_t firmware_update_popup;
static lv_obj_t * firmware_update_popup_title;
static char firmware_update_selected_path[512];
static bool manual_update_ui_active;
static gui_busy_handle_t manual_update_busy;
static int update_ui_last_phase = -1;
static bool update_ui_last_delayed;

static bool firmware_usb_ready(void) {
    if (!gui_network_firmware_update_usb_blocked()) return true;
    show_error_toast(TR("Wait for USB mode switching to finish and disconnect USB storage before updating."));
    return false;
}

static void hide_firmware_update_popup(void) {
    gui_popup_hide(&firmware_update_popup);
}

static void firmware_update_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
}

static void firmware_update_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
}

static void firmware_update_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
    if (!firmware_usb_ready()) return;
    char error[160];
    if (!firmware_update_start(firmware_update_selected_path, error, sizeof(error))) {
        show_error_toast(error);
        return;
    }
    manual_update_ui_active = true;
    update_ui_last_phase = -1;
    manual_update_busy = gui_busy_show(TR("Preparing update"), TR("Checking the file on the SD card"));
}

static void build_firmware_update_popup(void) {
    firmware_update_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &firmware_update_popup_title, NULL, TR("Update & Reboot"), lv_color_hex(GUI_BREEZE_NEGATIVE),
        firmware_update_confirm_cb, NULL, TR("Cancel"), accent_lv_color(), firmware_update_cancel_cb, NULL,
        firmware_update_popup_backdrop_cb, &firmware_update_popup.backdrop);
}

static void firmware_update_from_sd(void) {
    char error[160];
    if (!firmware_update_scan_checked(firmware_update_selected_path, sizeof(firmware_update_selected_path),
                                     error, sizeof(error))) {
        show_error_toast(error);
        return;
    }

    const char * filename = strrchr(firmware_update_selected_path, '/');
    filename = filename ? filename + 1 : firmware_update_selected_path;
    lv_label_set_text_fmt(firmware_update_popup_title, TR("Update using %s?\nDevice will reboot into recovery mode."),
                           filename);

    gui_popup_show(&firmware_update_popup);
}

/* ---- Online update from the latest weekly release (firmware_ota.h). The
 * worker runs the check and download; poll_firmware_ota() drives the UI
 * from the main loop. ---- */
static lv_obj_t * firmware_source_menu;
static lv_obj_t * firmware_source_backdrop;
static gui_popup_t ota_offer_popup;
static lv_obj_t * ota_offer_title;
static gui_popup_t ota_install_popup;
static lv_obj_t * ota_install_title;
static gui_busy_handle_t ota_busy;
static bool ota_ui_active;

/* Player-only update shares the settings screen's main-loop polling and busy
 * overlay. The worker's state survives an in-process UI rebuild; teardown
 * drops only the LVGL handle, and polling recreates the overlay if needed. */
static lv_obj_t * player_update_menu;
static lv_obj_t * player_update_backdrop;
static gui_popup_t player_update_offer_popup;
static lv_obj_t * player_update_offer_title;
static gui_popup_t player_update_confirm_popup;
static lv_obj_t * player_update_confirm_title;
static gui_busy_handle_t player_update_busy;
static bool player_update_ui_active;
static player_update_state_t player_update_ui_last_state = PLAYER_UPDATE_IDLE;

static bool player_update_worker_active(void) {
    player_update_status_t status;
    player_update_get_status(&status);
    return status.state == PLAYER_UPDATE_CHECKING || status.state == PLAYER_UPDATE_DOWNLOADING ||
           status.state == PLAYER_UPDATE_INSTALLING;
}

static bool player_update_can_start(void) {
    if (!firmware_usb_ready()) return false;
    if (firmware_ota_busy() || firmware_update_busy() || player_update_worker_active()) {
        show_error_toast(TR("An update is already in progress"));
        return false;
    }
    return true;
}

static void hide_player_update_menu(void) {
    if (player_update_menu) lv_obj_add_flag(player_update_menu, LV_OBJ_FLAG_HIDDEN);
    if (player_update_backdrop) lv_obj_add_flag(player_update_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void player_update_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_player_update_menu();
}

static void player_update_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&player_update_offer_popup);
    gui_popup_hide(&player_update_confirm_popup);
    player_update_reset();
}

static void player_update_show_sd_confirm(void) {
    lv_label_set_text(player_update_confirm_title,
                      TR("Install the player update from the SD card?\n\nUpdates the player and restarts the device. Firmware, fonts, and artwork are unchanged."));
    gui_popup_show(&player_update_confirm_popup);
}

static void player_update_install_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&player_update_confirm_popup);
    if (!player_update_can_start()) return;
    char error[160] = {0};
    bool started = player_update_start_sd_install(error, sizeof(error));
    if (!started) {
        show_error_toast(error[0] ? error : TR("Could not start the player update"));
        return;
    }
    player_update_ui_active = true;
    player_update_ui_last_state = PLAYER_UPDATE_IDLE;
    player_update_busy = gui_busy_show(TR("Updating player"), TR("Starting update"));
    gui_busy_set_progress(player_update_busy, 0);
}

static void player_update_online_offer_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&player_update_offer_popup);
    if (!player_update_can_start()) return;
    if (!gui_shell_wifi_effective_enabled()) {
        show_error_toast(TR("Connect to Wi-Fi before downloading the player update"));
        return;
    }
    char error[160] = {0};
    if (!player_update_start_online_install(error, sizeof(error))) {
        show_error_toast(error[0] ? error : TR("Could not start the player update"));
        return;
    }
    player_update_ui_active = true;
    player_update_ui_last_state = PLAYER_UPDATE_IDLE;
    player_update_busy = gui_busy_show(TR("Updating player"), TR("Starting download"));
    gui_busy_set_progress(player_update_busy, 0);
}

static void player_update_online_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_player_update_menu();
    if (!player_update_can_start()) return;
    if (!gui_shell_wifi_effective_enabled()) {
        show_error_toast(TR("Turn on Wi-Fi and connect first"));
        return;
    }
    if (!player_update_start_check()) {
        show_error_toast(TR("Could not start the player update check"));
        return;
    }
    player_update_ui_active = true;
    player_update_ui_last_state = PLAYER_UPDATE_IDLE;
    player_update_busy = gui_busy_show(TR("Checking for player updates"), "");
}

static void player_update_sd_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_player_update_menu();
    if (!player_update_can_start()) return;
    player_update_show_sd_confirm();
}

/* Explains what an update source menu replaces, above its action rows. */
static void add_update_menu_description(lv_obj_t * menu, const char * text) {
    lv_obj_t * label = lv_label_create(menu);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_all(label, BOARD_SCALE_PX(6), 0);
    lv_obj_move_to_index(label, 0);
}

static void build_player_update_popups(void) {
    const menu_popup_row_t rows[] = {
        { TR("Check for online update"), player_update_online_cb, false },
        { TR("Install from SD card"), player_update_sd_cb, false },
        { TR("Cancel"), player_update_backdrop_cb, false, true },
    };
    player_update_menu = build_menu_popup(rows, (int)(sizeof(rows) / sizeof(rows[0])),
                                          player_update_backdrop_cb, &player_update_backdrop);
    add_update_menu_description(player_update_menu,
                                TR("Update the Player only, retaining current drivers, kernel and bootloader."));
    player_update_offer_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &player_update_offer_title, NULL, TR("Download & Install"), accent_lv_color(),
        player_update_online_offer_cb, NULL, TR("Cancel"), accent_lv_color(), player_update_cancel_cb, NULL,
        player_update_cancel_cb, &player_update_offer_popup.backdrop);
    player_update_confirm_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &player_update_confirm_title, NULL, TR("Install & Restart"),
        lv_color_hex(GUI_BREEZE_NEGATIVE), player_update_install_confirm_cb, NULL,
        TR("Cancel"), accent_lv_color(), player_update_cancel_cb, NULL,
        player_update_cancel_cb, &player_update_confirm_popup.backdrop);
}

void gui_updates_player_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_update_menu) lv_obj_remove_flag(player_update_menu, LV_OBJ_FLAG_HIDDEN);
    if (player_update_backdrop) lv_obj_remove_flag(player_update_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (player_update_backdrop) lv_obj_move_foreground(player_update_backdrop);
    if (player_update_menu) lv_obj_move_foreground(player_update_menu);
}

static void poll_player_update(void) {
    if (!player_update_ui_active) return;
    player_update_status_t status;
    player_update_get_status(&status);
    if (status.state == PLAYER_UPDATE_CHECKING || status.state == PLAYER_UPDATE_DOWNLOADING ||
        status.state == PLAYER_UPDATE_INSTALLING) {
        bool recreated = !player_update_busy;
        if (recreated)
            player_update_busy = gui_busy_show(status.state == PLAYER_UPDATE_CHECKING ? TR("Checking for player updates") :
                status.state == PLAYER_UPDATE_DOWNLOADING ? TR("Downloading player update") : TR("Installing player update"), "");
        if (recreated || player_update_ui_last_state != status.state) {
            const char * detail = status.state == PLAYER_UPDATE_CHECKING ? TR("Checking the latest release") :
                status.state == PLAYER_UPDATE_DOWNLOADING ? TR("Downloading player update") :
                TR("Replacing the player and preparing to restart");
            gui_busy_set_detail(player_update_busy, detail);
            player_update_ui_last_state = status.state;
        }
        if (status.state == PLAYER_UPDATE_DOWNLOADING)
            gui_busy_set_progress(player_update_busy, status.percent);
        return;
    }

    if (player_update_busy) {
        gui_busy_hide(player_update_busy);
        player_update_busy = 0;
    }
    player_update_ui_active = false;
    if (status.state == PLAYER_UPDATE_CHECKED) {
        lv_label_set_text_fmt(player_update_offer_title,
                              TR("Available player release: %s.\nInstalled: %s\n\nDownload and install? This updates the player and restarts the device. Firmware, fonts, and artwork are unchanged."),
                              status.release.label, app_version_label());
        gui_popup_show(&player_update_offer_popup);
        return;
    }
    if (status.state == PLAYER_UPDATE_DONE) {
        player_update_reset();
#ifdef HOST_BUILD
        show_info_toast(TR("Player updated successfully"));
#else
        idle_shutdown_reboot_now();
#endif
        return;
    }
    if (status.state == PLAYER_UPDATE_FAILED) {
        show_error_toast(status.error[0] ? status.error : TR("Player update failed"));
        player_update_reset();
    }
}

static void hide_firmware_source_menu(void) {
    if (firmware_source_menu) lv_obj_add_flag(firmware_source_menu, LV_OBJ_FLAG_HIDDEN);
    if (firmware_source_backdrop) lv_obj_add_flag(firmware_source_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void firmware_source_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_source_menu();
}

static void show_ota_install_popup(const char * release_label) {
    lv_label_set_text_fmt(ota_install_title,
                          TR("Firmware release %s is downloaded and verified.\n\nInstall now? The device reboots into "
                          "recovery to flash it. Do not turn it off until it restarts."),
                          release_label);
    gui_popup_show(&ota_install_popup);
}

static void firmware_source_sd_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_source_menu();
    if (!firmware_usb_ready()) return;
    /* An online check/download/install worker is running (reachable even
     * from here: the busy screen allows swiping back to the player and
     * back into Settings). Refuse outright rather than falling through to
     * the unchecked manual path -- that worker may be about to replace the
     * very image this would install. */
    if (ota_ui_active || firmware_ota_busy()) {
        show_error_toast(TR("An update is already in progress"));
        return;
    }
    /* A verified online download goes through the same re-check, battery
     * gate and parking as installing it right after the download. */
    firmware_ota_release_t pending;
    firmware_ota_pending_t state = firmware_ota_pending(&pending);
    if (state == FIRMWARE_OTA_PENDING_VALID) show_ota_install_popup(pending.label);
    else if (state == FIRMWARE_OTA_PENDING_REJECTED) {
        /* A stale record from an earlier online attempt says nothing about
         * a file the user put on the card: clear it and take the manual
         * path (the image's contents are then checked by recovery). Only
         * a marker for a failed image still at the root keeps refusing. */
        firmware_ota_discard_t discard = firmware_ota_discard_stale_pending();
        if (discard == FIRMWARE_OTA_DISCARD_REMOVED) firmware_update_from_sd();
        else if (discard == FIRMWARE_OTA_DISCARD_BUSY) show_error_toast(TR("An update is already in progress"));
        else if (discard == FIRMWARE_OTA_DISCARD_IO_ERROR)
            show_error_toast(TR("Cannot read the update record on the SD card. Check the card and try again."));
        else if (discard == FIRMWARE_OTA_DISCARD_NOT_STALE) {
            /* The record changed state since the query: dispatch afresh. */
            state = firmware_ota_pending(&pending);
            if (state == FIRMWARE_OTA_PENDING_VALID) show_ota_install_popup(pending.label);
            else if (state == FIRMWARE_OTA_PENDING_NONE) firmware_update_from_sd();
            else if (state == FIRMWARE_OTA_PENDING_UNREADABLE)
                show_error_toast(TR("Cannot read the update record on the SD card. Check the card and try again."));
            else show_error_toast(TR("The previous download is incomplete or invalid. Remove the update file from the SD card, tap Install from SD card, then copy the new file and try again."));
        }
        else show_error_toast(TR("The previous download is incomplete or invalid. Remove the update file from the SD card, tap Install from SD card, then copy the new file and try again."));
    }
    else if (state == FIRMWARE_OTA_PENDING_UNREADABLE)
        show_error_toast(TR("Cannot read the update record on the SD card. Check the card and try again."));
    else firmware_update_from_sd();
}

static void firmware_source_online_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_source_menu();
    if (!firmware_usb_ready()) return;
    if (!gui_shell_wifi_effective_enabled()) {
        show_error_toast(TR("Turn on Wi-Fi and connect first"));
        return;
    }
    if (ota_ui_active || !firmware_ota_start_check()) {
        show_error_toast(TR("An update is already in progress"));
        return;
    }
    ota_ui_active = true;
    ota_busy = gui_busy_show(TR("Checking for updates"), "");
}

static void ota_offer_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&ota_offer_popup);
    firmware_ota_reset();
}

static void ota_offer_download_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&ota_offer_popup);
    if (!firmware_usb_ready()) return;
    if (!firmware_ota_start_download()) {
        show_error_toast(TR("Could not start the download"));
        firmware_ota_reset();
        return;
    }
    ota_ui_active = true;
    ota_busy = gui_busy_show(TR("Downloading update"), TR("This may take a while"));
    gui_busy_set_progress(ota_busy, 0);
}

static void ota_install_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&ota_install_popup);
    show_info_toast(TR("Update saved. Connect to Wi-Fi, then install from Firmware Update > Install from SD card."));
    firmware_ota_reset();
}

static void ota_install_now_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&ota_install_popup);
    if (!firmware_usb_ready()) return;
    char error[160];
    if (ota_ui_active || !firmware_ota_start_install(error, sizeof(error))) {
        show_error_toast(ota_ui_active ? TR("An update is already in progress") : error);
        return;
    }
    ota_ui_active = true;
    update_ui_last_phase = -1;
    ota_busy = gui_busy_show(TR("Preparing update"), TR("Checking the file on the SD card"));
}

static void build_firmware_ota_popups(void) {
    const menu_popup_row_t rows[] = {
        { TR("Check for online update"), firmware_source_online_cb, false },
        { TR("Install from SD card"), firmware_source_sd_cb, false },
        { TR("Cancel"), firmware_source_backdrop_cb, false, true },
    };
    firmware_source_menu = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                            firmware_source_backdrop_cb, &firmware_source_backdrop);
    add_update_menu_description(firmware_source_menu,
                                TR("Update the entire CompásOS. Flashing firmware contains Bootloader, Player, Kernel and Drivers."));
    ota_offer_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &ota_offer_title, NULL, TR("Download"), accent_lv_color(), ota_offer_download_cb, NULL,
        TR("Cancel"), accent_lv_color(), ota_offer_cancel_cb, NULL, ota_offer_cancel_cb, &ota_offer_popup.backdrop);
    ota_install_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &ota_install_title, NULL, TR("Install & Reboot"), lv_color_hex(GUI_BREEZE_NEGATIVE),
        ota_install_now_cb, NULL, TR("Later"), accent_lv_color(), ota_install_later_cb, NULL, ota_install_later_cb,
        &ota_install_popup.backdrop);
}

void firmware_update_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_remove_flag(firmware_source_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(firmware_source_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(firmware_source_backdrop);
    lv_obj_move_foreground(firmware_source_menu);
}

void poll_firmware_ota(void) {
    /* Keep player update progress advancing even when firmware OTA owns its
     * worker, or when the existing firmware poll is otherwise idle. */
    poll_player_update();
    if (manual_update_ui_active || ota_ui_active) {
        firmware_update_status_t recovery;
        firmware_update_get_status(&recovery);
        if (recovery.busy && ((int) recovery.phase != update_ui_last_phase ||
                              recovery.delayed != update_ui_last_delayed)) {
            const char * detail = TR("Checking the file on the SD card");
            if (recovery.delayed) detail = recovery.error;
            else if (recovery.phase == FIRMWARE_UPDATE_BOOTFLAG) detail = TR("Preparing and verifying recovery mode");
            else if (recovery.phase == FIRMWARE_UPDATE_SYNC) detail = TR("Saving data before reboot");
            else if (recovery.phase == FIRMWARE_UPDATE_REBOOT) detail = TR("Restarting into recovery. Do not turn off the player.");
            gui_busy_set_detail(manual_update_ui_active ? manual_update_busy : ota_busy, detail);
            update_ui_last_phase = recovery.phase;
            update_ui_last_delayed = recovery.delayed;
        }
        if (manual_update_ui_active) {
            if (recovery.busy) return;
            gui_busy_hide(manual_update_busy);
            manual_update_ui_active = false;
            if (recovery.phase == FIRMWARE_UPDATE_FAILED) show_error_toast(recovery.error);
            return;
        }
    }
    if (!ota_ui_active) return;
    firmware_ota_status_t status;
    firmware_ota_get_status(&status);
    switch (status.state) {
        case FIRMWARE_OTA_CHECKING:
        case FIRMWARE_OTA_INSTALLING: /* success reboots from the worker */
            return;
        case FIRMWARE_OTA_DOWNLOADING:
            gui_busy_set_progress(ota_busy, status.percent);
            return;
        case FIRMWARE_OTA_CHECKED:
            gui_busy_hide(ota_busy);
            ota_ui_active = false;
            if (status.newer)
                lv_label_set_text_fmt(ota_offer_title,
                                      TR("Firmware release %s is available.\nInstalled: %s\n\nDownload it now? This may "
                                      "take a while."),
                                      status.release.label, status.installed);
            else
                lv_label_set_text_fmt(ota_offer_title,
                                      TR("Installed version: %s\nAvailable release: %s\n\nDownload and reinstall it anyway? "
                                      "This may take a while."),
                                      status.installed, status.release.label);
            gui_popup_show(&ota_offer_popup);
            return;
        case FIRMWARE_OTA_READY:
            gui_busy_hide(ota_busy);
            ota_ui_active = false;
            firmware_ota_reset(); /* the record on the card carries it from here */
            show_ota_install_popup(status.release.label);
            return;
        case FIRMWARE_OTA_FAILED:
            gui_busy_hide(ota_busy);
            ota_ui_active = false;
            show_error_toast(status.error);
            firmware_ota_reset();
            return;
        case FIRMWARE_OTA_IDLE:
            gui_busy_hide(ota_busy);
            ota_ui_active = false;
            return;
    }
}

void gui_updates_build_popups(void) {
    build_firmware_update_popup();
    build_firmware_ota_popups();
    build_player_update_popups();
}

void gui_updates_teardown(void) {
    if (player_update_busy) {
        gui_busy_hide(player_update_busy);
        player_update_busy = 0;
    }
    if (!player_update_ui_active)
        player_update_reset(); /* discard any result whose popup is being torn down */
    gui_popup_teardown(&player_update_offer_popup);
    gui_popup_teardown(&player_update_confirm_popup);
    player_update_offer_title = player_update_confirm_title = NULL;
    if (player_update_menu) { lv_obj_delete(player_update_menu); player_update_menu = NULL; }
    if (player_update_backdrop) { lv_obj_delete(player_update_backdrop); player_update_backdrop = NULL; }
    gui_popup_teardown(&firmware_update_popup);
    gui_popup_teardown(&ota_offer_popup);
    gui_popup_teardown(&ota_install_popup);
    ota_offer_title = ota_install_title = NULL;
    if (firmware_source_menu) { lv_obj_delete(firmware_source_menu); firmware_source_menu = NULL; }
    if (firmware_source_backdrop) { lv_obj_delete(firmware_source_backdrop); firmware_source_backdrop = NULL; }
}
