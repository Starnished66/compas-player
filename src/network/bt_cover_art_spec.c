#define _GNU_SOURCE
#include "bt_cover_art.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t hook_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hook_cond = PTHREAD_COND_INITIALIZER;
static int block_stage;
static bool stage_entered;
static bool release_stage;
static bool fail_fsync;
static unsigned completed;

void bt_cover_art_test_hook(int stage) {
    pthread_mutex_lock(&hook_mutex);
    if (stage == 3) {
        completed++;
        pthread_cond_broadcast(&hook_cond);
    }
    if (stage == block_stage) {
        stage_entered = true;
        pthread_cond_broadcast(&hook_cond);
        while (!release_stage) pthread_cond_wait(&hook_cond, &hook_mutex);
        block_stage = 0;
    }
    pthread_mutex_unlock(&hook_mutex);
}

bool bt_cover_art_test_fail_fsync(void) {
    pthread_mutex_lock(&hook_mutex);
    bool fail = fail_fsync;
    fail_fsync = false;
    pthread_mutex_unlock(&hook_mutex);
    return fail;
}

static void arm_hook(int stage, bool fail) {
    pthread_mutex_lock(&hook_mutex);
    block_stage = stage;
    stage_entered = false;
    release_stage = false;
    fail_fsync = fail;
    pthread_mutex_unlock(&hook_mutex);
}

static void wait_entered(void) {
    pthread_mutex_lock(&hook_mutex);
    while (!stage_entered) pthread_cond_wait(&hook_cond, &hook_mutex);
    pthread_mutex_unlock(&hook_mutex);
}

static void release_hook(void) {
    pthread_mutex_lock(&hook_mutex);
    release_stage = true;
    pthread_cond_broadcast(&hook_cond);
    pthread_mutex_unlock(&hook_mutex);
}

static void wait_completed(unsigned target) {
    pthread_mutex_lock(&hook_mutex);
    while (completed < target) pthread_cond_wait(&hook_cond, &hook_mutex);
    pthread_mutex_unlock(&hook_mutex);
}

static unsigned completed_count(void) {
    pthread_mutex_lock(&hook_mutex);
    unsigned count = completed;
    pthread_mutex_unlock(&hook_mutex);
    return count;
}

static bool wait_reuse(uint64_t token, uint64_t key) {
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    for (unsigned i = 0; i < 500; i++) {
        if (bt_cover_art_reuse(token, key)) return true;
        nanosleep(&delay, NULL);
    }
    return false;
}

static bool has_url(void) {
    char url[128];
    return bt_cover_art_get_url(url, sizeof(url));
}

int main(void) {
    uint16_t * pixels = calloc(200U * 200U, sizeof(*pixels));
    unsigned target;
    assert(pixels);
    uint64_t token = bt_cover_art_begin_track("async-test-a");

    /* The caller returns while the worker is deliberately held in encoding. */
    arm_hook(1, false);
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 1001));
    wait_entered();

    /* A newer queued request replaces the one pending behind active work. */
    memset(pixels, 0xf800, 200U * 200U * sizeof(*pixels));
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 1002));
    memset(pixels, 0x07e0, 200U * 200U * sizeof(*pixels));
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 1003));
    release_hook();
    assert(wait_reuse(token, 1003));
    assert(!bt_cover_art_reuse(token, 1002));

    /* An async cache hit supersedes same-token active and pending requests. */
    assert(bt_cover_art_reuse(token, 1003));
    char cached_url[128], final_url[128];
    assert(bt_cover_art_get_url(cached_url, sizeof(cached_url)));
    token = bt_cover_art_begin_track("async-test-cache-latest");
    arm_hook(1, false);
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 5001));
    wait_entered();
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 5002));
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 1003));
    target = completed_count() + 1;
    release_hook();
    wait_completed(target);
    assert(bt_cover_art_get_url(final_url, sizeof(final_url)));
    assert(strcmp(cached_url, final_url) == 0);
    assert(!bt_cover_art_reuse(token, 5001));
    assert(!bt_cover_art_reuse(token, 5002));

    /* The public reuse entry point also supersedes active and pending work. */
    token = bt_cover_art_begin_track("async-test-public-reuse");
    arm_hook(1, false);
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 5101));
    wait_entered();
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 5102));
    assert(bt_cover_art_reuse(token, 1003));
    target = completed_count() + 1;
    release_hook();
    wait_completed(target);
    assert(bt_cover_art_get_url(final_url, sizeof(final_url)));
    assert(strcmp(cached_url, final_url) == 0);
    assert(!bt_cover_art_reuse(token, 5101));
    assert(!bt_cover_art_reuse(token, 5102));

    /* A later synchronous publication likewise invalidates both async slots. */
    token = bt_cover_art_begin_track("async-test-sync-latest");
    arm_hook(1, false);
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 5201));
    wait_entered();
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 5202));
    target = completed_count() + 1;
    release_hook();
    assert(bt_cover_art_publish_rgb565(token, pixels, 200, 200, 5203));
    assert(bt_cover_art_get_url(final_url, sizeof(final_url)));
    wait_completed(target);
    char after_worker_url[128];
    assert(bt_cover_art_get_url(after_worker_url, sizeof(after_worker_url)));
    assert(strcmp(final_url, after_worker_url) == 0);
    assert(bt_cover_art_reuse(token, 5203));
    assert(!bt_cover_art_reuse(token, 5201));
    assert(!bt_cover_art_reuse(token, 5202));

    /* Clear invalidates an already-running asynchronous publication. */
    token = bt_cover_art_begin_track("async-test-clear");
    arm_hook(1, false);
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 2001));
    wait_entered();
    bt_cover_art_clear(token);
    target = completed_count() + 1;
    release_hook();
    wait_completed(target);
    assert(!has_url());
    assert(!bt_cover_art_reuse(token, 2001));

    /* A changed track cancels a queued request while active encoding is held. */
    token = bt_cover_art_begin_track("async-test-old");
    arm_hook(1, false);
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 3001));
    wait_entered();
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 3002));
    uint64_t current = bt_cover_art_begin_track("async-test-new");
    target = completed_count() + 1;
    release_hook();
    wait_completed(target);
    assert(!bt_cover_art_reuse(current, 3002));
    assert(!has_url());

    /* A failed fsync must leave no published URL or reusable cache entry. */
    token = bt_cover_art_begin_track("async-test-fsync-fail");
    arm_hook(2, true);
    assert(bt_cover_art_publish_rgb565_async(token, pixels, 200, 200, 4001));
    wait_entered();
    target = completed_count() + 1;
    release_hook();
    wait_completed(target);
    assert(!has_url());
    assert(!bt_cover_art_reuse(token, 4001));

    free(pixels);
    puts("bt_cover_art_spec: ok");
    return 0;
}
