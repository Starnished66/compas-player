#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "gui_network.h"
#include "../widgets/builders/screen_builders.h"
#include "../../network/bluetooth/control/bt_pairing_agent.h"
#include <stdio.h>
#include "gui_network_internal.h"

/* ---- Bluetooth pairing prompts -------------------------------------------
 * Two popups on lv_layer_top(), so they show over whatever screen is active:
 * numeric comparison (device name, code, Pair / Cancel) and a code the user
 * types on the other device (device name, code, OK). The agent thread owns the
 * requests; poll_bt_pairing() mirrors them here and the buttons only post the
 * answer. A request id is shown once: dismissing a prompt keeps its id, so the
 * poll does not bring it back. */

typedef struct {
    gui_popup_t popup;
    lv_obj_t * name_label;
    lv_obj_t * code_label;
    uint32_t shown_id; /* id of the request last shown (0 = none) */
} pairing_popup_t;

static pairing_popup_t confirm_popup;
static pairing_popup_t display_popup;

static lv_obj_t * pairing_label(lv_obj_t * popup, lv_style_t * color_style, gui_font_role_t font_role) {
    lv_obj_t * label = lv_label_create(popup);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(label, color_style, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(font_role), 0);
    return label;
}

static void confirm_answer_cb(lv_event_t * e) {
    bool accept = lv_event_get_user_data(e) != NULL;
    if (confirm_popup.shown_id) bt_pairing_agent_answer_confirmation(confirm_popup.shown_id, accept);
    gui_popup_hide(&confirm_popup.popup);
}

static void display_close_cb(lv_event_t * e) {
    (void) e;
    gui_popup_hide(&display_popup.popup);
}

/* Title, device name, code, hint; the caller adds the buttons. */
static void build_pairing_popup(pairing_popup_t * p, lv_event_cb_t backdrop_cb, const char * hint) {
    lv_obj_t * title = NULL;
    build_titled_popup(&p->popup, backdrop_cb, &title, NULL);
    lv_label_set_text(title, TR("Bluetooth pairing"));
    p->name_label = pairing_label(p->popup.popup, &style_theme_text_muted, GUI_FONT_ROLE_BODY);
    p->code_label = pairing_label(p->popup.popup, &style_theme_text_primary, GUI_FONT_ROLE_TITLE);
    lv_label_set_text(pairing_label(p->popup.popup, &style_theme_text_muted, GUI_FONT_ROLE_SUBTEXT), hint);
}

void gui_network_pairing_build(void) {
    build_pairing_popup(&confirm_popup, NULL, TR("Confirm the same code is shown on the other device."));
    add_popup_action_row(confirm_popup.popup.popup, TR("Pair"), accent_lv_color(), confirm_answer_cb, (void *) 1);
    add_popup_action_row(confirm_popup.popup.popup, TR("Cancel"), lv_color_hex(GUI_BREEZE_NEGATIVE), confirm_answer_cb,
                         NULL);
    build_pairing_popup(&display_popup, display_close_cb, TR("Type this code on the other device."));
    add_popup_action_row(display_popup.popup.popup, TR("Close"), accent_lv_color(), display_close_cb, NULL);
}

void gui_network_pairing_teardown(void) {
    gui_popup_teardown(&confirm_popup.popup);
    gui_popup_teardown(&display_popup.popup);
    confirm_popup.name_label = confirm_popup.code_label = NULL;
    display_popup.name_label = display_popup.code_label = NULL;
    confirm_popup.shown_id = display_popup.shown_id = 0;
}

/* Shows p for request id (once), or hides it when the request is gone. */
static void sync_pairing_popup(pairing_popup_t * p, bool present, uint32_t id, const char * name, const char * code) {
    if (!p->popup.popup) return;
    if (!present) {
        gui_popup_hide(&p->popup);
        p->shown_id = 0;
        return;
    }
    if (id == p->shown_id) return;
    p->shown_id = id;
    lv_label_set_text(p->name_label, name);
    lv_label_set_text(p->code_label, code);
    gui_popup_show(&p->popup);
}

void poll_bt_pairing(void) {
    char name[64];
    char code[17];
    uint32_t passkey = 0;
    uint32_t id = 0;
    bool present = bt_pairing_agent_pending_confirmation(name, sizeof(name), &passkey, &id);
    if (present) snprintf(code, sizeof(code), "%06u", (unsigned) passkey);
    sync_pairing_popup(&confirm_popup, present, id, name, code);

    present = bt_pairing_agent_display_code(name, sizeof(name), code, sizeof(code), &id);
    sync_pairing_popup(&display_popup, present, id, name, code);
    if (gui_popup_is_visible(&confirm_popup.popup) || gui_popup_is_visible(&display_popup.popup))
        gui_keep_screen_awake_for_prompt();
}
