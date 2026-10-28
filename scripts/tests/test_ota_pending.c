/* Stale or invalid OTA pending records versus an explicit SD install. */
#include "src/network/firmware/firmware_ota.h"
#include "src/core/updates/firmware_update.h"
#include "src/core/updates/firmware_image.h"
#include "src/core/system/runtime/app_version.h"
#include "src/hardware/power/battery.h"
#include "src/network/http/client/http_client.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- Explicit stubs for everything firmware_ota.c links against ---- */
static bool claim_ok = true;
static int claims, releases;
bool sd_card_root_is_mounted(void) { return true; }
bool firmware_update_busy(void) { return false; }
bool firmware_update_claim_ota(void) { if (claim_ok) claims++; return claim_ok; }
void firmware_update_release_ota(void) { releases++; }
void firmware_update_get_status(firmware_update_status_t * out) { memset(out, 0, sizeof(*out)); }
bool firmware_update_enter_ota_recovery_for_path(const char * path) { (void) path; return false; }
bool firmware_image_validate(const char * path, char * error, size_t error_size) {
    (void) path; (void) error; (void) error_size; return false;
}
const char * app_version_label(void) { return "test"; }
int battery_get_percent(void) { return 100; }
bool battery_is_charging(void) { return true; }
bool http_get_to_file_redirects(const char * url, bool verify_tls, const char * dest_path, size_t max_body_size,
                                http_progress_cb_t progress, void * context, uint32_t connect_ms, uint32_t read_ms,
                                http_cancel_token_t * cancel, int max_redirects, int * status) {
    (void) url; (void) verify_tls; (void) dest_path; (void) max_body_size; (void) progress; (void) context;
    (void) connect_ms; (void) read_ms; (void) cancel; (void) max_redirects; (void) status;
    return false;
}
bool http_request_ex(const http_request_t * req, http_cancel_token_t * cancel, http_response_t * response) {
    (void) req; (void) cancel; (void) response; return false;
}
void http_response_free(http_response_t * response) { (void) response; }

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const char * rec = ".compas/ota/pending";
static char line[256];

static void put(const char * path, const char * text) {
    FILE * f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}
static int exists(const char * path) { struct stat st; return lstat(path, &st) == 0; }
static void make_image(size_t size) {
    char path[256];
    snprintf(path, sizeof(path), "music/%s", firmware_ota_board_asset());
    FILE * f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) { if (size) { fseek(f, (long) size - 1, SEEK_SET); fputc(0, f); } fclose(f); }
}
static void remove_image(void) {
    char path[256];
    snprintf(path, sizeof(path), "music/%s", firmware_ota_board_asset());
    unlink(path);
}
static void clear_record(void) {
    if (rmdir(rec) != 0) unlink(rec);
}

int main(void) {
    char tmpl[] = "/tmp/ota_pending_XXXXXX";
    if (!mkdtemp(tmpl) || chdir(tmpl) != 0) return 2;
    mkdir("music", 0755);
    mkdir(".compas", 0755);
    mkdir(".compas/ota", 0755);
    snprintf(line, sizeof(line), "%s 40000000 %064d weekly-beta-2026-10-20\n", firmware_ota_board_asset(), 7);

    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_NONE);
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_NOT_STALE);

    /* Malformed leftover. */
    put(rec, "not a record at all with too many fields here\n");
    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_REJECTED);
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_REMOVED);
    CHECK(!exists(rec));

    /* Empty record file. */
    put(rec, "");
    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_REJECTED);
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_REMOVED);
    CHECK(!exists(rec));

    /* Well-formed record, image missing. */
    put(rec, line);
    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_REJECTED);
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_REMOVED);
    CHECK(!exists(rec));

    /* The real bug: the user's own image (different size) is at the root. */
    make_image(41000000);
    put(rec, line);
    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_REJECTED);
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_REMOVED);
    CHECK(!exists(rec));
    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_NONE);

    /* Claim refused: nothing is touched. */
    put(rec, "stale\n");
    claim_ok = false;
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_BUSY);
    CHECK(exists(rec));
    claim_ok = true;
    int c = claims, r = releases;
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_REMOVED);
    CHECK(claims == c + 1 && releases == r + 1); /* claim always released */

    /* Rejected marker: kept while the failed image is at the root... */
    put(rec, "rejected\n");
    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_REJECTED);
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_MARKER_KEPT);
    CHECK(exists(rec));
    /* ...and cleared once the user removed that image. */
    remove_image();
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_REMOVED);
    CHECK(!exists(rec));

    /* Unreadable record (a directory: open works, read fails): refused. */
    mkdir(rec, 0755);
    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_UNREADABLE);
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_IO_ERROR);
    CHECK(exists(rec));
    clear_record();

    /* A valid record (image present with the recorded size) is kept. */
    make_image(40000000);
    put(rec, line);
    firmware_ota_release_t rel;
    CHECK(firmware_ota_pending(&rel) == FIRMWARE_OTA_PENDING_VALID);
    CHECK(firmware_ota_discard_stale_pending() == FIRMWARE_OTA_DISCARD_NOT_STALE);
    CHECK(exists(rec));
    CHECK(firmware_ota_pending(NULL) == FIRMWARE_OTA_PENDING_VALID);

    /* Clean up the temp tree. */
    remove_image();
    clear_record();
    rmdir("music"); rmdir(".compas/ota"); rmdir(".compas");
    if (chdir("/") == 0) rmdir(tmpl);

    if (failures) return 1;
    puts("ota pending tests passed");
    return 0;
}
