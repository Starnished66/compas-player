#include "bt_ssc_settings.h"
#include "../../network/bluetooth/control/bt_ssc_policy.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool mac_is_valid(const char * mac) {
    if (strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++)
        if (i % 3 == 2 ? mac[i] != ':' : !isxdigit((unsigned char) mac[i])) return false;
    return true;
}

void bt_ssc_settings_parse(player_settings_t * out, const char * value) {
    char mac[18];
    unsigned int worked = 0, failures = 0, skip = 0;
    if (strlen(value) < 18 || value[17] != ',' || out->bt_ssc_device_count >= BT_DEVICE_RATE_MAX) return;
    memcpy(mac, value, 17);
    mac[17] = '\0';
    if (!mac_is_valid(mac) || sscanf(value + 18, "%u,%u,%u", &worked, &failures, &skip) != 3 ||
            worked > 1 || failures > 255 || skip > BT_SSC_BACKOFF_MAX_SKIP) return;
    int i = out->bt_ssc_device_count++;
    memcpy(out->bt_ssc_devices[i].mac, mac, sizeof(mac));
    out->bt_ssc_devices[i].worked = worked != 0;
    out->bt_ssc_devices[i].failures = (unsigned char) failures;
    out->bt_ssc_devices[i].skip = (unsigned char) skip;
}

void bt_ssc_settings_write(FILE * f, const player_settings_t * settings) {
    for (int i = 0; i < settings->bt_ssc_device_count && i < BT_DEVICE_RATE_MAX; i++)
        fprintf(f, "bt_ssc_device=%s,%d,%u,%u\n", settings->bt_ssc_devices[i].mac,
                settings->bt_ssc_devices[i].worked ? 1 : 0, settings->bt_ssc_devices[i].failures,
                settings->bt_ssc_devices[i].skip);
}

void bt_ssc_settings_apply(const player_settings_t * settings) {
    bt_ssc_memory_t entries[BT_SSC_MEMORY_MAX];
    int count = settings->bt_ssc_device_count < BT_SSC_MEMORY_MAX ? settings->bt_ssc_device_count
                                                                  : BT_SSC_MEMORY_MAX;
    for (int i = 0; i < count; i++) {
        memcpy(entries[i].mac, settings->bt_ssc_devices[i].mac, sizeof(entries[i].mac));
        entries[i].worked = settings->bt_ssc_devices[i].worked;
        entries[i].failures = settings->bt_ssc_devices[i].failures;
        entries[i].skip = settings->bt_ssc_devices[i].skip;
    }
    bt_ssc_memory_import(entries, count);
}

bool bt_ssc_settings_sync(player_settings_t * settings) {
    bt_ssc_memory_t entries[BT_SSC_MEMORY_MAX];
    int count = 0;
    if (!bt_ssc_memory_export_if_dirty(entries, &count)) return false;
    settings->bt_ssc_device_count = 0;
    for (int i = 0; i < count && i < BT_DEVICE_RATE_MAX; i++) {
        memcpy(settings->bt_ssc_devices[i].mac, entries[i].mac, sizeof(entries[i].mac));
        settings->bt_ssc_devices[i].worked = entries[i].worked;
        settings->bt_ssc_devices[i].failures = entries[i].failures;
        settings->bt_ssc_devices[i].skip = entries[i].skip;
        settings->bt_ssc_device_count++;
    }
    return true;
}
