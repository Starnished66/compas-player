#include "parse_pool.h"
#include "../../../library/metadata/parsing/metadata_parser_internal.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Wall-clock limit per file. Generous because tags are read from slow card
 * readers and busy machines; a stalled read still ends. SDTOOL_PARSE_TIMEOUT
 * (seconds) overrides it, so the tests need not wait. */
#define PARSE_TIMEOUT_SECONDS 30

static const char *temp_directory(void) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = getenv("TEMP");
    return dir && *dir ? dir : "/tmp";
}

/* Writes paths[from..count) to a new temporary file as length-prefixed
 * records; returns its name. */
static bool write_list(const char *const *paths, size_t from, size_t count, char *name, size_t size) {
    if (snprintf(name, size, "%s/sdtool-parse-XXXXXX", temp_directory()) >= (int) size) return false;
    int fd = mkstemp(name);
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!file) {
        if (fd >= 0) close(fd);
        return false;
    }
    bool ok = true;
    for (size_t i = from; i < count; i++) {
        uint32_t length = (uint32_t) strlen(paths[i]);
        ok = fwrite(&length, sizeof(length), 1, file) == 1 && fwrite(paths[i], 1, length, file) == length && ok;
    }
    ok = fclose(file) == 0 && ok;
    if (!ok) unlink(name);
    return ok;
}

sdtool_parse_result_t sdtool_parse_files(const char *const *paths, size_t count,
                                         track_metadata_t *out, bool *ok, size_t *broken) {
    size_t next = 0, retry = SIZE_MAX;
    bool answered = false;
    *broken = 0;
    for (size_t i = 0; i < count; i++) ok[i] = false;
    while (next < count) {
        char list[PATH_MAX];
        if (!write_list(paths, next, count, list, sizeof(list))) return SDTOOL_PARSE_WORKER_FAILED;
        sdtool_worker_t *worker = sdtool_worker_start(list, false);
        if (!worker) {
            unlink(list);
            return SDTOOL_PARSE_WORKER_FAILED;
        }
        FILE *answers = sdtool_worker_output(worker);
        size_t done = next;
        sdtool_parse_record_t *record = malloc(sizeof(*record));
        while (record && done < count && fread(record, sizeof(*record), 1, answers) == 1) {
            if (record->index != done - next) break;
            out[done] = record->meta;
            ok[done++] = true;
            answered = true;
        }
        free(record);
        int status = sdtool_worker_finish(worker);
        unlink(list);

        if (done == count) break;
        /* The worker stopped, or answered for another file, before this one.
         * Exit 126/127, or a clean exit from a worker that never answered
         * anything, is a broken setup. A clean exit later is given one more
         * try in a fresh worker. Anything else (a signal, the watchdog) is
         * this file's fault: fail it and go on. */
        bool quiet = status == 0 || status == 126 || status == 127;
        if (done == next && (status == 126 || status == 127 || (status == 0 && !answered && retry == SIZE_MAX)))
            return SDTOOL_PARSE_WORKER_FAILED;
        if (status == 0 && retry != done) {
            retry = done;
            next = done;
            continue;
        }
        if (quiet) (*broken)++;
        memset(&out[done], 0, sizeof(out[done]));
        ok[done++] = false;
        next = done;
    }
    return SDTOOL_PARSE_OK;
}

static volatile time_t started_at; /* 0 while idle */

static void *watchdog(void *unused) {
    const char *override = getenv("SDTOOL_PARSE_TIMEOUT");
    long limit = override ? atol(override) : 0;
    if (limit <= 0) limit = PARSE_TIMEOUT_SECONDS;
    for (;;) {
        usleep(200000);
        time_t started = started_at;
        if (started && time(NULL) - started > limit) _exit(3);
    }
    return NULL;
}

/* For the tests: SDTOOL_TEST_FAULT=crash:<text> or hang:<text> makes the worker
 * die or stall on a file whose name contains <text>, exit:<text> makes it exit
 * cleanly on such a file unless it is the worker's first; SDTOOL_TEST_FAULT=start
 * makes it exit cleanly before answering anything. */
static void test_fault(const char *path, uint32_t worker_index) {
    const char *fault = getenv("SDTOOL_TEST_FAULT");
    if (!fault || !strchr(fault, ':') || !strstr(path, strchr(fault, ':') + 1)) return;
    if (strncmp(fault, "crash:", 6) == 0) abort();
    if (strncmp(fault, "exit:", 5) == 0 && worker_index > 0) exit(0);
    if (strncmp(fault, "hang:", 5) == 0) for (;;) sleep(60);
}

sdtool_art_result_t sdtool_parse_artwork(const char *path, uint8_t **data, uint32_t *size) {
    char list[PATH_MAX];
    const char *paths[1] = { path };
    uint32_t header[2] = { 0, 0 }; /* file index, picture size */
    sdtool_art_result_t result = SDTOOL_ART_FAILED;
    *data = NULL;
    *size = 0;
    if (!write_list(paths, 0, 1, list, sizeof(list))) return result;
    sdtool_worker_t *worker = sdtool_worker_start(list, true);
    if (worker) {
        FILE *answers = sdtool_worker_output(worker);
        if (fread(header, sizeof(header), 1, answers) == 1 && header[0] == 0 && header[1] <= EMBEDDED_COVER_MAX_BYTES) {
            if (header[1] == 0) {
                result = SDTOOL_ART_NONE;
            } else if ((*data = malloc(header[1])) && fread(*data, 1, header[1], answers) == header[1]) {
                *size = header[1];
                result = SDTOOL_ART_FOUND;
            }
        }
        sdtool_worker_finish(worker);
    }
    unlink(list);
    if (result != SDTOOL_ART_FOUND) {
        free(*data);
        *data = NULL;
    }
    return result;
}

int sdtool_parse_worker(bool artwork) {
    pthread_t thread;
    const char *fault = getenv("SDTOOL_TEST_FAULT");
    uint32_t length, index = 0;
    if (fault && strcmp(fault, "start") == 0) return 0;
    if (pthread_create(&thread, NULL, watchdog, NULL) != 0) return 1;
    if (artwork) metadata_parser_enable_artwork_only();
    while (fread(&length, sizeof(length), 1, stdin) == 1) {
        char *path = length < 1u << 20 ? malloc((size_t) length + 1) : NULL;
        if (!path || fread(path, 1, length, stdin) != length) return 1;
        path[length] = '\0';
        sdtool_parse_record_t *record = calloc(1, sizeof(*record));
        if (!record) return 1;
        record->index = index;
        started_at = time(NULL);
        test_fault(path, index++);
        bool written;
        if (artwork) {
            metadata_read(path, &record->meta);
            uint32_t size = record->meta.picture_size <= EMBEDDED_COVER_MAX_BYTES && record->meta.picture_data
                                ? record->meta.picture_size : 0;
            uint32_t header[2] = { record->index, size };
            written = fwrite(header, sizeof(header), 1, stdout) == 1 &&
                      (size == 0 || fwrite(record->meta.picture_data, 1, size, stdout) == size) &&
                      fflush(stdout) == 0;
        } else {
            metadata_read_without_artwork(path, &record->meta);
            written = fwrite(record, sizeof(*record), 1, stdout) == 1 && fflush(stdout) == 0;
        }
        started_at = 0;
        free(record->meta.picture_data);
        free(record->meta.lyrics);
        free(record);
        free(path);
        if (!written) return 1;
    }
    return 0;
}
