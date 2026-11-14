/* USB drive hotplug: toasts and screen cleanup around core/storage/usb_drive.c. */
#include "../../text/i18n/i18n.h"
#include "gui_library_internal.h"
#include "gui_folder_actions.h"
#include "../../application/gui.h"
#include "../../shell/gui_notifications.h"
#include "../../../core/storage/usb_drive.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/browser/files/file_ops.h"

/* Runs on the UI poll. The watch and the mounting live off this thread; this
 * only reacts to what they report. A drive that went away is already out of
 * the storage list, so screens and playback let go of it first. */
void poll_usb_drive_hotplug(void) {
    usb_drive_events_t events;
    usb_drive_poll(&events);
    for (size_t i = 0; i < events.removed_count; i++) {
        gui_folder_actions_invalidate_storage_targets();
        gui_folder_actions_cancel();
        file_op_cancel(); /* a copy to or from the drive must not carry on */
        gui_player_handle_storage_removed(events.removed[i]);
    }
    if (events.changed) file_browser_storage_changed();
    if (events.mounted) show_info_toast(TR("USB storage mounted"));
    else if (events.failed) show_error_toast(TR("USB storage could not be mounted"));
}
