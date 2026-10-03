#ifndef FIRMWARE_OTA_H
#define FIRMWARE_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Online firmware update from the latest weekly release on GitHub.
 *
 * Check: reads GitHub's authoritative /releases/latest object. That object
 * must be a published, non-prerelease weekly-beta-YYYY-MM-DD release (never
 * the staging-image-base release, which holds the unmodified base image),
 * and must carry this board's image (r1.upt, r3proii.upt or r3ii_2025.upt)
 * and SHA256SUMS. Missing or unsuitable assets fail closed; older releases
 * are never considered as a fallback.
 *
 * Download: fetches SHA256SUMS, then streams the image into
 * SD/.compas/ota/, checks its size and SHA-256 against the release, and
 * only then moves it to the SD card root under its release name,
 * replacing an older copy of the same name. Nothing unverified ever
 * reaches the root, where the recovery image looks.
 *
 * A verified download leaves a record (asset, size, SHA-256) in
 * SD/.compas/ota/pending, on the same card, so it can be installed later.
 *
 * Install (a worker): re-checks that the record still describes GitHub's
 * latest release and re-hashes the image at the root against that record,
 * moves any other *.upt out of the root (renamed *.upt.parked, so recovery
 * cannot pick a different image) and reboots into recovery via
 * firmware_update_enter_ota_recovery_for_path(), the same preparation as the SD-card
 * update.
 *
 * The check and download run on a worker thread; the UI polls
 * firmware_ota_get_status(). No function here touches LVGL. */

typedef enum {
    FIRMWARE_OTA_IDLE,
    FIRMWARE_OTA_CHECKING,
    FIRMWARE_OTA_CHECKED,     /* release describes the latest weekly */
    FIRMWARE_OTA_DOWNLOADING, /* percent is valid */
    FIRMWARE_OTA_READY,       /* verified image is on the SD root */
    FIRMWARE_OTA_INSTALLING,  /* re-verifying and parking before recovery */
    FIRMWARE_OTA_FAILED       /* error explains why */
} firmware_ota_state_t;

typedef struct {
    char tag[64];         /* weekly-beta-YYYY-MM-DD */
    char date[11];        /* YYYY-MM-DD */
    char asset_name[32];  /* this board's image name */
    char asset_url[512];
    char sums_url[512];
    uint64_t asset_size;
    char asset_digest[65]; /* GitHub's own SHA-256 of the asset, lowercase hex; empty if not published */
} firmware_ota_release_t;

typedef struct {
    firmware_ota_state_t state;
    int percent;
    firmware_ota_release_t release;
    bool newer;            /* the release is newer than the installed build (or that is unknown) */
    char installed[64];    /* installed release label */
    char error[160];
} firmware_ota_status_t;

/* This board's release asset name. */
const char * firmware_ota_board_asset(void);

/* Starts a check; false if one is already running or the thread fails. */
bool firmware_ota_start_check(void);
/* Downloads and verifies the release found by the last successful check. */
bool firmware_ota_start_download(void);
void firmware_ota_get_status(firmware_ota_status_t * out);
/* True while a check, download or install worker runs; idle suspend,
 * radio power-down and idle shutdown wait for it. */
bool firmware_ota_busy(void);
/* Back to IDLE after CHECKED, READY or FAILED (never while working). */
void firmware_ota_reset(void);

/* Whether the mounted card holds a verified download. VALID fills *out's
 * asset name, size and date (content is re-hashed at install). REJECTED
 * means the record is invalid. This query never changes files; the install
 * worker re-hashes the image and parks rejected content while holding the
 * shared update claim. UNREADABLE means the record could not be opened (an SD
 * error): refuse rather than fall back. NONE means no record (or no card). */
typedef enum {
    FIRMWARE_OTA_PENDING_NONE,
    FIRMWARE_OTA_PENDING_VALID,
    FIRMWARE_OTA_PENDING_REJECTED,
    FIRMWARE_OTA_PENDING_UNREADABLE /* a record exists but could not be read: refuse */
} firmware_ota_pending_t;
firmware_ota_pending_t firmware_ota_pending(firmware_ota_release_t * out);
/* Starts the install worker; refuses with a message when nothing verified
 * is pending or the battery is under 30% and not charging. Progress and
 * failure are reported through firmware_ota_get_status(); on success the
 * device reboots into recovery. */
bool firmware_ota_start_install(char * error, size_t error_size);

/* ---- Pure helpers, exposed for the host selftest ---- */

/* Parses GitHub's /releases/latest object; it never searches older releases. */
bool firmware_ota_parse_releases(const char * json, size_t length, const char * asset_name,
                                 firmware_ota_release_t * out, char * error, size_t error_size);
/* Finds asset_name's lowercase hex digest in a SHA256SUMS text. */
bool firmware_ota_parse_sums(const char * text, size_t length, const char * asset_name, char out_hex[65]);
/* Extracts YYYY-MM-DD from a "Weekly Beta YYYY-MM-DD..." label. */
bool firmware_ota_label_date(const char * label, char out_date[11]);

#endif /* FIRMWARE_OTA_H */
