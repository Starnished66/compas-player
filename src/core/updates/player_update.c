#define _POSIX_C_SOURCE 200809L

#include "player_update.h"

#include "../../hardware/power/battery.h"
#include "../../network/firmware/firmware_ota.h"
#include "../../network/http/client/http_client.h"
#include "firmware_update.h"
#include "../../ui/text/i18n/i18n.h"
#include "player_install.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define PLAYER_DATA_ROOT PLAYER_INSTALL_DATA_ROOT
#define PLAYER_SD_ROOT PLAYER_INSTALL_SD_ROOT
#define PLAYER_STAGING_DIR PLAYER_DATA_ROOT "/.compas/player-update"
#define PLAYER_SD_SOURCE PLAYER_INSTALL_SD_SOURCE
#define PLAYER_MAX_BYTES PLAYER_INSTALL_MAX_BYTES
#define PLAYER_SUMS_MAX (64u * 1024u)
#define PLAYER_CONNECT_MS 10000u
#define PLAYER_READ_MS 20000u

static pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;
static player_update_status_t update_status;
static bool worker_running;

static void set_state(player_update_state_t state, int percent) {
    pthread_mutex_lock(&update_mutex);
    update_status.state = state;
    update_status.percent = percent;
    pthread_mutex_unlock(&update_mutex);
}

static void fail_worker(const char *message) {
    pthread_mutex_lock(&update_mutex);
    firmware_update_release_ota();
    update_status.state = PLAYER_UPDATE_FAILED;
    update_status.percent = 0;
    snprintf(update_status.error, sizeof(update_status.error), "%s",
             message ? message : TR("Update failed."));
    worker_running = false;
    pthread_mutex_unlock(&update_mutex);
}

static void finish_worker(player_update_state_t state) {
    pthread_mutex_lock(&update_mutex);
    firmware_update_release_ota();
    update_status.state = state;
    update_status.percent = state == PLAYER_UPDATE_DONE ? 100 : update_status.percent;
    worker_running = false;
    pthread_mutex_unlock(&update_mutex);
}

static bool fetch_release(firmware_ota_release_t *release, char *error, size_t error_size) {
    return firmware_ota_fetch_release_for_asset(player_install_board_asset(), release, error,
                                                error_size);
}

static void *check_worker(void *unused) {
    (void)unused;
    firmware_ota_release_t release;
    char error[160] = "";
    if (!fetch_release(&release, error, sizeof(error))) {
        fail_worker(error);
        return NULL;
    }
    pthread_mutex_lock(&update_mutex);
    firmware_update_release_ota();
    update_status.release = release;
    update_status.state = PLAYER_UPDATE_CHECKED;
    worker_running = false;
    pthread_mutex_unlock(&update_mutex);
    return NULL;
}

static bool start_thread(void *(*entry)(void *), void *arg) {
    pthread_t thread;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return false;
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    (void)pthread_attr_setstacksize(&attr, 256u * 1024u);
    bool ok = pthread_create(&thread, &attr, entry, arg) == 0;
    pthread_attr_destroy(&attr);
    return ok;
}

static bool battery_allows_install(char *error, size_t error_size) {
    int battery = battery_get_percent();
#ifdef HOST_BUILD
    if (battery < 0)
        battery = 100;
#endif
    if (battery >= 0 && battery < 30 && !battery_is_charging()) {
        snprintf(error, error_size,
                 TR("Charge to at least 30%% or connect power before updating."));
        return false;
    }
    return true;
}

static bool ensure_private_stage_dir(void) {
    struct stat st;
    if (mkdir(PLAYER_DATA_ROOT, 0755) != 0 && errno != EEXIST)
        return false;
    int root = open(PLAYER_DATA_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0)
        return false;
    if (mkdirat(root, ".compas", 0755) != 0 && errno != EEXIST) {
        close(root);
        return false;
    }
    int compas = openat(root, ".compas", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(root);
    if (compas < 0)
        return false;
    if (mkdirat(compas, "player-update", 0700) != 0 && errno != EEXIST) {
        close(compas);
        return false;
    }
    int stage = openat(compas, "player-update", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(compas);
    if (stage < 0)
        return false;
    bool ok = fstat(stage, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
              (st.st_mode & 077) == 0;
    close(stage);
    return ok;
}

static bool enough_internal_space(uint64_t required_bytes) {
    struct statvfs fs;
    if (statvfs(PLAYER_DATA_ROOT, &fs) != 0)
        return false;
    uint64_t available = (uint64_t)fs.f_bavail * (uint64_t)fs.f_frsize;
    return available >= required_bytes;
}

static bool download_progress(uint64_t downloaded, uint64_t total, void *unused) {
    (void)unused;
    int percent = total ? (int)(downloaded * 100u / total) : 0;
    if (percent > 100)
        percent = 100;
    pthread_mutex_lock(&update_mutex);
    update_status.percent = percent;
    pthread_mutex_unlock(&update_mutex);
    return true;
}

static bool download_file_bounded_by_io_timeouts(const char *url, const char *path, size_t limit,
                                                 http_progress_cb_t progress, int *status) {
    http_cancel_token_t cancel;
    http_cancel_token_init(&cancel);
    bool ok = http_get_to_file_redirects(url, true, path, limit, progress, NULL, PLAYER_CONNECT_MS,
                                         PLAYER_READ_MS, &cancel, 5, status);
    http_cancel_token_destroy(&cancel);
    return ok;
}

static bool fetch_sums(const firmware_ota_release_t *release, char expected[65], char *error,
                       size_t error_size) {
    if (!ensure_private_stage_dir()) {
        snprintf(error, error_size, "%s", TR("Could not prepare secure update storage."));
        return false;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/checksums.part", PLAYER_STAGING_DIR);
    (void)unlink(path);
    int status = 0;
    if (!download_file_bounded_by_io_timeouts(release->sums_url, path, PLAYER_SUMS_MAX, NULL,
                                              &status)) {
        snprintf(error, error_size, "%s", TR("Could not download the release checksums."));
        unlink(path);
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    char *sums = NULL;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
              st.st_size <= PLAYER_SUMS_MAX;
    if (ok) {
        sums = malloc((size_t)st.st_size + 1);
        ok = sums != NULL;
    }
    size_t got = 0;
    while (ok && got < (size_t)st.st_size) {
        ssize_t n = read(fd, sums + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        got += (size_t)n;
    }
    if (fd >= 0)
        close(fd);
    unlink(path);
    if (ok) {
        sums[got] = '\0';
        ok = firmware_ota_parse_sums(sums, got, release->asset_name, expected);
    }
    free(sums);
    if (!ok)
        snprintf(error, error_size, "%s",
                 TR("The release does not provide a valid player checksum."));
    if (ok && release->asset_digest[0] && strcmp(expected, release->asset_digest) != 0) {
        snprintf(error, error_size, "%s",
                 TR("The release checksum does not match the published asset digest."));
        ok = false;
    }
    return ok;
}

static bool download_install(const firmware_ota_release_t *chosen, char *error, size_t error_size) {
    firmware_ota_release_t latest;
    if (!fetch_release(&latest, error, error_size))
        return false;
    if (strcmp(latest.tag, chosen->tag) != 0 ||
        strcmp(latest.asset_name, chosen->asset_name) != 0 ||
        latest.asset_size != chosen->asset_size ||
        strcmp(latest.asset_digest, chosen->asset_digest) != 0) {
        snprintf(error, error_size, "%s", TR("The latest player release changed. Check again."));
        return false;
    }
    char expected[65];
    if (!fetch_sums(&latest, expected, error, error_size))
        return false;
    if (latest.asset_size == 0 || latest.asset_size > PLAYER_MAX_BYTES) {
        snprintf(error, error_size, "%s", TR("The player image exceeds the safe size limit."));
        return false;
    }
    if (!ensure_private_stage_dir()) {
        snprintf(error, error_size, "%s", TR("Could not prepare secure update storage."));
        return false;
    }
    if (latest.asset_size > (UINT64_MAX - (8u << 20)) / 2u ||
        !enough_internal_space(latest.asset_size * 2u + (8u << 20))) {
        snprintf(error, error_size, "%s", TR("Not enough internal storage for the player update."));
        return false;
    }
    char part[256];
    snprintf(part, sizeof(part), "%s/player.part", PLAYER_STAGING_DIR);
    set_state(PLAYER_UPDATE_DOWNLOADING, 0);
    int status = 0;
    (void)unlink(part); /* stage directory is private; discard only our fixed
                           incomplete download */
    if (!download_file_bounded_by_io_timeouts(latest.asset_url, part, PLAYER_MAX_BYTES,
                                              download_progress, &status)) {
        unlink(part);
        snprintf(error, error_size, "%s", TR("The player download did not complete."));
        return false;
    }
    int fd = open(part, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    uint64_t size = 0;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
              (uint64_t)st.st_size == latest.asset_size;
    if (ok)
        size = (uint64_t)st.st_size;
    if (!ok) {
        if (fd >= 0)
            close(fd);
        unlink(part);
        snprintf(error, error_size, "%s", TR("The downloaded player failed SHA-256 verification."));
        return false;
    }
    set_state(PLAYER_UPDATE_INSTALLING, 0);
    ok = player_install_fd(fd, size, expected, true, false, NULL, NULL, error, error_size);
    close(fd);
    unlink(part);
    return ok;
}

static void *online_worker(void *arg) {
    firmware_ota_release_t release = *(firmware_ota_release_t *)arg;
    free(arg);
    char error[160] = "";
    if (!download_install(&release, error, sizeof(error))) {
        fail_worker(error);
        return NULL;
    }
    finish_worker(PLAYER_UPDATE_DONE);
    return NULL;
}

static void *sd_worker(void *unused) {
    (void)unused;
    char error[160] = "";
    int src = open(PLAYER_SD_SOURCE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat before;
    bool ok = src >= 0 && fstat(src, &before) == 0 && S_ISREG(before.st_mode) &&
              before.st_size > 0 && (uint64_t)before.st_size <= PLAYER_MAX_BYTES;
    if (ok && !player_install_preflight_fd(src, false, error, sizeof(error)))
        ok = false;
    if (ok && ((uint64_t)before.st_size > UINT64_MAX - (8u << 20) ||
               !enough_internal_space((uint64_t)before.st_size + (8u << 20)))) {
        ok = false;
        snprintf(error, sizeof(error), "%s",
                 TR("Not enough internal storage for the player update."));
    }
    if (ok) {
        set_state(PLAYER_UPDATE_INSTALLING, 0);
        ok = player_install_fd(src, (uint64_t)before.st_size, NULL, false, true, PLAYER_SD_SOURCE,
                               &before, error, sizeof(error));
    } else if (!error[0])
        snprintf(error, sizeof(error), "%s",
                 TR("Place a valid player executable at SD/.compas/compas_player."));
    if (src >= 0)
        close(src);
    if (!ok) {
        fail_worker(error);
        return NULL;
    }
    finish_worker(PLAYER_UPDATE_DONE);
    return NULL;
}

bool player_update_start_check(void) {
    if (!firmware_update_claim_ota())
        return false;
    pthread_mutex_lock(&update_mutex);
    if (worker_running) {
        pthread_mutex_unlock(&update_mutex);
        firmware_update_release_ota();
        return false;
    }
    memset(&update_status, 0, sizeof(update_status));
    update_status.state = PLAYER_UPDATE_CHECKING;
    worker_running = true;
    pthread_mutex_unlock(&update_mutex);
    if (!start_thread(check_worker, NULL)) {
        fail_worker(TR("Could not start the player update check."));
        return false;
    }
    return true;
}

bool player_update_start_online_install(char *error, size_t error_size) {
    if (!battery_allows_install(error, error_size))
        return false;
    if (!firmware_update_claim_ota()) {
        snprintf(error, error_size, "%s", TR("Another update is already active."));
        return false;
    }
    pthread_mutex_lock(&update_mutex);
    if (worker_running || update_status.state != PLAYER_UPDATE_CHECKED) {
        pthread_mutex_unlock(&update_mutex);
        firmware_update_release_ota();
        snprintf(error, error_size, "%s", TR("Check for a player update first."));
        return false;
    }
    firmware_ota_release_t *release = malloc(sizeof(*release));
    if (!release) {
        pthread_mutex_unlock(&update_mutex);
        firmware_update_release_ota();
        snprintf(error, error_size, "%s", TR("Not enough memory."));
        return false;
    }
    *release = update_status.release;
    worker_running = true;
    update_status.state = PLAYER_UPDATE_DOWNLOADING;
    update_status.percent = 0;
    pthread_mutex_unlock(&update_mutex);
    if (!start_thread(online_worker, release)) {
        free(release);
        fail_worker(TR("Could not start the player download."));
        return false;
    }
    return true;
}

bool player_update_start_sd_install(char *error, size_t error_size) {
    if (!battery_allows_install(error, error_size))
        return false;
    if (!firmware_update_claim_ota()) {
        snprintf(error, error_size, "%s", TR("Another update is already active."));
        return false;
    }
    pthread_mutex_lock(&update_mutex);
    if (worker_running) {
        pthread_mutex_unlock(&update_mutex);
        firmware_update_release_ota();
        snprintf(error, error_size, "%s", TR("Another update is already active."));
        return false;
    }
    memset(&update_status, 0, sizeof(update_status));
    update_status.state = PLAYER_UPDATE_INSTALLING;
    worker_running = true;
    pthread_mutex_unlock(&update_mutex);
    if (!start_thread(sd_worker, NULL)) {
        fail_worker(TR("Could not start the SD player update."));
        snprintf(error, error_size, "%s", TR("Could not start the SD player update."));
        return false;
    }
    return true;
}

void player_update_get_status(player_update_status_t *out) {
    if (!out)
        return;
    pthread_mutex_lock(&update_mutex);
    *out = update_status;
    pthread_mutex_unlock(&update_mutex);
}

void player_update_reset(void) {
    pthread_mutex_lock(&update_mutex);
    if (!worker_running)
        memset(&update_status, 0, sizeof(update_status));
    pthread_mutex_unlock(&update_mutex);
}
