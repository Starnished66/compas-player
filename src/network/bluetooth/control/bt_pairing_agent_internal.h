#ifndef BT_PAIRING_AGENT_INTERNAL_H
#define BT_PAIRING_AGENT_INTERNAL_H

/* Shared between bt_pairing_agent.c (D-Bus agent object, dispatch thread) and bt_pairing_prompt.c
 * (pairing window, UI prompts). Not part of the public API. */

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* bt_pairing_agent.c */
void bt_agent_send_empty_reply(DBusConnection * c, DBusMessage * msg);
void bt_agent_send_error_reply(DBusConnection * c, DBusMessage * msg, const char * error_name, const char * error_msg);
void bt_agent_send_basic_reply(DBusConnection * c, DBusMessage * msg, int type, const void * value);
bool bt_agent_device_is_paired(DBusConnection * c, const char * device);
void bt_agent_device_display_name(DBusConnection * c, const char * device, char * out, size_t size);

/* bt_pairing_prompt.c */
uint32_t bt_agent_monotonic_tenths(void);
bool bt_agent_take_discoverable_request(void);
bool bt_agent_pairing_window_open(void);
bool bt_agent_pairing_allowed(DBusConnection * c, DBusMessage * msg, const char * device);
void bt_agent_ui_state_clear(void);
bool bt_agent_confirm_begin(DBusConnection * c, DBusMessage * msg, const char * device, uint32_t passkey);
void bt_agent_display_begin(DBusConnection * c, const char * device, const char * code);
void bt_agent_ui_service(DBusConnection * c);
void bt_agent_revoke_trust_for(const char * device);

#endif
