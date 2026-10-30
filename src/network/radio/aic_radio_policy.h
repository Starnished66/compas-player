#ifndef AIC_RADIO_POLICY_H
#define AIC_RADIO_POLICY_H

#include <stdbool.h>

/* Pure decision logic for the AIC8800D40 combo chip, kept free of I/O so it
 * can be tested on the host. HiBy's v2.0 firmware makes Wi-Fi and Bluetooth
 * mutually exclusive on this chip: aic_wifi_load() fails while
 * /run/aic_bt_power_gate exists. This module decides which requests may run
 * for a given snapshot of the radios; aic_radio.c gathers the snapshot and
 * runs the vendor scripts. */

typedef enum {
    AIC_RADIO_WIFI_ON,
    AIC_RADIO_WIFI_OFF,
    AIC_RADIO_BT_ON,
    AIC_RADIO_BT_OFF
} aic_radio_request_t;

typedef enum {
    AIC_RADIO_RUN,               /* go ahead */
    AIC_RADIO_REFUSE_BT_ACTIVE,  /* Wi-Fi requested while Bluetooth is on */
    AIC_RADIO_REFUSE_WIFI_ACTIVE,/* Bluetooth requested while Wi-Fi is on */
    AIC_RADIO_REFUSE_UNAVAILABLE /* boot-time driver load failed or never finished */
} aic_radio_decision_t;

/* /run/r1_wireless_loaded: absent while the BSP loads in the background,
 * "ok" once bsp and btlpm are in, anything else on failure. */
typedef enum { AIC_BASE_PENDING, AIC_BASE_READY, AIC_BASE_FAILED } aic_base_state_t;

typedef struct {
    aic_base_state_t base;
    bool bt_active;   /* Bluetooth gate held, or hci0 present */
    bool wifi_active; /* fdrv loaded, or wpa_supplicant socket present */
} aic_radio_state_t;

/* text is the content of /run/r1_wireless_type, or NULL when unreadable.
 * Only the exact word "aic" (surrounding whitespace ignored) selects the AIC
 * path; anything else, including NULL, keeps the Broadcom behaviour. */
bool aic_radio_type_is_aic(const char * text);

/* text is the content of /run/r1_wireless_loaded, or NULL when missing. */
aic_base_state_t aic_radio_parse_base(const char * text);

/* Turning a radio off is always allowed. Turning one on needs the base
 * drivers and the other radio off. Neither radio is ever switched off on the
 * user's behalf, so running services and links are not dropped silently. */
aic_radio_decision_t aic_radio_decide(const aic_radio_state_t * state, aic_radio_request_t request);

/* Stock hiby_player refuses to suspend unless hci0 and aic8800_fdrv are both
 * gone ("AIC radio shutdown incomplete; skip suspend"). */
bool aic_radio_suspend_safe(bool hci0_present, bool fdrv_loaded);

#endif /* AIC_RADIO_POLICY_H */
