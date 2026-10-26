#include "../../../../src/library/metadata/parsing/metadata.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef METADATA_ISOLATION_SOURCE
#error "runner must select the frozen fixture or production isolation source"
#endif
#include METADATA_ISOLATION_SOURCE

static bool helper_artwork_only;
static int helper_priority;
static int direct_scan_calls;
pid_t test_parent_pid;
static int parent_admission_priority = -1;

void metadata_parser_enable_artwork_only(void) {
    helper_artwork_only = true;
}

static void fill_scan_metadata(const char * path, track_metadata_t * out) {
    memset(out, 0, sizeof(*out));
    if (strcmp(path, "scan-crash.flac") == 0) _exit(23);
    if (strcmp(path, "scan-timeout.flac") == 0) {
        struct timespec delay = {.tv_sec = 2, .tv_nsec = 0};
        nanosleep(&delay, NULL);
    }
    snprintf(out->title, sizeof(out->title), "%s", "scan-child-ok");
    out->has_title = true;
    /* The production wrapper frees these defensively before writing the
     * fixed-size result. Keep this ownership path exercised in both direct
     * and child reads. */
    out->picture_data = malloc(3);
    out->picture_size = out->picture_data ? 3 : 0;
    out->lyrics = strdup("discarded scan lyrics");
}

void metadata_read_without_artwork(const char * path, track_metadata_t * out) {
    if (getpid() == test_parent_pid) direct_scan_calls++;
    fill_scan_metadata(path, out);
}

void metadata_read(const char * path, track_metadata_t * out) {
    memset(out, 0, sizeof(*out));
    if (strcmp(path, "art-crash") == 0) _exit(24);
    if (strcmp(path, "art-timeout") == 0) {
        struct timespec delay = {.tv_sec = 2, .tv_nsec = 0};
        nanosleep(&delay, NULL);
    }
    if (!helper_artwork_only) _exit(25);
    snprintf(out->title, sizeof(out->title), "helper-priority-%d", helper_priority);
    out->has_title = true;
    if (strcmp(path, "art-found") == 0) {
        static const unsigned char bytes[] = {0xff, 0xd8, 0xff, 0xd9};
        out->picture_data = malloc(sizeof(bytes));
        if (!out->picture_data) _exit(26);
        memcpy(out->picture_data, bytes, sizeof(bytes));
        out->picture_size = sizeof(bytes);
        out->lyrics = strdup("must not cross helper IPC");
        return;
    }
    if (strcmp(path, "art-too-large") == 0) {
        out->picture_too_large = true;
        return;
    }
    if (strcmp(path, "art-invalid-size") == 0) {
        out->picture_size = UINT32_MAX;
        return;
    }
}

size_t system_get_mem_available_bytes(void) {
    return (size_t) 1024 * 1024 * 1024;
}

size_t artwork_reserve_bytes_for_priority(artwork_priority_t prio) {
    (void) prio;
    return 1024U * 1024U;
}

bool artwork_check_memory_admission(artwork_priority_t prio, size_t estimated_bytes) {
    parent_admission_priority = (int) prio;
    return estimated_bytes <= 1024U * 1024U;
}

int subprocess_close_inherited_fds_action(posix_spawn_file_actions_t * actions) {
    (void) actions;
    return 0;
}

static void check_zero_metadata(const track_metadata_t * out) {
    assert(out->title[0] == '\0');
    assert(!out->has_title);
    assert(out->picture_data == NULL);
    assert(out->picture_size == 0);
    assert(out->lyrics == NULL);
}

static void test_scan_paths(void) {
    track_metadata_t out;
    memset(&out, 0xa5, sizeof(out));
    direct_scan_calls = 0;
    assert(metadata_read_isolated("direct.mp3", &out, 300));
    assert(strcmp(out.title, "scan-child-ok") == 0 && out.has_title);
    assert(out.picture_data == NULL && out.picture_size == 0 && out.lyrics == NULL);
    assert(direct_scan_calls == 1);

    memset(&out, 0, sizeof(out));
    assert(metadata_read_isolated("direct.AAC", &out, 300));
    assert(strcmp(out.title, "scan-child-ok") == 0);
    assert(direct_scan_calls == 2);

    memset(&out, 0, sizeof(out));
    assert(metadata_read_isolated("scan-child.flac", &out, 500));
    assert(strcmp(out.title, "scan-child-ok") == 0 && out.has_title);
    assert(out.picture_data == NULL && out.picture_size == 0 && out.lyrics == NULL);
    assert(direct_scan_calls == 2);

    memset(&out, 0xa5, sizeof(out));
    assert(!metadata_read_isolated("scan-crash.flac", &out, 500));
    check_zero_metadata(&out);

    memset(&out, 0xa5, sizeof(out));
    assert(!metadata_read_isolated("scan-timeout.flac", &out, 40));
    check_zero_metadata(&out);

    memset(&out, 0xa5, sizeof(out));
    assert(!metadata_read_isolated("", &out, 500));
    check_zero_metadata(&out);
}

static void test_artwork_helper_exec(void) {
    track_metadata_t out;
    memset(&out, 0, sizeof(out));
    parent_admission_priority = -1;
    assert(metadata_read_artwork_isolated("art-found", &out, 1000, ARTWORK_PRIO_PLAYER) ==
           METADATA_ARTWORK_FOUND);
    assert(out.has_title);
    char expected_title[64];
    snprintf(expected_title, sizeof(expected_title), "helper-priority-%d", ARTWORK_PRIO_PLAYER);
    assert(strcmp(out.title, expected_title) == 0);
    static const unsigned char expected[] = {0xff, 0xd8, 0xff, 0xd9};
    assert(out.picture_data && out.picture_size == sizeof(expected));
    assert(memcmp(out.picture_data, expected, sizeof(expected)) == 0);
    assert(out.lyrics == NULL);
    assert(parent_admission_priority == ARTWORK_PRIO_PLAYER);
    free(out.picture_data);

    memset(&out, 0, sizeof(out));
    metadata_artwork_result_t none_result = metadata_read_artwork_isolated(
        "art-none", &out, 1000, ARTWORK_PRIO_THUMBNAIL);
    if (none_result != METADATA_ARTWORK_NOT_FOUND)
        fprintf(stderr, "art-none result=%d title=%s has_title=%d size=%u too_large=%d\n",
                (int) none_result, out.title, out.has_title, out.picture_size, out.picture_too_large);
    assert(none_result == METADATA_ARTWORK_NOT_FOUND);
    assert(out.has_title && out.picture_data == NULL && out.picture_size == 0);

    memset(&out, 0, sizeof(out));
    assert(metadata_read_artwork_isolated("art-too-large", &out, 1000, ARTWORK_PRIO_WARMER) ==
           METADATA_ARTWORK_TOO_LARGE);
    assert(out.picture_too_large && out.picture_data == NULL);

    memset(&out, 0, sizeof(out));
    assert(metadata_read_artwork_isolated("art-invalid-size", &out, 1000, ARTWORK_PRIO_WARMER) ==
           METADATA_ARTWORK_INVALID);
    assert(out.picture_data == NULL);

    memset(&out, 0xa5, sizeof(out));
    assert(metadata_read_artwork_isolated("art-crash", &out, 1000, ARTWORK_PRIO_PLAYER) ==
           METADATA_ARTWORK_TEMPORARY_FAILURE);
    check_zero_metadata(&out);

    memset(&out, 0xa5, sizeof(out));
    assert(metadata_read_artwork_isolated("art-timeout", &out, 40, ARTWORK_PRIO_PLAYER) ==
           METADATA_ARTWORK_TEMPORARY_FAILURE);
    check_zero_metadata(&out);
}


static int64_t monotonic_ms(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Keep the exact bounded-reap/kill/pool cleanup coverage from the older
 * include-based metadata artwork test without exporting private pool state.
 * Including the implementation in this standalone TU gives this test access
 * only; production headers remain private. */
static void test_scan_child_reap(void) {
    int64_t start = monotonic_ms();
    for (int i = 0; i < 32; i++) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) { usleep(5000); _exit(0); }
        track_metadata_t result = {0};
        assert(reap_scan_parser_child(child, &result, true));
    }
    assert(monotonic_ms() - start < 1000);

    pid_t stuck = fork();
    assert(stuck >= 0);
    if (stuck == 0) { for (;;) pause(); }
    track_metadata_t result = {0};
    start = monotonic_ms();
    assert(!reap_scan_parser_child(stuck, &result, false));
    assert(monotonic_ms() - start < 1800);
    int status;
    assert(waitpid(stuck, &status, 0) == stuck);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    reap_abandoned_metadata_children(&scan_child_pool);
}

static void assert_no_unreaped_children(void) {
    int status = 0;
    errno = 0;
    pid_t result = waitpid(-1, &status, WNOHANG);
    assert(result == -1 && errno == ECHILD);
}

int main(int argc, char ** argv) {
    if (argc >= 2 && strcmp(argv[1], "--metadata-artwork-helper") == 0) {
        if (argc != 5) return 1;
        helper_priority = atoi(argv[4]);
        long fd = strtol(argv[2], NULL, 10);
        return metadata_artwork_helper_run(argv[3], (int) fd, (artwork_priority_t) helper_priority);
    }

    test_parent_pid = getpid();
    test_scan_child_reap();
    test_scan_paths();
    test_artwork_helper_exec();
    assert_no_unreaped_children();
    puts("metadata isolation lifecycle: PASS");
    return 0;
}
