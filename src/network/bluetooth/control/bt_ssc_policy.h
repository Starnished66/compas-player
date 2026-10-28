#ifndef BT_SSC_POLICY_H
#define BT_SSC_POLICY_H

/* Samsung SSC reliability policy: how an SSC-capable accessory is
 * recognised, how a codec reconfigure is told apart from a real disconnect,
 * and when SSC is backed off. Pure decisions and small tables; every time is
 * passed in (monotonic ms) and no subprocess is run, so the host tests drive
 * each case directly. The control layer owns the I/O and the locking of the
 * per-connection state it embeds. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_SSC_VENDOR_ID 0x0075u      /* Samsung's Bluetooth SIG company ID */
#define BT_SSC_CODEC_ID 0x0103u       /* Samsung Seamless/Scalable codec ID */
/* A switch in flight keeps the PCMRemoved grace open at most this long. */
#define BT_SSC_SWITCH_CAP_MS 3000u
/* Audio is "flowing" once the readback matched and the PCM has been up this
 * long: a later drop is out of range or a user action, not the codec. */
#define BT_SSC_FLOWING_MS 5000u
/* A queued discovery recovery that nothing takes up is dropped after this. */
#define BT_SSC_RECOVERY_WAIT_CAP_MS 30000u
/* Failed SSC attempts that start a persisted backoff, and its longest run of
 * skipped connections. */
#define BT_SSC_BACKOFF_AFTER 2u
#define BT_SSC_BACKOFF_MAX_SKIP 6u
#define BT_SSC_MEMORY_MAX 8
/* Switch-then-drop cycles to one codec in a session before it is rejected
 * for the session whatever the link state said. */
#define BT_SSC_CYCLE_CAP 2

#define BT_A2DP_SINK_UUID "0000110b-0000-1000-8000-00805f9b34fb"

typedef enum {
    BT_SSC_SIGNAL_NONE = 0,
    BT_SSC_SIGNAL_ENDPOINT,   /* SSC endpoint offered in BlueALSA's codecs */
    BT_SSC_SIGNAL_MODALIAS,   /* BlueZ Modalias vendor is Samsung */
    BT_SSC_SIGNAL_REMEMBERED, /* SSC worked on this device before */
    BT_SSC_SIGNAL_NAME        /* Galaxy Buds name, the weakest hint */
} bt_ssc_signal_t;

typedef enum {
    BT_SSC_LINK_UNKNOWN = 0,
    BT_SSC_LINK_CONNECTED,
    BT_SSC_LINK_DISCONNECTED
} bt_ssc_link_t;

/* Strongest signal, in the order endpoint, Modalias, remembered, name. */
bt_ssc_signal_t bt_ssc_policy_signal(bool endpoint, bool modalias, bool remembered, bool name);
const char * bt_ssc_policy_signal_name(bt_ssc_signal_t signal);

/* "Available codecs:" of `bluealsactl info` lists vendor 0x0075 / codec
 * 0x0103 as samsung-SC (or SSC); a bare "0075:0103" is accepted as well. */
bool bt_ssc_policy_endpoint_offered(const char * bluealsa_info);
/* `bluetoothctl info` carries "Modalias: bluetooth:v0075p...". */
bool bt_ssc_policy_modalias_is_samsung(const char * bluez_info);
/* A Modalias naming another vendor rules the name out (Pixel/OnePlus Buds). */
bool bt_ssc_policy_modalias_excludes_name(const char * bluez_info);
/* "Connected: yes|no" of `bluetoothctl info`. */
bt_ssc_link_t bt_ssc_policy_link_state(const char * bluez_info);

/* `bluealsactl info` text with the selected codec counted as offered: BlueZ
 * skips a remote endpoint that is "in use" (the Buds' own SSC stream), so a
 * live SSC stream may be missing from "Available codecs:". Never treat the
 * codec in use as unavailable. */
void bt_ssc_policy_offer_selected(char * info, size_t size);
/* Renders a Device1 Properties.GetAll reply (dbus-send --print-reply) as the
 * "Name:/Alias:/Modalias:/Connected:" lines of `bluetoothctl info`, without
 * starting bluetoothctl (which loads the whole BlueZ tree each time). */
void bt_ssc_policy_dbus_to_info(const char * dbus_out, char * out, size_t size);

/* Whether the pending discovery recovery may start: the control layer keys
 * it on the device's own MAC, so an empty cache (the top bar has not
 * refreshed, screen off) must not stop it. */
typedef enum { BT_SSC_RECOVERY_CANCEL = 0, BT_SSC_RECOVERY_START } bt_ssc_recovery_verdict_t;
bt_ssc_recovery_verdict_t bt_ssc_policy_recovery_verdict(const char * bt_codec, const char * cached_mac,
                                                         const char * pending_mac);

/* Per-device, per-connection state, embedded in the control layer's session
 * table. All-zero is the initial state. */
typedef struct {
    uint32_t switch_begin;  /* codec command issued, 0 when none */
    uint32_t pcm_back;      /* last PCMAdded, 0 until the PCM returns */
    uint32_t pcm_added_at;  /* this connection's PCM appeared; 0 when unseen */
    uint32_t confirmed;     /* readback matched the wanted codec */
    bool confirmed_ssc;
    bool conn_started;      /* backoff consulted for this connection */
    bool skip_ssc;          /* persisted backoff applies to this connection */
    bool endpoint_failure_counted;
    uint32_t keep_until;    /* our own reconnect is coming until then */
    bool session_noted;     /* a flowing SSC session was recorded */
} bt_ssc_dev_t;

void bt_ssc_dev_switch_begin(bt_ssc_dev_t * d, uint32_t now);
void bt_ssc_dev_switch_abort(bt_ssc_dev_t * d);
void bt_ssc_dev_pcm_added(bt_ssc_dev_t * d, uint32_t now);
void bt_ssc_dev_confirmed(bt_ssc_dev_t * d, uint32_t now, bool ssc);
/* A removal at removed_at while a switch is in flight (the PCM has not come
 * back, within the cap): the time the disconnect must be held back until, or
 * 0 when the normal grace applies. */
uint32_t bt_ssc_dev_reconfigure_deadline(const bt_ssc_dev_t * d, uint32_t removed_at);
/* Whether a drop at `dropped_at` counts against the codec: BlueZ must report
 * the device itself disconnected, and audio must not have been flowing. */
bool bt_ssc_dev_blames_codec(const bt_ssc_dev_t * d, uint32_t dropped_at, bt_ssc_link_t link);
/* A codec command waits until the PCM is this old, so it lands on an open
 * stream (an abort of a stream not yet OPEN drops the ACL). */
#define BT_SSC_STREAM_OPEN_MS 1000u
/* Time the stream counts as open, or 0 when the PCM's age is unknown. */
uint32_t bt_ssc_dev_stream_open_at(const bt_ssc_dev_t * d);
bool bt_ssc_dev_flowing(const bt_ssc_dev_t * d, uint32_t at);
/* A confirmed disconnect ends the connection; forgets what belongs to it.
 * Our own recovery or rate-correction reconnect (same_connection) keeps the
 * backoff bookkeeping, since it is the same connection. */
void bt_ssc_dev_connection_end(bt_ssc_dev_t * d, bool same_connection);

/* Per-device memory, persisted by the settings layer: SSC worked on this
 * device, failed attempts, and connections left to skip. */
typedef struct {
    char mac[18];
    bool worked;
    uint8_t failures;
    uint8_t skip;
} bt_ssc_memory_t;

void bt_ssc_memory_import(const bt_ssc_memory_t * entries, int count);
/* Copies the table out when it changed since the last export or import. */
bool bt_ssc_memory_export_if_dirty(bt_ssc_memory_t * out, int * count);
bool bt_ssc_memory_worked(const char * mac);
/* A copy of one device's entry, false when it has none. */
bool bt_ssc_memory_get(const char * mac, bt_ssc_memory_t * out);
/* Start of a connection that would try SSC: true when the persisted backoff
 * skips it. Least recently used entries are evicted, never-worked first. */
bool bt_ssc_memory_connection_begin(const char * mac);
void bt_ssc_memory_note_worked(const char * mac);
void bt_ssc_memory_note_failure(const char * mac);
/* A successful SSC session ends repeated failures. */
void bt_ssc_memory_note_session_ok(const char * mac);
void bt_ssc_memory_reset(void);

#endif /* BT_SSC_POLICY_H */
