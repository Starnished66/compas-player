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

/* Writes paths[from..count) to a new temporary file; returns its name. */
static bool write_list(const char *const *paths, size_t from, size_t count, char *name, size_t size) {
    if (snprintf(name, size, "%s/sdtool-parse-XXXXXX", temp_directory()) >= (int) size) return false;
    int fd = mkstemp(name);
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!file) {
        if (fd >= 0) close(fd);
        return false;
    }
    bool ok = true;
    for (size_t i = from; i < count; i++) ok = fprintf(file, "%s\n", paths[i]) > 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (!ok) unlink(name);
    return ok;
}

bool sdtool_parse_files(const char *const *paths, size_t count, track_metadata_t *out, bool *ok) {
    size_t next = 0;
    for (size_t i = 0; i < count; i++) ok[i] = false;
    while (next < count) {
        char list[PATH_MAX];
        if (!write_list(paths, next, count, list, sizeof(list))) return false;
        FILE *worker = sdtool_worker_open(list);
        if (!worker) {
            unlink(list);
            return false;
        }
        size_t done = next;
        while (done < count && fread(&out[done], sizeof(out[done]), 1, worker) == 1) ok[done++] = true;
        sdtool_worker_close(worker);
        unlink(list);
        /* The worker stopped before answering for this file: it crashed or
         * overran its time. Skip it and carry on with the rest. */
        if (done < count) {
            memset(&out[done], 0, sizeof(out[done]));
            done++;
        }
        next = done;
    }
    return true;
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
 * die or stall on a file whose name contains <text>. */
static void test_fault(const char *path) {
    const char *fault = getenv("SDTOOL_TEST_FAULT");
    if (!fault || !strchr(fault, ':') || !strstr(path, strchr(fault, ':') + 1)) return;
    if (strncmp(fault, "crash:", 6) == 0) abort();
    if (strncmp(fault, "hang:", 5) == 0) for (;;) sleep(60);
}

int sdtool_parse_worker(void) {
    pthread_t thread;
    char line[PATH_MAX];
    if (pthread_create(&thread, NULL, watchdog, NULL) != 0) return 1;
    while (fgets(line, sizeof(line), stdin)) {
        track_metadata_t meta;
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
        memset(&meta, 0, sizeof(meta));
        started_at = time(NULL);
        test_fault(line);
        metadata_read_scan_tags(line, &meta);
        started_at = 0;
        if (fwrite(&meta, sizeof(meta), 1, stdout) != 1 || fflush(stdout) != 0) return 1;
    }
    return 0;
}
