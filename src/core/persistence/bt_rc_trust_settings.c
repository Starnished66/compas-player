#include "bt_rc_trust_settings.h"
#include "../../network/bluetooth/control/bt_rc_trust.h"

#include <string.h>

void bt_rc_trust_settings_parse(player_settings_t * out, const char * value) {
    char mac[BT_RC_TRUST_MAC_SIZE];
    const char * fp = value ? strchr(value, ' ') : NULL;
    if (!fp || out->bt_rc_trusted_count >= BT_RC_TRUST_MAX) return;
    char mac_text[BT_RC_TRUST_MAC_SIZE + 1];
    if ((size_t) (fp - value) >= sizeof(mac_text)) return;
    memcpy(mac_text, value, (size_t) (fp - value));
    mac_text[fp - value] = '\0';
    fp++;
    if (!bt_rc_trust_normalize(mac_text, mac) || !bt_rc_trust_fingerprint_valid(fp)) return;
    for (int i = 0; i < out->bt_rc_trusted_count; i++)
        if (strcmp(out->bt_rc_trusted[i].mac, mac) == 0) return;
    memcpy(out->bt_rc_trusted[out->bt_rc_trusted_count].mac, mac, sizeof(mac));
    memcpy(out->bt_rc_trusted[out->bt_rc_trusted_count++].fingerprint, fp, BT_RC_TRUST_FP_SIZE);
}

void bt_rc_trust_settings_write(FILE * f, const player_settings_t * settings) {
    for (int i = 0; i < settings->bt_rc_trusted_count && i < BT_RC_TRUST_MAX; i++)
        fprintf(f, "bt_rc_trusted=%s %s\n", settings->bt_rc_trusted[i].mac, settings->bt_rc_trusted[i].fingerprint);
}

void bt_rc_trust_settings_apply(const player_settings_t * settings) {
    int count = settings->bt_rc_trusted_count < BT_RC_TRUST_MAX ? settings->bt_rc_trusted_count : BT_RC_TRUST_MAX;
    bt_rc_trust_import(settings->bt_rc_trusted, count);
}

bool bt_rc_trust_settings_sync(player_settings_t * settings) {
    bt_rc_trust_entry_t entries[BT_RC_TRUST_MAX];
    int count = 0;
    if (!bt_rc_trust_export_if_dirty(entries, &count)) return false;
    memcpy(settings->bt_rc_trusted, entries, sizeof(entries[0]) * (size_t) count);
    settings->bt_rc_trusted_count = count;
    return true;
}
