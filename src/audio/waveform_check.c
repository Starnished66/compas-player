#define _POSIX_C_SOURCE 200809L

#include "waveform.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static atomic_uint decoder_calls;
static atomic_uint decoder_cancellations;

bool audio_extract_waveform(const char * path, uint8_t * bins, size_t count,
                            bool (*cancel)(void *), void * user) {
    atomic_fetch_add(&decoder_calls, 1);
    FILE * file = fopen(path, "rb");
    if (!file) return false;
    int value = fgetc(file);
    fclose(file);
    if (value == EOF) return false;

    if (strstr(path, "slow")) {
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 2000000L };
        for (unsigned i = 0; i < 1000; i++) {
            if (cancel(user)) {
                atomic_fetch_add(&decoder_cancellations, 1);
                return false;
            }
            nanosleep(&delay, NULL);
        }
    }
    if (cancel(user)) {
        atomic_fetch_add(&decoder_cancellations, 1);
        return false;
    }
    memset(bins, value, count);
    return true;
}

static void sleep_ms(unsigned milliseconds) {
    struct timespec delay = {
        .tv_sec = (time_t) (milliseconds / 1000U),
        .tv_nsec = (long) (milliseconds % 1000U) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

static void write_file(const char * path, const char * data) {
    FILE * file = fopen(path, "wb");
    assert(file);
    size_t len = strlen(data);
    assert(fwrite(data, 1, len, file) == len);
    assert(fclose(file) == 0);
}

static waveform_data_t wait_ready(const char * path) {
    for (unsigned i = 0; i < 500; i++) {
        waveform_data_t data;
        if (waveform_copy(path, &data)) return data;
        sleep_ms(10);
    }
    fprintf(stderr, "waveform-check: timed out waiting for %s\n", path);
    abort();
}

static bool bytes_contain(const char * haystack, size_t haystack_len,
                          const char * needle, size_t needle_len) {
    if (!needle_len || needle_len > haystack_len) return false;
    for (size_t i = 0; i <= haystack_len - needle_len; i++)
        if (memcmp(haystack + i, needle, needle_len) == 0) return true;
    return false;
}

static bool corrupt_cache_record_for(const char * path) {
    DIR * dir = opendir(".compas/waveforms");
    if (!dir) return false;
    struct dirent * entry;
    bool found = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "wf_", 3) != 0) continue;
        char full[512];
        snprintf(full, sizeof(full), ".compas/waveforms/%s", entry->d_name);
        FILE * file = fopen(full, "rb");
        if (!file) continue;
        char content[4096];
        size_t len = fread(content, 1, sizeof(content), file);
        fclose(file);
        if (!bytes_contain(content, len, path, strlen(path))) continue;
        file = fopen(full, "wb");
        if (!file) continue;
        assert(fwrite("WFM1broken", 1, 10, file) == 10);
        assert(fclose(file) == 0);
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

static size_t cache_record_count(void) {
    DIR * dir = opendir(".compas/waveforms");
    if (!dir) return 0;
    size_t count = 0;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (strncmp(entry->d_name, "wf_", 3) == 0 && len >= 7 &&
            strcmp(entry->d_name + len - 4, ".bin") == 0) count++;
    }
    closedir(dir);
    return count;
}

static int remove_tree(const char * path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR * dir = opendir(path);
    if (!dir) return -1;
    struct dirent * entry;
    int result = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[512];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n <= 0 || (size_t) n >= sizeof(child) || remove_tree(child) != 0) result = -1;
    }
    closedir(dir);
    if (rmdir(path) != 0) result = -1;
    return result;
}

int main(void) {
    char temp[] = "/tmp/waveform-check-XXXXXX";
    assert(mkdtemp(temp));
    char original_dir[512];
    assert(getcwd(original_dir, sizeof(original_dir)));
    assert(chdir(temp) == 0);
    write_file("slow.raw", "SLOW");
    write_file("fast.raw", "F");
    write_file("bad.raw", "B");

    waveform_request("https://example.invalid/stream");
    assert(atomic_load(&decoder_calls) == 0);
    waveform_data_t data;
    assert(!waveform_copy("https://example.invalid/stream", &data));

    waveform_request("slow.raw");
    for (unsigned i = 0; i < 200 && atomic_load(&decoder_calls) == 0; i++) sleep_ms(2);
    assert(atomic_load(&decoder_calls) == 1);
    waveform_request("fast.raw");
    data = wait_ready("fast.raw");
    assert(data.ready && data.bins[0] == 'F' && data.bins[WAVEFORM_BINS - 1] == 'F');
    for (unsigned i = 0; i < 200 && atomic_load(&decoder_cancellations) == 0; i++) sleep_ms(2);
    assert(atomic_load(&decoder_cancellations) == 1);
    assert(!waveform_copy("slow.raw", &data));
    unsigned calls = atomic_load(&decoder_calls);

    waveform_request("fast.raw");
    sleep_ms(30);
    assert(atomic_load(&decoder_calls) == calls); /* identical request is coalesced */

    waveform_request("https://example.invalid/stream");
    assert(!waveform_copy("fast.raw", &data)); /* non-local playback clears old local result */
    waveform_request("https://example.invalid/stream"); /* repeated invalid input is harmless */
    assert(atomic_load(&decoder_calls) == calls);
    waveform_request("fast.raw");
    data = wait_ready("fast.raw");
    assert(data.bins[0] == 'F' && atomic_load(&decoder_calls) == calls); /* cache hit */

    waveform_cancel();
    assert(!waveform_copy("fast.raw", &data));
    waveform_request("fast.raw");
    data = wait_ready("fast.raw");
    assert(data.bins[0] == 'F');
    assert(atomic_load(&decoder_calls) == calls); /* cache hit */

    write_file("fast.raw", "changed"); /* size change invalidates cache key */
    waveform_cancel();
    waveform_request("fast.raw");
    data = wait_ready("fast.raw");
    assert(data.bins[0] == 'c');
    calls = atomic_load(&decoder_calls);
    assert(calls == 3);

    waveform_request("bad.raw");
    data = wait_ready("bad.raw");
    assert(data.bins[0] == 'B');
    calls = atomic_load(&decoder_calls);
    assert(calls == 4 && corrupt_cache_record_for("bad.raw"));
    waveform_cancel();
    waveform_request("bad.raw");
    data = wait_ready("bad.raw");
    assert(data.bins[0] == 'B');
    assert(atomic_load(&decoder_calls) == calls + 1); /* malformed record decoded again */

    for (unsigned i = 0; i < 65; i++) {
        char path[32], content[2] = { (char) ('a' + (i % 26U)), '\0' };
        snprintf(path, sizeof(path), "entry%02u.raw", i);
        write_file(path, content);
        waveform_request(path);
        assert(wait_ready(path).ready);
    }
    assert(cache_record_count() <= 64); /* cache eviction remains bounded */

    waveform_shutdown();
    assert(!waveform_copy("bad.raw", &data));
    assert(chdir(original_dir) == 0);
    assert(remove_tree(temp) == 0);
    puts("waveform-selftest: PASS");
    return 0;
}
