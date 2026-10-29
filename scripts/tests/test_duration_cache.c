#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "src/library/metadata/durations/duration_cache.h"

static char file[PATH_MAX];

/* The calling convention of a page: take the epoch once, hand it back with the result. */
#define PUT(...) duration_cache_put(duration_cache_epoch(), __VA_ARGS__)
#define PUT_FAILED(...) duration_cache_put_failed(duration_cache_epoch(), __VA_ARGS__)
#define PRUNE(...) duration_cache_prune(duration_cache_epoch(), __VA_ARGS__)

static duration_info_t info(uint32_t ms) {
    return (duration_info_t) { .duration_ms = ms, .sample_rate = 44100, .bit_depth = 16, .codec = 3, .flags = 0 };
}

static void reload(void) {
    duration_cache_reset();
    duration_cache_wait_loaded();
}

static long file_size(void) {
    struct stat st;
    return stat(file, &st) == 0 ? (long) st.st_size : -1;
}

static void poke(long offset, unsigned char value) {
    int fd = open(file, O_WRONLY);
    assert(fd >= 0);
    assert(pwrite(fd, &value, 1, offset) == 1);
    close(fd);
}

static void test_put_get(void) {
    duration_info_t out;
    duration_cache_wait_loaded();
    assert(duration_cache_count() == 0);
    assert(!duration_cache_get("/m/a.flac", 100, 5000, &out));
    duration_info_t a = info(183500);
    a.sample_rate = 96000; a.bit_depth = 24; a.flags = DURATION_CACHE_FLAG_DSD;
    PUT("/m/a.flac", 100, 5000, &a);
    PUT("/m/b.flac", 100, 5000, &(duration_info_t) { 1000, 44100, 16, 1, 0 });
    assert(duration_cache_get("/m/a.flac", 100, 5000, &out));
    assert(out.duration_ms == 183500 && out.sample_rate == 96000 && out.bit_depth == 24 &&
           out.codec == 3 && out.flags == DURATION_CACHE_FLAG_DSD);
    /* Path, mtime and size are all part of the identity. */
    assert(!duration_cache_get("/m/a.flac", 101, 5000, &out));
    assert(!duration_cache_get("/m/a.flac", 100, 5001, &out));
    assert(!duration_cache_get("/m/c.flac", 100, 5000, &out));
    /* A zero duration is never stored. */
    PUT("/m/zero.flac", 1, 1, &(duration_info_t) { 0 });
    assert(!duration_cache_get("/m/zero.flac", 1, 1, &out));
    /* A newer value replaces the old one. */
    PUT("/m/b.flac", 100, 5000, &(duration_info_t) { 2000, 44100, 16, 1, 0 });
    assert(duration_cache_get("/m/b.flac", 100, 5000, &out) && out.duration_ms == 2000);
    assert(duration_cache_count() == 2);
}

static void test_persist_and_reload(void) {
    duration_info_t out;
    /* Writing is debounced: nothing on the card right after a put. */
    assert(file_size() < 0);
    assert(duration_cache_flush());
    assert(file_size() == 16 + 2 * 24);
    reload();
    assert(duration_cache_count() == 2);
    assert(duration_cache_get("/m/a.flac", 100, 5000, &out) && out.duration_ms == 183500 && out.sample_rate == 96000);
    assert(duration_cache_get("/m/b.flac", 100, 5000, &out) && out.duration_ms == 2000);
    /* Many entries survive the worker's merges (puts are bounded enqueues). */
    reload();
    for (int i = 0; i < 600; i++) {
        char p[64]; snprintf(p, sizeof p, "/m/t%d.flac", i);
        PUT(p, i, i * 3, &(duration_info_t) { (uint32_t) (i + 1) * 10, 44100, 16, 1, 0 });
        if (i % 64 == 63) duration_cache_wait_merged();
    }
    assert(duration_cache_flush());
    reload();
    assert(duration_cache_count() == 602);
    for (int i = 0; i < 600; i++) {
        char p[64]; snprintf(p, sizeof p, "/m/t%d.flac", i);
        assert(duration_cache_get(p, i, i * 3, &out) && out.duration_ms == (uint32_t) (i + 1) * 10);
    }
    /* No temp file is left behind. */
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", file);
    assert(access(tmp, F_OK) != 0);
}

static void msleep(int ms) { usleep((useconds_t) ms * 1000); }

static void test_debounced_write(void) {
    duration_cache_set_timing(400, 3000);
    PUT("/m/late.flac", 7, 7, &(duration_info_t) { 4242, 44100, 16, 1, 0 });
    long before = file_size();
    msleep(1200); /* the debounce is 400 ms here */
    assert(file_size() == before + 24);
    reload();
    duration_info_t out;
    assert(duration_cache_get("/m/late.flac", 7, 7, &out) && out.duration_ms == 4242);
}

/* The write waits for a quiet period after the LAST put (trailing edge)... */
static void test_trailing_edge(void) {
    duration_cache_set_timing(500, 5000);
    unlink(file);
    reload();
    for (int i = 0; i < 4; i++) {
        char p[32]; snprintf(p, sizeof p, "/m/trail%d", i);
        PUT(p, i, i, &(duration_info_t) { 1000u + (uint32_t) i, 44100, 16, 1, 0 });
        msleep(250); /* each put arrives before the debounce of the previous one ran out */
        assert(file_size() < 0);
    }
    msleep(900);
    assert(file_size() == 16 + 4 * 24);
}

/* ...but never later than the maximum dirty age, however steadily puts arrive. */
static void test_max_dirty_age(void) {
    duration_cache_set_timing(600, 1500);
    unlink(file);
    reload();
    bool written = false;
    for (int i = 0; i < 14 && !written; i++) {
        char p[32]; snprintf(p, sizeof p, "/m/age%d", i);
        PUT(p, i, i, &(duration_info_t) { 1000u + (uint32_t) i, 44100, 16, 1, 0 });
        msleep(200);
        written = file_size() > 0;
    }
    assert(written); /* the debounce alone would have kept deferring it */
    duration_cache_set_timing(400, 3000);
}

/* Records of a replaced card are dropped, never written to the new one, and
 * the new card's file is loaded lazily. */
static void test_storage_epoch(void) {
    char card_a[PATH_MAX + 8], card_b[PATH_MAX + 8];
    snprintf(card_a, sizeof card_a, "%s.cardA", file);
    snprintf(card_b, sizeof card_b, "%s.cardB", file);
    unlink(card_a); unlink(card_b);
    duration_info_t out;
    duration_cache_set_path(card_b);
    reload();
    PUT("/m/b-only.flac", 2, 2, &(duration_info_t) { 2222, 44100, 16, 1, 0 });
    assert(duration_cache_flush());
    duration_cache_set_path(card_a);
    reload();
    PUT("/m/a-only.flac", 1, 1, &(duration_info_t) { 1111, 44100, 16, 1, 0 });
    assert(duration_cache_get("/m/a-only.flac", 1, 1, &out));
    /* The card is swapped before the debounce ran out. */
    duration_cache_new_storage_epoch();
    duration_cache_set_path(card_b);
    assert(duration_cache_count() == 0);
    assert(!duration_cache_get("/m/a-only.flac", 1, 1, &out)); /* RAM was dropped (and a load started) */
    msleep(1200);
    struct stat st;
    assert(stat(card_a, &st) != 0); /* the old card's unwritten record never reached disk */
    duration_cache_wait_loaded();
    assert(duration_cache_count() == 1);
    assert(duration_cache_get("/m/b-only.flac", 2, 2, &out) && out.duration_ms == 2222);
    assert(!duration_cache_get("/m/a-only.flac", 1, 1, &out));
    /* A flush after the swap writes only the new card's records. */
    PUT("/m/b2.flac", 3, 3, &(duration_info_t) { 3333, 44100, 16, 1, 0 });
    assert(duration_cache_flush());
    duration_cache_set_path(file);
    unlink(card_a); unlink(card_b);
    reload();
}

/* Puts that arrive while the file is being written are neither lost nor torn. */
static void * putter(void * arg) {
    (void) arg;
    for (int i = 0; i < 3000; i++) {
        char p[32]; snprintf(p, sizeof p, "/m/race%d", i);
        PUT(p, i, i, &(duration_info_t) { 500u + (uint32_t) i, 44100, 16, 1, 0 });
        if (i % 100 == 99) usleep(2000);
    }
    return NULL;
}

static void test_snapshot_race(void) {
    unlink(file);
    reload();
    duration_cache_set_timing(20, 200);
    pthread_t t;
    assert(pthread_create(&t, NULL, putter, NULL) == 0);
    for (int i = 0; i < 20; i++) { (void) duration_cache_flush_bounded(500); msleep(10); }
    pthread_join(t, NULL);
    duration_cache_wait_merged();
    assert(duration_cache_flush());
    assert(duration_cache_count() == 3000 || duration_cache_count() > 2500); /* a full queue may drop a put */
    size_t expected = duration_cache_count();
    reload();
    assert(duration_cache_count() == expected);
    duration_cache_set_timing(400, 3000);
}

static void test_prune(void) {
    reload();
    size_t before = duration_cache_count();
    assert(before == 603);
    uint64_t keep[3];
    keep[0] = duration_cache_key("/m/a.flac", 100, 5000);
    keep[1] = duration_cache_key("/m/t5.flac", 5, 15);
    keep[2] = duration_cache_key("/m/gone.flac", 1, 1); /* not in the cache: ignored */
    /* valid keys must be sorted */
    for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) if (keep[j] < keep[i]) { uint64_t t = keep[i]; keep[i] = keep[j]; keep[j] = t; }
    assert(PRUNE(keep, 3) == before - 2);
    duration_info_t out;
    assert(duration_cache_count() == 2);
    assert(duration_cache_get("/m/a.flac", 100, 5000, &out));
    assert(duration_cache_get("/m/t5.flac", 5, 15, &out));
    assert(!duration_cache_get("/m/b.flac", 100, 5000, &out));
    /* A changed file (same path, new mtime) no longer matches either. */
    assert(PRUNE(&keep[0], 0) == 2);
    assert(duration_cache_count() == 0);
    assert(duration_cache_flush());
    assert(file_size() == 16);
    reload();
    assert(duration_cache_count() == 0);
}

static bool card_present = true;
static bool card_mounted(void) { return card_present; }

/* A put measured before a card change is dropped, and so is a prune. */
static void test_epoch_tagged_puts(void) {
    duration_info_t out;
    unlink(file);
    reload();
    uint32_t old_epoch = duration_cache_epoch();
    PUT("/m/keep.flac", 1, 1, &(duration_info_t) { 1111, 44100, 16, 1, 0 });
    assert(duration_cache_count() == 1);
    duration_cache_new_storage_epoch();
    assert(duration_cache_epoch() != old_epoch);
    duration_cache_put(old_epoch, "/m/stale.flac", 2, 2, &(duration_info_t) { 2222, 44100, 16, 1, 0 });
    duration_cache_put_failed(old_epoch, "/m/stale-failed.flac", 3, 3);
    duration_cache_wait_loaded();
    assert(duration_cache_count() == 0);
    assert(!duration_cache_get("/m/stale.flac", 2, 2, &out));
    assert(!duration_cache_get("/m/stale-failed.flac", 3, 3, &out));
    /* The current epoch is accepted. */
    PUT("/m/fresh.flac", 4, 4, &(duration_info_t) { 4444, 44100, 16, 1, 0 });
    assert(duration_cache_get("/m/fresh.flac", 4, 4, &out) && out.duration_ms == 4444);
    /* A prune planned on the old epoch's data does nothing. */
    uint64_t none[1] = { 1 };
    assert(duration_cache_prune(old_epoch, none, 1) == 0);
    assert(duration_cache_count() == 1);
    assert(duration_cache_flush());
    assert(file_size() == 16 + 24);
    reload();
}

/* No card: nothing loads, nothing is written, and the cache never turns into a
 * loaded-but-empty table that could replace the real file. */
static void test_no_card(void) {
    duration_info_t out;
    unlink(file);
    reload();
    PUT("/m/a.flac", 1, 1, &(duration_info_t) { 1000, 44100, 16, 1, 0 });
    PUT("/m/b.flac", 2, 2, &(duration_info_t) { 2000, 44100, 16, 1, 0 });
    assert(duration_cache_flush());
    long size = file_size();
    assert(size == 16 + 2 * 24);
    card_present = false;
    duration_cache_set_mount_check(card_mounted);
    duration_cache_new_storage_epoch();
    assert(!duration_cache_wait_loaded());
    assert(!duration_cache_get("/m/a.flac", 1, 1, &out));
    assert(!duration_cache_get_blocking(duration_cache_key("/m/a.flac", 1, 1), &out));
    PUT("/m/c.flac", 3, 3, &(duration_info_t) { 3000, 44100, 16, 1, 0 }); /* nowhere to keep it */
    assert(duration_cache_count() == 0);
    assert(duration_cache_flush_bounded(300));
    assert(file_size() == size); /* the real file is untouched */
    /* The card comes back (the mount hook bumps the epoch): the real file loads. */
    card_present = true;
    duration_cache_new_storage_epoch();
    assert(duration_cache_wait_loaded());
    assert(duration_cache_count() == 2);
    assert(duration_cache_get("/m/b.flac", 2, 2, &out) && out.duration_ms == 2000);
    duration_cache_set_mount_check(NULL);
}

/* A file that cannot be read is not an empty one: the cache serves RAM only and
 * never overwrites it. */
static void test_read_only_after_failed_load(void) {
    duration_info_t out;
    unlink(file);
    assert(mkdir(file, 0755) == 0); /* opens, but cannot be read as a file */
    reload();
    assert(duration_cache_read_only());
    PUT("/m/ram.flac", 1, 1, &(duration_info_t) { 5000, 44100, 16, 1, 0 });
    assert(duration_cache_get("/m/ram.flac", 1, 1, &out) && out.duration_ms == 5000);
    assert(duration_cache_flush_bounded(300)); /* nothing is dirty, so nothing to write */
    struct stat st;
    assert(stat(file, &st) == 0 && S_ISDIR(st.st_mode));
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", file);
    assert(access(tmp, F_OK) != 0);
    /* A new epoch with a readable (missing) file is writable again. */
    assert(rmdir(file) == 0);
    reload();
    assert(!duration_cache_read_only());
    PUT("/m/real.flac", 2, 2, &(duration_info_t) { 6000, 44100, 16, 1, 0 });
    assert(duration_cache_flush());
    assert(file_size() == 16 + 24);
}

/* The directory is opened when the file is loaded; a write goes through that
 * handle, so it cannot land in whatever the path names later. */
static void test_dirfd_write(void) {
    char dir_a[PATH_MAX + 16], moved[PATH_MAX + 32], path[PATH_MAX + 64], other[PATH_MAX + 96];
    snprintf(dir_a, sizeof dir_a, "%s.card", file);
    snprintf(moved, sizeof moved, "%s.old", dir_a);
    snprintf(path, sizeof path, "%s/durations.bin", dir_a);
    snprintf(other, sizeof other, "%s/durations.bin", moved);
    mkdir(dir_a, 0755);
    duration_cache_set_path(path);
    reload();
    PUT("/m/loaded.flac", 1, 1, &(duration_info_t) { 7000, 44100, 16, 1, 0 });
    /* The "card" is swapped under the path before the write happens. */
    assert(rename(dir_a, moved) == 0);
    assert(mkdir(dir_a, 0755) == 0);
    assert(duration_cache_flush());
    struct stat st;
    assert(stat(other, &st) == 0 && st.st_size == 16 + 24); /* landed in the directory it was loaded from */
    assert(stat(path, &st) != 0);                           /* and not in the new one */
    unlink(other); rmdir(moved); rmdir(dir_a);
    duration_cache_set_path(file);
    reload();
}

static void rebuild(int n) {
    duration_cache_reset();
    unlink(file);
    duration_cache_wait_loaded();
    for (int i = 0; i < n; i++) {
        char p[64]; snprintf(p, sizeof p, "/m/c%d.flac", i);
        PUT(p, i, i, &(duration_info_t) { 1000u + (uint32_t) i, 44100, 16, 1, 0 });
    }
    assert(duration_cache_flush());
    assert(file_size() == 16 + n * 24L);
}

static void test_corrupt(void) {
    duration_info_t out;
    rebuild(10);
    /* One damaged record is dropped, the rest stay. */
    poke(16 + 24 * 3 + 9, 0x5a);
    reload();
    assert(duration_cache_count() == 9);
    /* A truncated file is discarded as a whole. */
    rebuild(10);
    assert(truncate(file, file_size() - 7) == 0);
    reload();
    assert(duration_cache_count() == 0);
    /* Bad magic, bad version, bad header check. */
    rebuild(10); poke(0, 'X'); reload(); assert(duration_cache_count() == 0);
    rebuild(10); poke(4, 9); reload(); assert(duration_cache_count() == 0);
    rebuild(10); poke(12, 0); poke(13, 0); poke(14, 0); poke(15, 0); reload(); assert(duration_cache_count() == 0);
    /* An absurd entry count. */
    rebuild(10); poke(11, 0x7f); reload(); assert(duration_cache_count() == 0);
    /* Garbage and an empty file. */
    int fd = open(file, O_WRONLY | O_TRUNC); assert(fd >= 0);
    assert(write(fd, "not a cache file at all, just text", 34) == 34); close(fd);
    reload(); assert(duration_cache_count() == 0);
    fd = open(file, O_WRONLY | O_TRUNC); assert(fd >= 0); close(fd);
    reload(); assert(duration_cache_count() == 0);
    /* After a bad file the cache works and the next write replaces it. */
    PUT("/m/new.flac", 1, 2, &(duration_info_t) { 3000, 44100, 16, 1, 0 });
    assert(duration_cache_get("/m/new.flac", 1, 2, &out));
    assert(duration_cache_flush());
    reload();
    assert(duration_cache_count() == 1 && duration_cache_get("/m/new.flac", 1, 2, &out) && out.duration_ms == 3000);
}

static void test_bound(void) {
    duration_cache_reset();
    unlink(file);
    duration_cache_wait_loaded();
    for (uint32_t i = 0; i < DURATION_CACHE_MAX_ENTRIES + 50; i++) {
        char p[48]; snprintf(p, sizeof p, "/b/%u", i);
        PUT(p, (int64_t) i, 1, &(duration_info_t) { i + 1, 44100, 16, 1, 0 });
        if (i % 128 == 127) duration_cache_wait_merged();
    }
    duration_cache_wait_merged();
    assert(duration_cache_count() == DURATION_CACHE_MAX_ENTRIES);
    assert(duration_cache_flush());
    assert(file_size() == 16 + (long) DURATION_CACHE_MAX_ENTRIES * 24);
    reload();
    assert(duration_cache_count() == DURATION_CACHE_MAX_ENTRIES);
    unlink(file);
}

static void test_unwritable(void) {
    /* A read-only or full card only loses persistence: RAM keeps working. */
    duration_cache_reset();
    duration_cache_set_path("/proc/no-such-dir/durations.bin");
    duration_cache_wait_loaded();
    PUT("/m/ram.flac", 1, 1, &(duration_info_t) { 5000, 44100, 16, 1, 0 });
    assert(!duration_cache_flush());
    duration_info_t out;
    assert(duration_cache_get("/m/ram.flac", 1, 1, &out) && out.duration_ms == 5000);
}

static void test_failed(void) {
    unlink(file);
    reload();
    duration_info_t out;
    PUT_FAILED("/m/frag.m4a", 10, 999);
    assert(duration_cache_get("/m/frag.m4a", 10, 999, &out));
    assert(duration_info_is_failed(&out) && out.duration_ms == 0 && out.bit_depth == DURATION_PROBE_REVISION);
    PUT("/m/ok.flac", 1, 2, &(duration_info_t) { 3000, 44100, 16, 1, 0 });
    assert(!duration_info_is_failed(&(duration_info_t) { 3000, 44100, 16, 1, 0 }));
    /* A failed record never carries a duration. */
    PUT("/m/bad.m4a", 1, 1, &(duration_info_t) { 5, 0, DURATION_PROBE_REVISION, 0, DURATION_CACHE_FLAG_FAILED });
    assert(!duration_cache_get("/m/bad.m4a", 1, 1, &out));
    /* Round trip through the file. */
    assert(duration_cache_flush());
    reload();
    assert(duration_cache_count() == 2);
    assert(duration_cache_get("/m/frag.m4a", 10, 999, &out));
    assert(duration_info_is_failed(&out) && out.duration_ms == 0);
    assert(duration_cache_get("/m/ok.flac", 1, 2, &out) && out.duration_ms == 3000 && !duration_info_is_failed(&out));
    /* Another probe revision is a miss, and probing again replaces the record. */
    PUT("/m/old.m4a", 4, 4, &(duration_info_t) { 0, 0, DURATION_PROBE_REVISION + 1, 0, DURATION_CACHE_FLAG_FAILED });
    assert(!duration_cache_get("/m/old.m4a", 4, 4, &out));
    assert(duration_cache_flush());
    reload();
    assert(!duration_cache_get("/m/old.m4a", 4, 4, &out));
    PUT("/m/old.m4a", 4, 4, &(duration_info_t) { 2500, 44100, 16, 1, 0 });
    assert(duration_cache_get("/m/old.m4a", 4, 4, &out) && out.duration_ms == 2500);
    PUT_FAILED("/m/old.m4a", 4, 4);
    assert(duration_cache_get("/m/old.m4a", 4, 4, &out) && duration_info_is_failed(&out));
    /* Pruning treats failed records like any other. */
    uint64_t keep[1] = { duration_cache_key("/m/frag.m4a", 10, 999) };
    assert(PRUNE(keep, 1) == 2);
    assert(duration_cache_get("/m/frag.m4a", 10, 999, &out) && duration_info_is_failed(&out));
    assert(!duration_cache_get("/m/old.m4a", 4, 4, &out));
    assert(duration_cache_flush());
    reload();
    assert(duration_cache_count() == 1);
    assert(duration_cache_get("/m/frag.m4a", 10, 999, &out) && duration_info_is_failed(&out));
    assert(PRUNE(keep, 0) == 1);
}

int main(void) {
    char dir[] = "/tmp/compas-durations-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(file, sizeof file, "%s/durations.bin", dir);
    duration_cache_set_path(file);
    test_put_get();
    test_persist_and_reload();
    test_debounced_write();
    test_prune();
    test_corrupt();
    test_failed();
    test_bound();
    test_unwritable();
    duration_cache_set_path(file);
    test_trailing_edge();
    test_max_dirty_age();
    test_storage_epoch();
    test_epoch_tagged_puts();
    test_no_card();
    test_read_only_after_failed_load();
    test_dirfd_write();
    test_snapshot_race();
    unlink(file);
    rmdir(dir);
    puts("duration cache tests passed");
    return 0;
}
