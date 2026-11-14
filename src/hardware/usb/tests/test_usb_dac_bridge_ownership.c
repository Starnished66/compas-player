#define HOST_BUILD 1
#define USB_DAC_BRIDGE_TEST 1
#include "../usb_dac_bridge.c"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

static atomic_bool fake_audio_idle = true;
static atomic_int ensure_count;
static atomic_int open_count;
static atomic_int close_count;
static atomic_int write_count;
static atomic_bool fake_output_active;
static atomic_bool fake_ensure_succeeds = true;

bool audio_is_idle(void) { return atomic_load(&fake_audio_idle); }

bool audio_output_ensure(unsigned int channels, unsigned int sample_rate, bool low_latency, bool want_s24) {
    (void) channels; (void) sample_rate; (void) low_latency; (void) want_s24;
    atomic_fetch_add(&ensure_count, 1);
    if (!atomic_load(&fake_ensure_succeeds)) return false;
    if (!atomic_exchange(&fake_output_active, true)) atomic_fetch_add(&open_count, 1);
    return true;
}

void audio_output_close(void) {
    if (atomic_exchange(&fake_output_active, false)) atomic_fetch_add(&close_count, 1);
}
bool audio_output_is_s24_active(void) { return false; }
bool audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels, uint64_t * written) {
    (void) buf; (void) channels;
    atomic_fetch_add(&write_count, 1);
    *written = frames;
    return true;
}
bool audio_output_write_s24(const int32_t * buf, uint64_t frames, unsigned int channels, uint64_t * written) {
    (void) buf; (void) channels;
    atomic_fetch_add(&write_count, 1);
    *written = frames;
    return true;
}

static void wait_for(atomic_int * value, int expected) {
    for (int i = 0; i < 300 && atomic_load(value) < expected; ++i) usleep(10000);
    assert(atomic_load(value) >= expected);
}

static void write_test_audio(void) {
    int32_t frames[BRIDGE_PERIOD_FRAMES * BRIDGE_CHANNELS];
    memset(frames, 0, sizeof(frames));
    ring_buffer_write_drop_oldest(&g_rb, (const uint8_t *) frames, sizeof(frames));
}

static bool snapshot_owns_output(void) {
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    return info.owns_output;
}

static bool snapshot_output_releasing(void) {
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    return info.output_releasing;
}

static bool snapshot_output_failed(void) {
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    return info.output_failed;
}

static void start_fake_writer(pthread_t * writer) {
    assert(ring_buffer_init(&g_rb, RING_BUFFER_CAPACITY));
    atomic_store(&stop_requested, false);
    bridge_running = true;
    g_writer_running = true;
    g_reader_running = false;
    g_host_rate = 48000;
    g_host_bits = 32;
    g_host_format_gen = 1;
    g_host_format_known = true;
    output_failed = false;
    output_allowed = false;
    bridge_owns_output = false;
    output_releasing = false;
    assert(pthread_create(writer, NULL, bridge_writer_thread_func, NULL) == 0);
}

static void finish_fake_writer(pthread_t writer) {
    atomic_store(&stop_requested, true);
    ring_buffer_signal_finished(&g_rb);
    assert(pthread_join(writer, NULL) == 0);
    ring_buffer_destroy(&g_rb);
}

int main(void) {
    pthread_t writer;
    start_fake_writer(&writer);

    /* An idle gadget and a stream without the UI grant leave local output alone. */
    set_streaming(true);
    write_test_audio();
    usleep(100000);
    assert(atomic_load(&ensure_count) == 0);
    assert(atomic_load(&close_count) == 0);

    /* Permission plus an idle local player opens the output and publishes ownership. */
    usb_dac_bridge_set_output_allowed(true);
    write_test_audio();
    wait_for(&ensure_count, 1);
    wait_for(&write_count, 1);
    assert(snapshot_owns_output());

    /* A format change canceled while waiting for idle retains ownership until close. */
    atomic_store(&fake_audio_idle, false);
    pthread_mutex_lock(&bridge_mutex);
    g_host_format_gen++;
    pthread_mutex_unlock(&bridge_mutex);
    write_test_audio();
    usleep(100000);
    usb_dac_bridge_set_output_allowed(false);
    assert(snapshot_owns_output());
    int closes_before_format_cancel = atomic_load(&close_count);
    for (int i = 0; i < 100 && atomic_load(&close_count) == closes_before_format_cancel; ++i) usleep(10000);
    assert(atomic_load(&close_count) == closes_before_format_cancel + 1);
    assert(!snapshot_owns_output());
    atomic_store(&fake_audio_idle, true);
    usb_dac_bridge_set_output_allowed(true);
    write_test_audio();
    wait_for(&open_count, 2);

    /* A short host renegotiation inside the grace window keeps the device open. */
    int closes_before_flap = atomic_load(&close_count);
    set_streaming(false);
    usleep(100000);
    set_streaming(true);
    write_test_audio();
    usleep(350000);
    assert(atomic_load(&close_count) == closes_before_flap);
    assert(snapshot_owns_output());

    /* A stopped host releases after the grace period, then a resumed stream reopens. */
    int closes_before_stop = atomic_load(&close_count);
    uint64_t stop_started_ns = monotonic_ns();
    set_streaming(false);
    for (int i = 0; i < 100 && atomic_load(&close_count) == closes_before_stop; ++i) usleep(10000);
    assert(atomic_load(&close_count) == closes_before_stop + 1);
    assert(monotonic_ns() - stop_started_ns >= 250000000ULL);
    assert(!snapshot_owns_output());
    set_streaming(true);
    write_test_audio();
    wait_for(&open_count, 3);
    assert(snapshot_owns_output());

    /* Revocation while waiting for local playback to stop cancels before open. */
    set_streaming(false);
    usb_dac_bridge_set_output_allowed(false);
    for (int i = 0; i < 100 && (snapshot_owns_output() || snapshot_output_releasing()); ++i) usleep(10000);
    atomic_store(&fake_audio_idle, false);
    set_streaming(true);
    write_test_audio();
    usb_dac_bridge_set_output_allowed(true);
    for (int i = 0; i < 100 && !snapshot_owns_output(); ++i) usleep(10000);
    assert(snapshot_owns_output());
    int ensures_before_cancel = atomic_load(&ensure_count);
    usb_dac_bridge_set_output_allowed(false);
    for (int i = 0; i < 100 && snapshot_owns_output(); ++i) usleep(10000);
    assert(!snapshot_owns_output());
    atomic_store(&fake_audio_idle, true);
    usleep(100000);
    assert(atomic_load(&ensure_count) == ensures_before_cancel);

    finish_fake_writer(writer);

    /* An idle timeout on a format reopen closes the previously owned output. */
    start_fake_writer(&writer);
    set_streaming(true);
    usb_dac_bridge_set_output_allowed(true);
    write_test_audio();
    wait_for(&open_count, 4);
    int closes_before_timeout = atomic_load(&close_count);
    atomic_store(&fake_audio_idle, false);
    pthread_mutex_lock(&bridge_mutex);
    g_host_format_gen++;
    pthread_mutex_unlock(&bridge_mutex);
    write_test_audio();
    for (int i = 0; i < 300 && !snapshot_output_failed(); ++i) usleep(10000);
    assert(snapshot_output_failed());
    assert(atomic_load(&close_count) == closes_before_timeout + 1);
    assert(!snapshot_owns_output());
    assert(!atomic_load(&fake_output_active));
    assert(pthread_join(writer, NULL) == 0);
    ring_buffer_destroy(&g_rb);

    /* A first open failure reports failure without closing unowned local output. */
    atomic_store(&fake_audio_idle, true);
    start_fake_writer(&writer);
    atomic_store(&fake_ensure_succeeds, false);
    set_streaming(true);
    usb_dac_bridge_set_output_allowed(true);
    write_test_audio();
    for (int i = 0; i < 100 && !snapshot_output_failed(); ++i) usleep(10000);
    assert(snapshot_output_failed());
    assert(!atomic_load(&fake_output_active));
    assert(pthread_join(writer, NULL) == 0);
    ring_buffer_destroy(&g_rb);
    return 0;
}
