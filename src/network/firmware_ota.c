#include "firmware_ota.h"
#include "firmware_update.h"
#include "http_client.h"
#include "battery.h"
#include "app_version.h"
#include "storage_paths.h"
#include "cJSON.h"
#include "gui_library.h" /* sd_card_root_is_mounted() */
#include "mbedtls/sha256.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef OTA_RELEASES_URL
#define OTA_RELEASES_URL "https://api.github.com/repos/Starnished66/compas-player/releases?per_page=20"
#endif
#define OTA_TAG_PREFIX "weekly-beta-"
#define OTA_SUMS_NAME "SHA256SUMS"

#ifdef HOST_BUILD
#define OTA_SD_ROOT "./music"
#else
#define OTA_SD_ROOT "/data/mnt/sd_0"
#endif
#define OTA_WORK_DIR SD_COMPAS_ROOT "/ota"

/* Current images are ~38 MiB and the repacker refuses anything over
 * 45 MiB; a release asset outside this range is not a firmware image. */
#define OTA_MIN_IMAGE_BYTES (1u << 20)
#define OTA_MAX_IMAGE_BYTES (64u << 20)
#define OTA_MAX_SUMS_BYTES (64u << 10)
#define OTA_FREE_SPACE_MARGIN (8ull << 20)
#define OTA_MIN_BATTERY_PERCENT 30
#define OTA_CONNECT_TIMEOUT_MS 10000
#define OTA_READ_TIMEOUT_MS 20000

static pthread_mutex_t ota_mutex = PTHREAD_MUTEX_INITIALIZER;
static firmware_ota_status_t ota_status;
static bool ota_worker_running;

const char * firmware_ota_board_asset(void) {
#if defined(BOARD_R3PROII)
    return "r3proii.upt";
#elif defined(BOARD_R3II_2025)
    return "r3ii_2025.upt";
#else
    return "r1.upt";
#endif
}

/* ---- Pure helpers ---- */

static bool is_date(const char * s) {
    for (int i = 0; i < 10; i++) {
        bool dash = i == 4 || i == 7;
        if (dash ? s[i] != '-' : !isdigit((unsigned char) s[i])) return false;
    }
    return true;
}

bool firmware_ota_label_date(const char * label, char out_date[11]) {
    static const char prefix[] = "Weekly Beta ";
    out_date[0] = '\0';
    if (!label || strncmp(label, prefix, sizeof(prefix) - 1) != 0) return false;
    const char * date = label + sizeof(prefix) - 1;
    if (strlen(date) < 10 || !is_date(date) || (date[10] != '\0' && date[10] != ' ')) return false;
    memcpy(out_date, date, 10);
    out_date[10] = '\0';
    return true;
}

static const char * json_string(const cJSON * object, const char * key) {
    const cJSON * item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static bool copy_https_url(char * out, size_t size, const char * url) {
    return url && strncmp(url, "https://", 8) == 0 && strlen(url) < size &&
           snprintf(out, size, "%s", url) > 0;
}

bool firmware_ota_parse_releases(const char * json, size_t length, const char * asset_name,
                                 firmware_ota_release_t * out, char * error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    cJSON * root = cJSON_ParseWithLength(json, length);
    if (!cJSON_IsArray(root)) {
        snprintf(error, error_size, "Unexpected reply from GitHub");
        cJSON_Delete(root);
        return false;
    }
    bool found = false;
    const cJSON * release;
    cJSON_ArrayForEach(release, root) {
        const char * tag = json_string(release, "tag_name");
        size_t prefix_len = sizeof(OTA_TAG_PREFIX) - 1;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "draft")) || !tag ||
            strncmp(tag, OTA_TAG_PREFIX, prefix_len) != 0 || strlen(tag) != prefix_len + 10 ||
            !is_date(tag + prefix_len))
            continue;
        const char * date = tag + prefix_len;
        /* Newest by the date in the tag, not by list order. */
        if (found && strcmp(date, out->date) <= 0) continue;

        firmware_ota_release_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        bool have_asset = false, have_sums = false;
        const cJSON * asset;
        cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(release, "assets")) {
            const char * name = json_string(asset, "name");
            const char * url = json_string(asset, "browser_download_url");
            if (!name) continue;
            if (strcmp(name, asset_name) == 0) {
                const cJSON * size = cJSON_GetObjectItemCaseSensitive(asset, "size");
                if (cJSON_IsNumber(size) && size->valuedouble >= OTA_MIN_IMAGE_BYTES &&
                    size->valuedouble <= OTA_MAX_IMAGE_BYTES &&
                    copy_https_url(candidate.asset_url, sizeof(candidate.asset_url), url)) {
                    candidate.asset_size = (uint64_t) size->valuedouble;
                    have_asset = true;
                    const char * digest = json_string(asset, "digest");
                    if (digest && strncmp(digest, "sha256:", 7) == 0 && strlen(digest + 7) == 64) {
                        bool hex = true;
                        for (int i = 0; i < 64 && hex; i++) hex = isxdigit((unsigned char) digest[7 + i]) != 0;
                        for (int i = 0; hex && i < 64; i++)
                            candidate.asset_digest[i] = (char) tolower((unsigned char) digest[7 + i]);
                    }
                }
            } else if (strcmp(name, OTA_SUMS_NAME) == 0) {
                have_sums = copy_https_url(candidate.sums_url, sizeof(candidate.sums_url), url);
            }
        }
        /* A week without this board's image is skipped, not an error. */
        if (!have_asset || !have_sums) continue;
        snprintf(candidate.tag, sizeof(candidate.tag), "%s", tag);
        memcpy(candidate.date, date, 10);
        snprintf(candidate.asset_name, sizeof(candidate.asset_name), "%s", asset_name);
        *out = candidate;
        found = true;
    }
    cJSON_Delete(root);
    if (!found) snprintf(error, error_size, "No weekly release has a %s image", asset_name);
    return found;
}

bool firmware_ota_parse_sums(const char * text, size_t length, const char * asset_name, char out_hex[65]) {
    size_t name_len = strlen(asset_name);
    const char * end = text + length;
    for (const char * line = text; line < end;) {
        const char * eol = memchr(line, '\n', (size_t) (end - line));
        if (!eol) eol = end;
        /* sha256sum format: 64 hex digits, two separators ("  " or " *"),
         * then the file, which older releases wrote with a directory
         * ("release/r1.upt"): match on the name after the last '/'. */
        size_t line_len = (size_t) (eol - line);
        if (line_len && line[line_len - 1] == '\r') line_len--;
        const char * file = line + 66;
        size_t file_len = line_len > 66 ? line_len - 66 : 0;
        bool name_matches = file_len >= name_len && memcmp(file + file_len - name_len, asset_name, name_len) == 0 &&
                            (file_len == name_len || file[file_len - name_len - 1] == '/');
        if (line_len > 66 && line[64] == ' ' && (line[65] == ' ' || line[65] == '*') && name_matches) {
            bool hex = true;
            for (int i = 0; i < 64 && hex; i++) hex = isxdigit((unsigned char) line[i]) != 0;
            if (hex) {
                for (int i = 0; i < 64; i++) out_hex[i] = (char) tolower((unsigned char) line[i]);
                out_hex[64] = '\0';
                return true;
            }
        }
        line = eol + 1;
    }
    return false;
}

/* ---- Worker ---- */

static void set_failed(const char * message) {
    pthread_mutex_lock(&ota_mutex);
    ota_status.state = FIRMWARE_OTA_FAILED;
    snprintf(ota_status.error, sizeof(ota_status.error), "%s", message);
    ota_worker_running = false;
    pthread_mutex_unlock(&ota_mutex);
}

static const char * describe_http_error(const char * code) {
    if (code && strcmp(code, HTTP_ERR_TLS) == 0) return "Secure connection failed. Check Wi-Fi and the date and time.";
    if (code && (strcmp(code, HTTP_ERR_DNS) == 0 || strcmp(code, HTTP_ERR_CONNECT) == 0 ||
                 strcmp(code, HTTP_ERR_CONNECT_TIMEOUT) == 0))
        return "Cannot reach GitHub. Check the Wi-Fi connection.";
    if (code && strcmp(code, HTTP_ERR_TIMEOUT) == 0) return "GitHub did not respond in time. Try again.";
    return "Could not read the release list from GitHub.";
}

static void * check_worker(void * unused) {
    (void) unused;
    http_request_t request;
    memset(&request, 0, sizeof(request));
    snprintf(request.url, sizeof(request.url), "%s", OTA_RELEASES_URL);
    request.method = HTTP_METHOD_GET;
    request.verify_tls = true;
    request.connect_timeout_ms = OTA_CONNECT_TIMEOUT_MS;
    request.read_timeout_ms = OTA_READ_TIMEOUT_MS;
    request.total_timeout_ms = 45000;
    request.max_response_bytes = 2u << 20;
    request.redirect_limit = 3;
    snprintf(request.headers[0].name, sizeof(request.headers[0].name), "Accept");
    snprintf(request.headers[0].value, sizeof(request.headers[0].value), "application/vnd.github+json");
    snprintf(request.headers[1].name, sizeof(request.headers[1].name), "X-GitHub-Api-Version");
    snprintf(request.headers[1].value, sizeof(request.headers[1].value), "2022-11-28");
    request.header_count = 2;

    http_response_t response;
    bool ok = http_request_ex(&request, NULL, &response);
    if (!ok || !response.body) {
        const char * message = describe_http_error(ok ? NULL : response.error);
        http_response_free(&response);
        set_failed(message);
        return NULL;
    }
    if (response.status != 200) {
        char message[96];
        if (response.status == 403 || response.status == 429)
            snprintf(message, sizeof(message), "GitHub is limiting requests. Try again later.");
        else
            snprintf(message, sizeof(message), "GitHub returned HTTP %d.", response.status);
        http_response_free(&response);
        set_failed(message);
        return NULL;
    }

    firmware_ota_release_t release;
    char error[128];
    bool parsed = firmware_ota_parse_releases((const char *) response.body, response.body_len,
                                              firmware_ota_board_asset(), &release, error, sizeof(error));
    http_response_free(&response);
    if (!parsed) {
        set_failed(error);
        return NULL;
    }

    char installed_date[11];
    bool known = firmware_ota_label_date(app_version_label(), installed_date);
    pthread_mutex_lock(&ota_mutex);
    ota_status.release = release;
    ota_status.newer = !known || strcmp(release.date, installed_date) > 0;
    ota_status.state = FIRMWARE_OTA_CHECKED;
    ota_worker_running = false;
    pthread_mutex_unlock(&ota_mutex);
    return NULL;
}

static bool progress_cb(uint64_t downloaded, uint64_t total, void * user) {
    uint64_t expected = *(const uint64_t *) user;
    if (!total) total = expected;
    int percent = total ? (int) (downloaded * 100 / total) : 0;
    if (percent > 100) percent = 100;
    pthread_mutex_lock(&ota_mutex);
    ota_status.percent = percent;
    pthread_mutex_unlock(&ota_mutex);
    return true;
}

static bool sha256_file(const char * path, uint64_t * out_size, char out_hex[65]) {
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
    uint8_t buffer[16384];
    uint64_t size = 0;
    size_t n;
    while (ok && (n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        ok = mbedtls_sha256_update(&ctx, buffer, n) == 0;
        size += n;
    }
    if (ferror(f)) ok = false;
    fclose(f);
    uint8_t digest[32];
    if (ok) ok = mbedtls_sha256_finish(&ctx, digest) == 0;
    mbedtls_sha256_free(&ctx);
    if (!ok) return false;
    for (int i = 0; i < 32; i++) snprintf(out_hex + 2 * i, 3, "%02x", digest[i]);
    *out_size = size;
    return true;
}

/* Moves another .upt aside as <path>.parked (or .parked.1 ... .parked.9
 * when taken), never deleting or replacing a parked file. True when the
 * path is free afterwards. */
static bool park_file(const char * path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    char parked[PATH_MAX];
    for (int n = 0; n < 10; n++) {
        if (n == 0) snprintf(parked, sizeof(parked), "%s.parked", path);
        else snprintf(parked, sizeof(parked), "%s.parked.%d", path, n);
        if (lstat(parked, &st) == 0) continue;
        return rename(path, parked) == 0;
    }
    return false;
}

/* The verification record lives on the card beside the image, so it
 * survives a reboot and never vouches for an image on a different card.
 * One line: "<asset> <size> <sha256> <date>". */
#define OTA_PENDING_PATH OTA_WORK_DIR "/pending"
/* Record content once an image failed verification but could not be moved
 * out of the root. */
#define OTA_PENDING_REJECTED "rejected"

static bool write_pending(const firmware_ota_release_t * release, const char * sha256_hex) {
    char temp[sizeof(OTA_PENDING_PATH) + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", OTA_PENDING_PATH);
    FILE * f = fopen(temp, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s %llu %s %s\n", release->asset_name, (unsigned long long) release->asset_size,
                      sha256_hex, release->date) > 0;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(temp, OTA_PENDING_PATH) != 0) {
        unlink(temp);
        return false;
    }
    return true;
}

typedef struct {
    firmware_ota_release_t release;
    char sha256[65];
} ota_pending_t;

/* Reads the record and checks the named image is at the root with the
 * recorded size. The content is re-hashed at install time. False both
 * when there is no record and when it no longer matches; *present tells
 * them apart, and *unreadable flags a record that exists but could not be
 * opened (an SD error is not proof of absence). */
static bool read_pending(ota_pending_t * out, bool * present, bool * unreadable) {
    memset(out, 0, sizeof(*out));
    FILE * f = fopen(OTA_PENDING_PATH, "r");
    *unreadable = !f && errno != ENOENT;
    *present = f != NULL || *unreadable;
    if (!f) return false;
    unsigned long long size = 0;
    int fields = fscanf(f, "%31s %llu %64s %10s", out->release.asset_name, &size, out->sha256, out->release.date);
    /* A read error is not a mismatch: report it so nothing is quarantined. */
    if (ferror(f)) *unreadable = true;
    fclose(f);
    if (*unreadable) return false;
    if (fields != 4 || strcmp(out->release.asset_name, OTA_PENDING_REJECTED) == 0 || strcmp(out->release.asset_name, firmware_ota_board_asset()) != 0 ||
        strlen(out->sha256) != 64 || !is_date(out->release.date))
        return false;
    out->release.asset_size = size;
    char path[256];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", OTA_SD_ROOT, out->release.asset_name);
    if (stat(path, &st) != 0) {
        /* Only a missing image is a mismatch; other errors may be transient. */
        if (errno != ENOENT) *unreadable = true;
        return false;
    }
    return S_ISREG(st.st_mode) && (uint64_t) st.st_size == size;
}

/* The mount point exists without a card, so writing there would land on
 * internal storage. Also returns the card's device for later checks. */
static bool sd_mounted(dev_t * out_device) {
    struct stat st;
    if (!sd_card_root_is_mounted() || stat(OTA_SD_ROOT, &st) != 0) return false;
    if (out_device) *out_device = st.st_dev;
    return true;
}

static void * download_worker(void * arg) {
    firmware_ota_release_t release = *(firmware_ota_release_t *) arg;
    free(arg);
    dev_t card;
    if (!sd_mounted(&card)) {
        set_failed("Insert an SD card to download the update.");
        return NULL;
    }
    mkdir(SD_COMPAS_ROOT, 0755);
    mkdir(OTA_WORK_DIR, 0755);
    /* The previous record stays until the new one replaces it just before
     * the new image is published: if this download fails, the old image
     * keeps its verified path. */

    char sums_path[256], part_path[256], final_path[256];
    snprintf(sums_path, sizeof(sums_path), "%s/%s.part", OTA_WORK_DIR, OTA_SUMS_NAME);
    snprintf(part_path, sizeof(part_path), "%s/%s.part", OTA_WORK_DIR, release.asset_name);
    snprintf(final_path, sizeof(final_path), "%s/%s", OTA_SD_ROOT, release.asset_name);

    /* Checksums first: a release without a usable entry fails before the
     * large download. */
    int status = 0;
    if (!http_get_to_file_redirects(release.sums_url, true, sums_path, OTA_MAX_SUMS_BYTES, NULL, NULL,
                                    OTA_CONNECT_TIMEOUT_MS, OTA_READ_TIMEOUT_MS, NULL, 5, &status)) {
        set_failed("Could not download the release checksums.");
        return NULL;
    }
    char sums[OTA_MAX_SUMS_BYTES + 1];
    FILE * f = fopen(sums_path, "rb");
    size_t sums_len = f ? fread(sums, 1, OTA_MAX_SUMS_BYTES, f) : 0;
    if (f) fclose(f);
    unlink(sums_path);
    char expected[65];
    if (!firmware_ota_parse_sums(sums, sums_len, release.asset_name, expected)) {
        set_failed("The release has no checksum for this device's image.");
        return NULL;
    }
    /* GitHub's digest of the file actually attached must match the build's
     * checksum; a replaced asset is refused before the large download. */
    if (release.asset_digest[0] && strcmp(release.asset_digest, expected) != 0) {
        set_failed("This release's image does not match its checksums. Try again after the next weekly release.");
        return NULL;
    }

    struct statvfs fs;
    if (statvfs(OTA_SD_ROOT, &fs) != 0 ||
        (uint64_t) fs.f_bavail * fs.f_frsize < release.asset_size + OTA_FREE_SPACE_MARGIN) {
        set_failed("Not enough free space on the SD card for the update.");
        return NULL;
    }

    uint64_t expected_size = release.asset_size;
    if (!http_get_to_file_redirects(release.asset_url, true, part_path, (size_t) OTA_MAX_IMAGE_BYTES, progress_cb,
                                    &expected_size, OTA_CONNECT_TIMEOUT_MS, OTA_READ_TIMEOUT_MS, NULL, 5,
                                    &status)) {
        set_failed("The download did not complete. Check Wi-Fi and try again.");
        return NULL;
    }

    char actual[65];
    uint64_t actual_size = 0;
    if (!sha256_file(part_path, &actual_size, actual) || actual_size != release.asset_size ||
        strcmp(actual, expected) != 0) {
        unlink(part_path);
        set_failed("The downloaded image failed verification and was deleted.");
        return NULL;
    }
    /* Same card, so the verified image appears at the root in one step,
     * replacing an older copy of the same name. */
    dev_t card_now;
    if (!sd_mounted(&card_now) || card_now != card) {
        unlink(part_path);
        set_failed("The SD card changed during the download.");
        return NULL;
    }
    /* Record first, then publish: an image never reaches the root without
     * a record. If the rename then fails, the new record does not match
     * whatever is at the root, so installs stay blocked. */
    if (!write_pending(&release, expected)) {
        unlink(part_path);
        set_failed("Could not record the verified update on the SD card.");
        return NULL;
    }
    if (rename(part_path, final_path) != 0) {
        unlink(part_path);
        set_failed("Could not place the update on the SD card.");
        return NULL;
    }
    sync();

    pthread_mutex_lock(&ota_mutex);
    ota_status.percent = 100;
    ota_status.state = FIRMWARE_OTA_READY;
    ota_worker_running = false;
    pthread_mutex_unlock(&ota_mutex);
    return NULL;
}

static bool start_worker(void * (*fn)(void *), void * arg) {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024); /* sums buffer + TLS handshake */
    bool ok = pthread_create(&thread, &attr, fn, arg) == 0;
    pthread_attr_destroy(&attr);
    return ok;
}

bool firmware_ota_start_check(void) {
    pthread_mutex_lock(&ota_mutex);
    if (ota_worker_running) {
        pthread_mutex_unlock(&ota_mutex);
        return false;
    }
    memset(&ota_status, 0, sizeof(ota_status));
    snprintf(ota_status.installed, sizeof(ota_status.installed), "%s", app_version_label());
    ota_status.state = FIRMWARE_OTA_CHECKING;
    ota_worker_running = true;
    pthread_mutex_unlock(&ota_mutex);
    if (!start_worker(check_worker, NULL)) {
        set_failed("Could not start the update check.");
        return false;
    }
    return true;
}

bool firmware_ota_start_download(void) {
    pthread_mutex_lock(&ota_mutex);
    if (ota_worker_running || ota_status.state != FIRMWARE_OTA_CHECKED) {
        pthread_mutex_unlock(&ota_mutex);
        return false;
    }
    firmware_ota_release_t * release = malloc(sizeof(*release));
    if (!release) {
        pthread_mutex_unlock(&ota_mutex);
        return false;
    }
    *release = ota_status.release;
    ota_status.state = FIRMWARE_OTA_DOWNLOADING;
    ota_status.percent = 0;
    ota_worker_running = true;
    pthread_mutex_unlock(&ota_mutex);
    if (!start_worker(download_worker, release)) {
        free(release);
        set_failed("Could not start the download.");
        return false;
    }
    return true;
}

void firmware_ota_get_status(firmware_ota_status_t * out) {
    pthread_mutex_lock(&ota_mutex);
    *out = ota_status;
    pthread_mutex_unlock(&ota_mutex);
}

bool firmware_ota_busy(void) {
    pthread_mutex_lock(&ota_mutex);
    bool busy = ota_worker_running;
    pthread_mutex_unlock(&ota_mutex);
    return busy;
}

void firmware_ota_reset(void) {
    pthread_mutex_lock(&ota_mutex);
    if (!ota_worker_running) memset(&ota_status, 0, sizeof(ota_status));
    pthread_mutex_unlock(&ota_mutex);
}

static bool is_upt_name(const char * name) {
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".upt") == 0;
}

/* Takes a rejected image out of recovery's reach (renamed *.rejected, or
 * numbered). The record is removed only once the image is gone from the
 * root; otherwise it stays and keeps every install path refusing. */
static void reject_pending(const char * asset_name) {
    char path[256], rejected[280];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", OTA_SD_ROOT, asset_name);
    for (int n = 0; n < 10 && lstat(path, &st) == 0; n++) {
        if (n == 0) snprintf(rejected, sizeof(rejected), "%s.rejected", path);
        else snprintf(rejected, sizeof(rejected), "%s.rejected.%d", path, n);
        if (lstat(rejected, &st) == 0) continue;
        if (rename(path, rejected) != 0) break;
    }
    if (lstat(path, &st) != 0 && errno == ENOENT) {
        unlink(OTA_PENDING_PATH);
    } else {
        /* Still at the root: mark the record rejected so every query says
         * so. If even that write fails, the old record stays and the
         * install-time re-hash keeps refusing. */
        char temp[sizeof(OTA_PENDING_PATH) + 8];
        snprintf(temp, sizeof(temp), "%s.tmp", OTA_PENDING_PATH);
        FILE * f = fopen(temp, "w");
        bool ok = f && fputs(OTA_PENDING_REJECTED "\n", f) >= 0;
        if (f) {
            ok = fflush(f) == 0 && ok;
            ok = fsync(fileno(f)) == 0 && ok;
            ok = fclose(f) == 0 && ok; /* always closed */
        }
        if (!ok || rename(temp, OTA_PENDING_PATH) != 0) unlink(temp);
    }
    sync();
}

firmware_ota_pending_t firmware_ota_pending(firmware_ota_release_t * out) {
    ota_pending_t pending;
    bool present = false, unreadable = false;
    if (!sd_mounted(NULL)) return FIRMWARE_OTA_PENDING_NONE;
    bool valid = read_pending(&pending, &present, &unreadable);
    if (unreadable) return FIRMWARE_OTA_PENDING_UNREADABLE;
    if (valid) {
        if (out) *out = pending.release;
        return FIRMWARE_OTA_PENDING_VALID;
    }
    if (!present) return FIRMWARE_OTA_PENDING_NONE;
    reject_pending(firmware_ota_board_asset());
    return FIRMWARE_OTA_PENDING_REJECTED;
}

static void * install_worker(void * unused) {
    (void) unused;
    ota_pending_t pending;
    bool present = false, unreadable = false;
    if (!sd_mounted(NULL) || !read_pending(&pending, &present, &unreadable)) {
        if (present && !unreadable) reject_pending(firmware_ota_board_asset());
        set_failed("No verified update is on this SD card. Download it again.");
        return NULL;
    }
    /* Re-hash what is actually at the root now: the card or the file may
     * have changed since the download. */
    char path[256], actual[65];
    uint64_t actual_size = 0;
    snprintf(path, sizeof(path), "%s/%s", OTA_SD_ROOT, pending.release.asset_name);
    if (!sha256_file(path, &actual_size, actual)) {
        /* An I/O error proves nothing about the image: keep it and its
         * record so the install can be retried. */
        set_failed("Cannot read the update file on the SD card. Check the card and try again.");
        return NULL;
    }
    if (actual_size != pending.release.asset_size || strcmp(actual, pending.sha256) != 0) {
        reject_pending(pending.release.asset_name);
        set_failed("The update file on the SD card changed. Download it again.");
        return NULL;
    }

    /* Recovery flashes a .upt from the card root; leave it only ours. */
    DIR * dir = opendir(OTA_SD_ROOT);
    if (!dir) {
        set_failed("Cannot read the SD card.");
        return NULL;
    }
    struct dirent * entry;
    bool parked_ok = true;
    /* A read error must not look like the end of the listing: an unseen
     * .upt would stay beside ours. */
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) parked_ok = false;
            break;
        }
        if (entry->d_name[0] == '.' || !is_upt_name(entry->d_name) ||
            strcasecmp(entry->d_name, pending.release.asset_name) == 0)
            continue;
        char from[sizeof(OTA_SD_ROOT) + 1 + NAME_MAX + 1];
        snprintf(from, sizeof(from), "%s/%.*s", OTA_SD_ROOT, NAME_MAX, entry->d_name);
        if (!park_file(from)) parked_ok = false;
    }
    closedir(dir);
    if (!parked_ok) {
        set_failed("Could not move other .upt files aside on the SD card.");
        return NULL;
    }
    sync();
    firmware_update_enter_recovery(); /* does not return on the device */
    set_failed("Could not enter recovery mode.");
    return NULL;
}

bool firmware_ota_start_install(char * error, size_t error_size) {
    if (firmware_ota_pending(NULL) != FIRMWARE_OTA_PENDING_VALID) {
        snprintf(error, error_size, "No verified update is on this SD card. Download it again.");
        return false;
    }
    int battery = battery_get_percent();
#ifdef HOST_BUILD
    if (battery < 0) battery = 100; /* the host has no battery */
#endif
    if (battery < OTA_MIN_BATTERY_PERCENT && !battery_is_charging()) {
        snprintf(error, error_size, "Charge to at least %d%% or connect power before updating.",
                 OTA_MIN_BATTERY_PERCENT);
        return false;
    }
    pthread_mutex_lock(&ota_mutex);
    if (ota_worker_running) {
        pthread_mutex_unlock(&ota_mutex);
        snprintf(error, error_size, "An update is already in progress.");
        return false;
    }
    ota_status.state = FIRMWARE_OTA_INSTALLING;
    ota_worker_running = true;
    pthread_mutex_unlock(&ota_mutex);
    if (!start_worker(install_worker, NULL)) {
        set_failed("Could not start the installation.");
        snprintf(error, error_size, "Could not start the installation.");
        return false;
    }
    return true;
}
