/* Route-level USB DSD coverage with a mocked direct-output module and aplay. */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "usb_dsd_output.h"
#include <tinyalsa/asoundlib.h>

enum { USB_DSD_OUTPUT_PCM = 0, USB_DSD_OUTPUT_NATIVE = 1, USB_DSD_OUTPUT_DOP = 2 };

static int direct_open_count, direct_close_count, direct_write_count;
static int aplay_count;
static bool direct_open_fails;
static bool last_native;
static unsigned int last_card, last_device;
static unsigned int direct_hardware_rate, direct_hardware_bits;
static int read_pipe_fd = -1;
static unsigned char direct_handle_storage;
static unsigned char fake_mixer_storage;

struct mixer *mixer_open(unsigned int card) { (void)card; return (struct mixer *)&fake_mixer_storage; }
struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *m, const char *name) {
    (void)m; (void)name; return NULL;
}
int mixer_ctl_set_value(struct mixer_ctl *ctl, unsigned int id, int value) {
    (void)ctl; (void)id; (void)value; return 0;
}
struct pcm *pcm_open(unsigned int card, unsigned int device, unsigned int flags,
                      const struct pcm_config *config) {
    (void)card; (void)device; (void)flags; (void)config; return NULL;
}
int pcm_close(struct pcm *pcm) { (void)pcm; return 0; }
int pcm_is_ready(const struct pcm *pcm) { (void)pcm; return 0; }
int pcm_writei(struct pcm *pcm, const void *data, unsigned int frames) {
    (void)pcm; (void)data; return (int)frames;
}

typedef struct usb_dsd_output usb_dsd_output_t;
usb_dsd_output_t *usb_dsd_output_open(unsigned int card, unsigned int device,
                                      bool native, unsigned int channels,
                                      unsigned int dop_rate) {
    assert(channels == 2 && dop_rate > 0);
    direct_open_count++;
    last_card = card;
    last_device = device;
    last_native = native;
    direct_hardware_rate = native ? dop_rate * 2 : dop_rate;
    direct_hardware_bits = native ? 8 : 24;
    return direct_open_fails ? NULL : (usb_dsd_output_t *)&direct_handle_storage;
}
bool usb_dsd_output_write(usb_dsd_output_t *out, const int32_t *words,
                          uint64_t frames, uint64_t *written) {
    assert(out == (usb_dsd_output_t *)&direct_handle_storage);
    assert(words && frames);
    direct_write_count++;
    if (written) *written = frames;
    return true;
}
void usb_dsd_output_close(usb_dsd_output_t *out) {
    if (out) direct_close_count++;
}
bool usb_dsd_output_is_native(const usb_dsd_output_t *out) {
    return out && last_native;
}
unsigned int usb_dsd_output_hardware_rate(const usb_dsd_output_t *out) {
    return out ? direct_hardware_rate : 0;
}
unsigned int usb_dsd_output_hardware_bits(const usb_dsd_output_t *out) {
    return out ? direct_hardware_bits : 0;
}

#include "audio_output.c"

/* Only routes exercised here need these application/OS dependencies. */
bool subprocess_popen_stdin(char *const argv[], pid_t *pid, int *fd) {
    int p[2];
    assert(argv && !strcmp(argv[0], "aplay"));
    assert(pipe(p) == 0);
    read_pipe_fd = p[0];
    *pid = 12345;
    *fd = p[1];
    aplay_count++;
    return true;
}
void subprocess_terminate(pid_t pid) { (void)pid; }
bool bt_control_prepare_source_volume(void) { return true; }
bool bt_control_prepare_playback_pcm(char *out, size_t size) {
    snprintf(out, size, "bluealsa");
    return true;
}
const char *bt_control_get_playback_pcm(void) { return "bluealsa"; }

static void close_mock_read_pipe(void) {
    if (read_pipe_fd >= 0) close(read_pipe_fd);
    read_pipe_fd = -1;
}

static void select_usb(const char *device) {
    audio_output_set_usb_requested(true, device);
}

static void direct_stream(int mode, const char *device) {
    audio_output_set_usb_dsd_mode(mode);
    select_usb(device);
    assert(audio_output_ensure_dop(2, 352800));
    assert(audio_output_is_dop_active());
    int32_t words[4] = {0x05696a00, 0x05696a00, 0x05696a00, 0x05696a00};
    uint64_t written = 0;
    assert(audio_output_write_s24(words, 2, 2, &written));
    assert(written == 2);
    audio_output_close();
}

static void *write_from_nonowner(void *unused) {
    (void)unused;
    int32_t words[2] = {0, 0};
    uint64_t written = 99;
    assert(!audio_output_write_s24(words, 1, 2, &written));
    assert(written == 0);
    return NULL;
}

int main(void) {
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_PCM);
    select_usb("plughw:2,3");
    assert(!audio_output_dsd_passthrough_requested(352800));
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_NATIVE);
    assert(audio_output_dsd_passthrough_requested(352800));
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_DOP);
    assert(audio_output_dsd_passthrough_requested(352800));

    direct_stream(USB_DSD_OUTPUT_DOP, "plughw:2,3");
    assert(direct_open_count == 1 && !last_native);
    assert(aplay_count == 0 && direct_write_count == 1);
    audio_output_info_t info;
    audio_output_get_info(&info);
    assert(!info.active);

    direct_stream(USB_DSD_OUTPUT_NATIVE, "plughw:2,3");
    assert(last_native && aplay_count == 0 && direct_write_count == 2);
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_NATIVE);
    select_usb("plughw:2,3");
    assert(audio_output_ensure_dop(2, 352800));
    audio_output_get_info(&info);
    assert(info.active && info.route == AUDIO_OUTPUT_ROUTE_USB && !info.dop);
    assert(info.native_dsd && info.sample_rate == 705600 && info.bit_depth == 8);
    audio_output_close();

    direct_stream(USB_DSD_OUTPUT_DOP, "plughw:2,3");
    select_usb("plughw:2,3");
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_DOP);
    assert(audio_output_ensure_dop(2, 352800));
    audio_output_get_info(&info);
    assert(info.active && info.route == AUDIO_OUTPUT_ROUTE_USB && info.dop);
    assert(!info.native_dsd && info.sample_rate == 352800 && info.bit_depth == 24);
    pthread_t other;
    assert(pthread_create(&other, NULL, write_from_nonowner, NULL) == 0);
    assert(pthread_join(other, NULL) == 0);

    int opens_before = direct_open_count;
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_NATIVE);
    assert(audio_output_ensure_dop(2, 352800)); /* mode change reopens */
    assert(direct_open_count == opens_before + 1 && last_native);
    select_usb("plughw:4,5");
    assert(audio_output_ensure_dop(2, 352800)); /* address change reopens */
    assert(last_card == 4 && last_device == 5);
    audio_output_close();

    /* Cache a refused rate, then verify retries leave a working PCM stream open. */
    direct_open_fails = true;
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_DOP);
    select_usb("plughw:2,3");
    int refused_open_count = direct_open_count;
    assert(!audio_output_ensure_dop(2, 352800));
    assert(direct_open_count == refused_open_count + 1);
    direct_open_fails = false;
    assert(!audio_output_dsd_passthrough_requested(352800));
    assert(audio_output_dsd_passthrough_requested(384000));
    select_usb("plughw:2,3"); /* unchanged device poll must preserve refusal */
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_DOP);
    assert(!audio_output_dsd_passthrough_requested(352800));

    int aplay_before_pcm = aplay_count;
    assert(audio_output_ensure(2, 44100, false, true));
    int direct_writes_before_pcm = direct_write_count;
    assert(aplay_count == aplay_before_pcm + 1);
    refused_open_count = direct_open_count;
    assert(!audio_output_ensure_dop(2, 352800));
    assert(direct_open_count == refused_open_count);
    assert(audio_output_is_s24_active());
    assert(audio_output_ensure(2, 44100, false, true));
    assert(aplay_count == aplay_before_pcm + 1);
    int32_t pcm_words[2] = {0, 0};
    uint64_t written = 0;
    assert(audio_output_write_s24(pcm_words, 1, 2, &written));
    assert(written == 1 && direct_write_count == direct_writes_before_pcm);

    /* A different DSD rate remains eligible and can open directly. */
    refused_open_count = direct_open_count;
    assert(audio_output_ensure_dop(2, 384000));
    assert(direct_open_count == refused_open_count + 1 && aplay_count == aplay_before_pcm + 1);
    audio_output_close();
    close_mock_read_pipe();

    /* Changing mode clears the refusal cache for the same device and rate. */
    direct_open_fails = true;
    assert(!audio_output_ensure_dop(2, 352800));
    refused_open_count = direct_open_count;
    assert(!audio_output_dsd_passthrough_requested(352800));
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_NATIVE);
    assert(audio_output_dsd_passthrough_requested(352800));
    assert(!audio_output_ensure_dop(2, 352800));
    assert(direct_open_count == refused_open_count + 1);
    direct_open_fails = false;

    /* A USB device change clears refusal; disconnect/reconnect does too. */
    direct_open_fails = true;
    assert(!audio_output_ensure_dop(2, 352800));
    assert(!audio_output_dsd_passthrough_requested(352800));
    select_usb("plughw:4,5");
    assert(audio_output_dsd_passthrough_requested(352800));
    direct_open_fails = false;
    assert(audio_output_ensure_dop(2, 352800));
    audio_output_close();
    direct_open_fails = true;
    assert(!audio_output_ensure_dop(2, 384000));
    assert(!audio_output_dsd_passthrough_requested(384000));
    audio_output_set_usb_requested(false, NULL);
    select_usb("plughw:4,5");
    assert(audio_output_dsd_passthrough_requested(384000));
    direct_open_fails = false;
    assert(audio_output_ensure_dop(2, 384000));
    audio_output_close();

    /* Bluetooth and local requests refuse USB pass-through before any aplay
     * pipe can be opened for DSD words. */
    audio_output_set_usb_requested(false, NULL);
    audio_output_set_bt_requested(true);
    audio_output_set_usb_dsd_mode(USB_DSD_OUTPUT_DOP);
    assert(!audio_output_dsd_passthrough_requested(352800));
    assert(!audio_output_ensure_dop(2, 352800));
    uint64_t rejected_written = 1;
    assert(!audio_output_write_s24(pcm_words, 1, 2, &rejected_written));
    assert(rejected_written == 0);
    audio_output_set_bt_requested(false);
    assert(!audio_output_dsd_passthrough_requested(352800));
    assert(!audio_output_ensure_dop(2, 352800));
    rejected_written = 1;
    assert(!audio_output_write_s24(pcm_words, 1, 2, &rejected_written));
    assert(rejected_written == 0);
    assert(aplay_count == 1);
    puts("USB DSD route spec passed");
    return 0;
}
