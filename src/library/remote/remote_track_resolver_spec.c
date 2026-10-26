#include "remote_track.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static remote_track_meta_t entry(void) {
    remote_track_meta_t m = {0};
    strcpy(m.provider, "test"); strcpy(m.track_id, "1");
    strcpy(m.title, "queue title"); m.resolver_owner = 42; m.resolve_stream = true;
    return m;
}
typedef struct { uint64_t generation; bool result; remote_track_meta_t out; } wait_case_t;
static int notify_count;
static void count_notify(void) { notify_count++; }
static void *waiter(void *p) {
    wait_case_t *w = p;
    w->result = remote_track_stream_wait("remote://test/1", w->generation, &w->out);
    return NULL;
}
int main(void) {
    remote_track_meta_t m = entry(), out = {0};
    remote_stream_request_t req;
    remote_track_test_set_clock(1000);
    strcpy(m.stream_url, "https://upfront/link");
    assert(remote_track_meta_set_all(&m, 1));
    assert(remote_track_stream_is_fresh("remote://test/1"));
    remote_track_test_set_clock(59000);
    assert(!remote_track_stream_is_fresh("remote://test/1"));
    m.stream_url[0] = 0;
    assert(remote_track_meta_set_all(&m, 1));
    remote_track_stream_begin_playback("remote://test/1", 7);
    assert(!remote_track_stream_is_fresh("remote://test/1"));
    assert(remote_track_stream_prefetch("remote://test/1", 7));
    assert(remote_track_stream_prefetch("remote://test/1", 7)); /* deduplicated */
    assert(remote_track_stream_take_request(&req));
    assert(req.owner == 42 && !strcmp(req.provider, "test"));
    assert(!remote_track_stream_take_request(&req));
    assert(!remote_track_stream_provide(9, req.request_id, &m, NULL));
    wait_case_t success = {.generation = 7};
    pthread_t thread; assert(pthread_create(&thread, NULL, waiter, &success) == 0);
    assert(remote_track_test_wait_for_waiters(1));
    remote_track_meta_t result = m; strcpy(result.stream_url, "https://stream/one");
    strcpy(result.codec, "flac"); result.url_ttl_s = 60;
    assert(remote_track_stream_provide(42, req.request_id, &result, NULL));
    assert(pthread_join(thread, NULL) == 0 && success.result);
    out = success.out;
    assert(remote_track_stream_wait("remote://test/1", 7, &out));
    assert(!strcmp(out.title, "queue title") && !strcmp(out.stream_url, result.stream_url));
    assert(remote_track_stream_is_fresh("remote://test/1"));

    /* A short TTL keeps a proportionate guard rather than expiring at once. */
    m = entry(); strcpy(m.stream_url, "short://url"); m.url_ttl_s = 1;
    remote_track_test_set_clock(60000);
    assert(remote_track_meta_set_all(&m, 1));
    assert(remote_track_stream_is_fresh("remote://test/1"));
    remote_track_test_set_clock(60760);
    assert(!remote_track_stream_is_fresh("remote://test/1"));

    /* Replacing queue invalidates in-flight IDs, but legacy links ignore age. */
    m.resolve_stream = false; strcpy(m.stream_url, "legacy://usable");
    assert(remote_track_meta_set_all(&m, 1));
    assert(remote_track_stream_is_fresh("remote://test/1"));
    remote_track_stream_begin_playback("remote://test/1", 8);
    m.resolve_stream = true; m.stream_url[0] = 0;
    assert(remote_track_meta_set_all(&m, 1));
    remote_track_stream_begin_playback("remote://test/1", 9);
    assert(remote_track_stream_prefetch("remote://test/1", 9));
    assert(remote_track_stream_take_request(&req));
    remote_track_stream_cancel_all();
    assert(!remote_track_stream_provide(42, req.request_id, &result, NULL));
    assert(!remote_track_stream_wait("remote://test/1", 9, &out));

    /* owner cancellation wakes a waiter and prevents re-enqueue. */
    m = entry(); assert(remote_track_meta_set_all(&m, 1));
    remote_track_stream_begin_playback("remote://test/1", 11);
    wait_case_t canceled = {.generation = 11};
    assert(pthread_create(&thread, NULL, waiter, &canceled) == 0);
    assert(remote_track_test_wait_for_waiters(1));
    assert(remote_track_test_pending_count() == 1);
    assert(remote_track_stream_take_request(&req));
    remote_track_stream_cancel_owner(42);
    assert(pthread_join(thread, NULL) == 0 && !canceled.result);
    assert(remote_track_test_pending_count() == 0);
    assert(!remote_track_stream_prefetch("remote://test/1", 11));

    /* Deadline failure is visible and rejects a late provider response. */
    m = entry(); assert(remote_track_meta_set_all(&m, 1));
    remote_track_test_use_real_clock();
    remote_track_stream_begin_playback("remote://test/1", 12);
    remote_track_test_set_wait_timeout_ms(5);
    assert(remote_track_stream_prefetch("remote://test/1", 12));
    assert(remote_track_stream_take_request(&req));
    wait_case_t timedout = {.generation = 12};
    assert(pthread_create(&thread, NULL, waiter, &timedout) == 0);
    assert(pthread_join(thread, NULL) == 0 && !timedout.result);
    assert(!remote_track_stream_provide(42, req.request_id, &result, NULL));
    char err[256]; assert(remote_track_stream_error_for_path("remote://test/1", 12, err, sizeof(err)));
    assert(strstr(err, "did not answer"));
    remote_track_test_set_wait_timeout_ms(15000);

    /* Successful table replacement cancels playback waiters. */
    remote_track_stream_begin_playback("remote://test/1", 13);
    wait_case_t replaced = {.generation = 13};
    assert(pthread_create(&thread, NULL, waiter, &replaced) == 0);
    assert(remote_track_test_wait_for_waiters(1));
    assert(remote_track_meta_set_all(&m, 1));
    assert(pthread_join(thread, NULL) == 0 && !replaced.result);

    remote_track_stream_begin_playback("remote://test/1", 14);
    wait_case_t stopped = {.generation = 14};
    assert(pthread_create(&thread, NULL, waiter, &stopped) == 0);
    assert(remote_track_test_wait_for_waiters(1));
    remote_track_stream_cancel_all();
    assert(pthread_join(thread, NULL) == 0 && !stopped.result);

    remote_track_stream_begin_playback("remote://test/1", 15);
    wait_case_t restarted = {.generation = 15};
    assert(pthread_create(&thread, NULL, waiter, &restarted) == 0);
    assert(remote_track_test_wait_for_waiters(1));
    remote_track_stream_begin_playback("remote://test/1", 16);
    assert(pthread_join(thread, NULL) == 0 && !restarted.result);

    /* Natural transition adopts an in-flight next-track request only. */
    remote_track_meta_t queue[4];
    for (int i = 0; i < 4; ++i) { queue[i] = entry(); snprintf(queue[i].track_id, sizeof(queue[i].track_id), "%d", i + 1); }
    assert(remote_track_meta_set_all(queue, 4));
    remote_track_stream_begin_playback("remote://test/1", 20);
    assert(remote_track_stream_prefetch("remote://test/1", 20));
    remote_track_stream_prefetch("remote://test/2", 20);
    remote_track_stream_prefetch("remote://test/3", 20);
    assert(remote_track_stream_take_request(&req)); remote_stream_request_t old_current = req;
    assert(remote_track_stream_take_request(&req)); remote_stream_request_t next_two = req;
    assert(remote_track_stream_take_request(&req)); remote_stream_request_t abandoned = req;
    remote_track_stream_begin_playback("remote://test/2", 21);
    assert(!remote_track_stream_provide(42, old_current.request_id, &result, NULL));
    result = queue[1]; strcpy(result.stream_url, "https://next/two");
    assert(remote_track_stream_provide(42, next_two.request_id, &result, NULL));
    assert(remote_track_stream_wait("remote://test/2", 21, &out));
    assert(!remote_track_stream_provide(42, abandoned.request_id, &result, NULL));

    /* Gapless next-track resolution is accepted while current track is active. */
    strcpy(queue[1].codec, "flac"); queue[1].sample_rate = 44100; queue[1].bit_depth = 24;
    assert(remote_track_meta_set_all(queue, 4));
    remote_track_stream_begin_playback("remote://test/1", 30);
    assert(remote_track_stream_prefetch("remote://test/2", 30));
    assert(remote_track_stream_take_request(&req));
    result = (remote_track_meta_t){0}; strcpy(result.stream_url, "https://prefetched/two");
    assert(remote_track_stream_provide(42, req.request_id, &result, NULL));
    assert(remote_track_meta_copy_fresh_for_path("remote://test/2", &out));
    assert(!strcmp(out.codec, "flac") && out.sample_rate == 44100 && out.bit_depth == 24);
    remote_track_stream_begin_playback("remote://test/2", 31);
    assert(remote_track_meta_copy_fresh_for_path("remote://test/2", &out));
    assert(!remote_track_stream_take_request(&req));

    /* Promotion refreshes an old prefetch's full resolution deadline. */
    remote_track_test_set_wait_timeout_ms(15000);
    remote_track_test_set_clock(70000);
    assert(remote_track_meta_set_all(queue, 4));
    remote_track_stream_begin_playback("remote://test/1", 32);
    assert(remote_track_stream_prefetch("remote://test/2", 32));
    assert(remote_track_stream_take_request(&req));
    remote_track_test_set_clock(84000);
    remote_track_stream_begin_playback("remote://test/2", 33);
    remote_track_test_set_clock(86000); /* old request age 16s; promoted age 2s */
    result = (remote_track_meta_t){0}; strcpy(result.stream_url, "https://promoted/two");
    assert(remote_track_stream_provide(42, req.request_id, &result, NULL));
    assert(!remote_track_stream_take_request(&req));

    /* New next candidates evict stale queued candidates while retaining current. */
    remote_track_stream_begin_playback("remote://test/1", 22);
    assert(remote_track_stream_prefetch("remote://test/1", 22));
    assert(remote_track_stream_prefetch("remote://test/2", 22));
    assert(remote_track_stream_prefetch("remote://test/3", 22));
    assert(remote_track_stream_prefetch("remote://test/4", 22));
    bool saw_current = false, saw_four = false;
    while (remote_track_stream_take_request(&req)) {
        if (!strcmp(req.track_id, "1")) saw_current = true;
        if (!strcmp(req.track_id, "4")) saw_four = true;
    }
    assert(saw_current && saw_four);

    /* The Lua-facing table cap safely copies 500 entries. */
    static remote_track_meta_t maximum_table[500];
    for (int i = 0; i < 500; ++i) {
        maximum_table[i] = entry();
        snprintf(maximum_table[i].track_id, sizeof(maximum_table[i].track_id), "id-%d", i);
    }
    assert(remote_track_meta_set_all(maximum_table, 500));
    assert(remote_track_meta_copy_for_path("remote://test/id-499", &out));
    assert(!remote_track_meta_copy_for_path("remote://test/id-500", &out));
    remote_track_stream_begin_playback("remote://test/id-0", 10);
    remote_track_test_set_wait_timeout_ms(5);
    assert(!remote_track_stream_wait("remote://test/id-0", 10, &out));
    assert(remote_track_stream_error_for_path("remote://test/id-0", 10, err, sizeof(err)));
    assert(strstr(err, "did not answer"));
    /* A plugin reload hands the queue to the new instance of the same plugin,
     * and queuing a request wakes the dispatcher. */
    remote_track_test_use_real_clock();
    remote_track_stream_set_request_notify(count_notify);
    m = entry(); strcpy(m.resolver_plugin, "community.test");
    assert(remote_track_meta_set_all(&m, 1));
    remote_track_stream_begin_playback("remote://test/1", 30);
    remote_track_stream_cancel_owner(42);
    assert(!remote_track_stream_prefetch("remote://test/1", 30));
    remote_track_stream_adopt_owner("community.other", 77);
    assert(!remote_track_stream_prefetch("remote://test/1", 30));
    remote_track_stream_adopt_owner("community.test", 77);
    notify_count = 0;
    assert(remote_track_stream_prefetch("remote://test/1", 30));
    assert(notify_count == 1);
    assert(remote_track_stream_take_request(&req) && req.owner == 77);
    remote_track_stream_set_request_notify(NULL);

    puts("remote track resolver: ok");
    return 0;
}
