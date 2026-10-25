#include "parse_pool.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PARSE_TIMEOUT_SECONDS 5

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
                                         track_metadata_t *out, bool *ok) {
    size_t next = 0;
    for (size_t i = 0; i < count; i++) ok[i] = false;
    while (next < count) {
        char list[PATH_MAX];
        if (!write_list(paths, next, count, list, sizeof(list))) return SDTOOL_PARSE_WORKER_FAILED;
        sdtool_worker_t *worker = sdtool_worker_start(list);
        if (!worker) {
            unlink(list);
            return SDTOOL_PARSE_WORKER_FAILED;
        }
        FILE *answers = sdtool_worker_output(worker);
        size_t done = next;
        sdtool_parse_record_t *record = malloc(sizeof(*record));
        while (record && done < count && fread(record, sizeof(*record), 1, answers) == 1) {
            if (record->index != done - next) {
                break;
            }
            out[done] = record->meta;
            ok[done++] = true;
        }
        free(record);
        int status = sdtool_worker_finish(worker);
        unlink(list);

        /* A worker that could not start (126/127), or exited cleanly without
         * answering, is a broken setup. One that died or timed out on a file
         * is that file's fault, handled below. */
        if (done == next && (status == 0 || status == 126 || status == 127))
            return SDTOOL_PARSE_WORKER_FAILED;
        /* The worker stopped, or answered for another file, before this one:
         * it crashed, overran its time or lost step. Fail it and go on. */
        if (done < count) {
            memset(&out[done], 0, sizeof(out[done]));
            ok[done++] = false;
        }
        next = done;
    }
    return SDTOOL_PARSE_OK;
}

static volatile time_t started_at; /* 0 while idle */

static void *watchdog(void *unused) {
    (void) unused;
    for (;;) {
        usleep(200000);
        time_t started = started_at;
        if (started && time(NULL) - started > PARSE_TIMEOUT_SECONDS) _exit(3);
    }
    return NULL;
}

/* For the tests: SDTOOL_TEST_FAULT=crash:<text> or hang:<text> makes the worker
 * die or stall on a file whose name contains <text>; SDTOOL_TEST_FAULT=start
 * makes it exit cleanly before answering anything. */
static void test_fault(const char *path) {
    const char *fault = getenv("SDTOOL_TEST_FAULT");
    if (!fault || !strchr(fault, ':') || !strstr(path, strchr(fault, ':') + 1)) return;
    if (strncmp(fault, "crash:", 6) == 0) abort();
    if (strncmp(fault, "hang:", 5) == 0) for (;;) sleep(60);
}

int sdtool_parse_worker(void) {
    pthread_t thread;
    const char *fault = getenv("SDTOOL_TEST_FAULT");
    uint32_t length, index = 0;
    if (fault && strcmp(fault, "start") == 0) return 0;
    if (pthread_create(&thread, NULL, watchdog, NULL) != 0) return 1;
    while (fread(&length, sizeof(length), 1, stdin) == 1) {
        char *path = length < 1u << 20 ? malloc((size_t) length + 1) : NULL;
        if (!path || fread(path, 1, length, stdin) != length) return 1;
        path[length] = '\0';
        sdtool_parse_record_t *record = calloc(1, sizeof(*record));
        if (!record) return 1;
        record->index = index++;
        started_at = time(NULL);
        test_fault(path);
        metadata_read_scan_tags(path, &record->meta);
        started_at = 0;
        bool written = fwrite(record, sizeof(*record), 1, stdout) == 1 && fflush(stdout) == 0;
        free(record);
        free(path);
        if (!written) return 1;
    }
    return 0;
}
