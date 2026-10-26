#include "selector_input.h"
#include "../../../../core/system/runtime/board_config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SELECTOR_INPUT_MAX_DEVICES 12
#define SELECTOR_INPUT_MAX_SLOTS 16
#define SELECTOR_INPUT_MAX_BATCH 256
#define SELECTOR_INPUT_READ_RECORDS 32
#define SELECTOR_INPUT_TEST_QUEUE 128
#define SELECTOR_INPUT_MAX_KEY_CODE 767
#define SELECTOR_ACTIVATE_MAX_MS 1200
#define SELECTOR_EVDEV_RECORD_SIZE 16

typedef struct {
    int32_t seconds;
    int32_t microseconds;
    uint16_t type;
    uint16_t code;
    int32_t value;
} selector_evdev_record_t;

_Static_assert(sizeof(selector_evdev_record_t) == SELECTOR_EVDEV_RECORD_SIZE,
               "selector evdev parser expects the 32-bit MIPS input_event layout");

typedef enum {
    DEVICE_KEYS = 0,
    DEVICE_TOUCH
} selector_device_kind_t;

typedef enum {
    KEY_KIND_POWER = 0,
    KEY_KIND_VOLUME_UP,
    KEY_KIND_VOLUME_DOWN,
    KEY_KIND_PLAY,
    KEY_KIND_COUNT
} selector_key_kind_t;

typedef struct {
    int fd;
    char path[64];
    char name[128];
    selector_device_kind_t kind;
    bool live;
    bool pending_cancel;
    bool drop_until_report;
    bool key_down[KEY_KIND_COUNT];
    bool key_ignore_until_release[KEY_KIND_COUNT];
    uint64_t key_pressed_ms[KEY_KIND_COUNT];

    int x_min;
    int x_max;
    int y_min;
    int y_max;
    int raw_x;
    int raw_y;
    bool have_x;
    bool have_y;
    bool mt_available;
    bool slot_capable;
    bool synthetic;
    /* The R1 hyn_ts driver reports coordinates in panel pixels even though
     * EVIOCGABS advertises the larger sensor/controller range. */
    bool pixel_coordinates;
    int slot_min;
    int current_slot;
    unsigned int type_a_contacts;
    bool type_a_contact_payload;
    bool has_btn_touch;
    bool slot_active[SELECTOR_INPUT_MAX_SLOTS];
    int slot_tracking_id[SELECTOR_INPUT_MAX_SLOTS];
    int primary_slot;
    int primary_tracking_id;
    int slot_x[SELECTOR_INPUT_MAX_SLOTS];
    int slot_y[SELECTOR_INPUT_MAX_SLOTS];
    bool slot_have_x[SELECTOR_INPUT_MAX_SLOTS];
    bool slot_have_y[SELECTOR_INPUT_MAX_SLOTS];
    bool btn_touch;
    bool touch_down;
    bool ignore_touch_until_up;
    bool touch_changed;
    uint8_t partial_record[SELECTOR_EVDEV_RECORD_SIZE];
    size_t partial_size;
} selector_input_device_t;

struct selector_input {
    int width;
    int height;
    selector_input_device_t devices[SELECTOR_INPUT_MAX_DEVICES];
    size_t device_count;
    bool test_mode;
    int test_x_min;
    int test_x_max;
    int test_y_min;
    int test_y_max;
    selector_input_event_t queue[SELECTOR_INPUT_TEST_QUEUE];
    size_t queue_head;
    size_t queue_count;
    selector_input_event_cb active_callback;
    void * active_userdata;
};

static uint64_t monotonic_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static void queue_event(selector_input_t * input, selector_input_event_type_t type,
                        int x, int y, int value, uint64_t when_ms) {
    if (!input) return;
    selector_input_event_t event = { type, x, y, value, when_ms };
    if (input->active_callback) {
        input->active_callback(&event, input->active_userdata);
        return;
    }
    if (input->queue_count == SELECTOR_INPUT_TEST_QUEUE) {
        /* Keep cancellation observable under hostile test/event floods. */
        size_t tail = (input->queue_head + input->queue_count - 1) % SELECTOR_INPUT_TEST_QUEUE;
        input->queue[tail] = (selector_input_event_t){
            SELECTOR_INPUT_CANCEL, 0, 0, 0, when_ms
        };
        return;
    }
    size_t tail = (input->queue_head + input->queue_count) % SELECTOR_INPUT_TEST_QUEUE;
    input->queue[tail] = event;
    input->queue_count++;
}

static void drain_queue(selector_input_t * input, selector_input_event_cb callback,
                        void * userdata) {
    while (input->queue_count) {
        selector_input_event_t event = input->queue[input->queue_head];
        input->queue_head = (input->queue_head + 1) % SELECTOR_INPUT_TEST_QUEUE;
        input->queue_count--;
        if (callback) callback(&event, userdata);
    }
}

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static int map_axis(int value, int min, int max, int pixels) {
    if (pixels <= 1 || max <= min) return 0;
    value = clamp_int(value, min, max);
    int64_t offset = (int64_t) value - min;
    int64_t range = (int64_t) max - min;
    int64_t mapped = offset * (pixels - 1) / range;
    return clamp_int((int) mapped, 0, pixels - 1);
}

static int mapped_x(const selector_input_t * input, const selector_input_device_t * device) {
    if (device && device->pixel_coordinates)
        return clamp_int(device->raw_x, 0, input->width - 1);
    int min = device ? device->x_min : input->test_x_min;
    int max = device ? device->x_max : input->test_x_max;
    int raw = device ? device->raw_x : input->test_x_min;
    return map_axis(raw, min, max, input->width);
}

static int mapped_y(const selector_input_t * input, const selector_input_device_t * device) {
    if (device && device->pixel_coordinates)
        return clamp_int(device->raw_y, 0, input->height - 1);
    int min = device ? device->y_min : input->test_y_min;
    int max = device ? device->y_max : input->test_y_max;
    int raw = device ? device->raw_y : input->test_y_min;
    return map_axis(raw, min, max, input->height);
}

static int key_kind(unsigned int key) {
    switch (key) {
        case KEY_POWER: return KEY_KIND_POWER;
        case KEY_VOLUMEUP: return KEY_KIND_VOLUME_UP;
        case KEY_VOLUMEDOWN: return KEY_KIND_VOLUME_DOWN;
        case KEY_PLAYPAUSE:
        case KEY_PLAY:
        case KEY_PLAYCD:
        case KEY_PAUSECD: return KEY_KIND_PLAY;
        default: return -1;
    }
}

static void cancel_device(selector_input_t * input, selector_input_device_t * device,
                          uint64_t when_ms) {
    if (device) {
        for (int i = 0; i < KEY_KIND_COUNT; i++) {
            if (device->key_down[i]) device->key_ignore_until_release[i] = true;
            device->key_down[i] = false;
        }
        device->touch_down = false;
        device->primary_slot = -1;
        device->primary_tracking_id = -1;
        device->ignore_touch_until_up = true;
        device->touch_changed = false;
        device->pending_cancel = false;
    }
    queue_event(input, SELECTOR_INPUT_CANCEL, 0, 0, 0, when_ms);
}

static void feed_key(selector_input_t * input, selector_input_device_t * device,
                     unsigned int key, int value, uint64_t when_ms,
                     bool startup_held) {
    int kind = key_kind(key);
    if (!input || kind < 0 || value < 0 || value > 2) return;
    if (startup_held) {
        if (device) {
            device->key_down[kind] = false;
            device->key_ignore_until_release[kind] = true;
            device->pending_cancel = true;
        }
        return;
    }
    if (value == 0) {
        bool ignored = device && device->key_ignore_until_release[kind];
        if (ignored) {
            device->key_ignore_until_release[kind] = false;
            device->key_down[kind] = false;
            return;
        }
        if (!device || !device->key_down[kind]) return;
        device->key_down[kind] = false;
        if (kind == KEY_KIND_POWER || kind == KEY_KIND_PLAY) {
            uint64_t elapsed = when_ms >= device->key_pressed_ms[kind]
                ? when_ms - device->key_pressed_ms[kind] : UINT64_MAX;
            if (elapsed < SELECTOR_ACTIVATE_MAX_MS)
                queue_event(input, SELECTOR_INPUT_ACTIVATE, 0, 0, 0, when_ms);
        }
        return;
    }
    if (value == 2) {
        if (kind == KEY_KIND_VOLUME_UP || kind == KEY_KIND_VOLUME_DOWN) {
            if (!device || device->key_ignore_until_release[kind] || !device->key_down[kind]) return;
            queue_event(input, kind == KEY_KIND_VOLUME_UP ? SELECTOR_INPUT_NAV_UP :
                        SELECTOR_INPUT_NAV_DOWN, 0, 0, 1, when_ms);
        }
        return;
    }
    if (!device || device->key_ignore_until_release[kind] || device->key_down[kind]) return;
    device->key_down[kind] = true;
    device->key_pressed_ms[kind] = when_ms;
    if (kind == KEY_KIND_POWER || kind == KEY_KIND_PLAY)
        queue_event(input, SELECTOR_INPUT_CANCEL, 0, 0, 0, when_ms);
    if (kind == KEY_KIND_VOLUME_UP || kind == KEY_KIND_VOLUME_DOWN)
        queue_event(input, kind == KEY_KIND_VOLUME_UP ? SELECTOR_INPUT_NAV_UP :
                    SELECTOR_INPUT_NAV_DOWN, 0, 0, 0, when_ms);
}

static unsigned int active_contacts(const selector_input_device_t * device) {
    unsigned int count = 0;
    for (size_t i = 0; i < SELECTOR_INPUT_MAX_SLOTS; i++)
        if (device->slot_active[i]) count++;
    if (!device->mt_available) {
        if (device->type_a_contacts > 1) return device->type_a_contacts;
        if (device->has_btn_touch) return device->btn_touch ? 1u : 0u;
        return device->type_a_contacts;
    }
    /* BTN_TOUCH catches drivers that report the aggregate key but omit a
     * tracking slot. Do not double-count the same primary contact. */
    if (count == 0 && device->btn_touch) return 1;
    return count;
}

static void report_touch(selector_input_t * input, selector_input_device_t * device,
                         bool changed, uint64_t when_ms) {
    unsigned int contacts = active_contacts(device);
    int current_slot = -1;
    int tracking_id = -1;
    if (device->mt_available && contacts == 1) {
        for (size_t i = 0; i < SELECTOR_INPUT_MAX_SLOTS; i++) {
            if (!device->slot_active[i]) continue;
            current_slot = (int) i;
            tracking_id = device->slot_tracking_id[i];
            if (device->slot_have_x[i]) device->raw_x = device->slot_x[i];
            if (device->slot_have_y[i]) device->raw_y = device->slot_y[i];
            break;
        }
    }
    int x = mapped_x(input, device);
    int y = mapped_y(input, device);
    if (device->ignore_touch_until_up) {
        if (contacts == 0) device->ignore_touch_until_up = false;
        device->touch_down = false;
        device->primary_slot = -1;
        device->primary_tracking_id = -1;
        return;
    }
    if (contacts > 1) {
        if (device->touch_down || !device->ignore_touch_until_up)
            cancel_device(input, device, when_ms);
        device->ignore_touch_until_up = true;
        return;
    }
    if (contacts == 1) {
        /* A primary finger replacement within one contact-count frame is
         * ambiguous to a gesture consumer, so cancel until all fingers lift. */
        if (device->touch_down && device->mt_available &&
            (device->primary_slot != current_slot ||
             (device->primary_tracking_id >= 0 && tracking_id >= 0 &&
              device->primary_tracking_id != tracking_id))) {
            cancel_device(input, device, when_ms);
            device->ignore_touch_until_up = true;
            return;
        }
        if (!device->touch_down) {
            device->touch_down = true;
            device->primary_slot = current_slot;
            device->primary_tracking_id = tracking_id;
            device->touch_changed = false;
            queue_event(input, SELECTOR_INPUT_TOUCH_DOWN, x, y, 0, when_ms);
        } else if (changed || device->touch_changed) {
            queue_event(input, SELECTOR_INPUT_TOUCH_MOVE, x, y, 0, when_ms);
        }
        device->touch_changed = false;
        return;
    }
    if (device->touch_down) {
        device->touch_down = false;
        queue_event(input, SELECTOR_INPUT_TOUCH_UP, x, y, 0, when_ms);
    }
    device->primary_slot = -1;
    device->primary_tracking_id = -1;
    device->touch_changed = false;
}

static void inject_touch_to_device(selector_input_t * input, selector_input_device_t * device,
                                   int raw_x, int raw_y, unsigned int contacts,
                                   bool changed, uint64_t when_ms, bool startup_held) {
    if (!input || !device) return;
    device->raw_x = raw_x;
    device->raw_y = raw_y;
    device->mt_available = true;
    memset(device->slot_active, 0, sizeof(device->slot_active));
    for (unsigned int i = 0; i < contacts; i++) device->slot_active[i] = true;
    if (startup_held && contacts) {
        device->ignore_touch_until_up = true;
        device->pending_cancel = true;
    }
    report_touch(input, device, changed, when_ms);
}

selector_input_t * selector_input_create(int screen_width, int screen_height) {
    if (screen_width <= 0 || screen_height <= 0) return NULL;
    selector_input_t * input = calloc(1, sizeof(*input));
    if (!input) return NULL;
    input->width = screen_width;
    input->height = screen_height;
    input->test_x_max = screen_width - 1;
    input->test_y_max = screen_height - 1;
    for (size_t i = 0; i < SELECTOR_INPUT_MAX_DEVICES; i++) input->devices[i].fd = -1;
    return input;
}

selector_input_t * selector_input_create_for_test(int screen_width, int screen_height) {
    selector_input_t * input = selector_input_create(screen_width, screen_height);
    if (input) input->test_mode = true;
    return input;
}

static bool whitelisted_name(const char * name, selector_device_kind_t * kind) {
    if (!name || !kind) return false;
    if (strcmp(name, "md-gpio-keys") == 0 || strcmp(name, "jz adc keyboard") == 0) {
        *kind = DEVICE_KEYS;
        return true;
    }
#if defined(BOARD_R3PROII) || defined(BOARD_R3II_2025)
    if (strcmp(name, "goodix-ts") == 0) {
        *kind = DEVICE_TOUCH;
        return true;
    }
#else
    if (strcmp(name, "hyn_ts") == 0) {
        *kind = DEVICE_TOUCH;
        return true;
    }
#endif
    return false;
}

static bool query_abs_range(int fd, unsigned int code, int * min, int * max) {
    struct input_absinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, EVIOCGABS(code), &info) < 0 || info.maximum <= info.minimum) return false;
    *min = info.minimum;
    *max = info.maximum;
    return true;
}

static bool has_evdev_code(int fd, unsigned int type, unsigned int code) {
    if (type > EV_MAX) return false;
    unsigned long bits[(KEY_MAX + sizeof(unsigned long) * 8) /
                       (sizeof(unsigned long) * 8)];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits) < 0) return false;
    return ((bits[code / (8 * sizeof(unsigned long))] >>
             (code % (8 * sizeof(unsigned long)))) & 1UL) != 0;
}

static bool query_touch_slots(selector_input_device_t * device) {
    struct input_absinfo slots;
    memset(&slots, 0, sizeof(slots));
    int slot_info_result = ioctl(device->fd, EVIOCGABS(ABS_MT_SLOT), &slots);
    /* Some type-A drivers return a zero-filled absinfo for an unsupported
     * axis. The advertised ABS bits, not that ioctl's success alone, decide
     * whether slot state exists. */
    if (!device->slot_capable) {
        device->mt_available = false;
        return true;
    }
    if (slot_info_result < 0 || slots.maximum < slots.minimum) return false;
    int first = slots.minimum < 0 ? 0 : slots.minimum;
    int current = slots.value >= first && slots.value <= slots.maximum
        ? slots.value : first;
    int count = slots.maximum - first + 1;
    if (count < 1) return false;
    if (count > SELECTOR_INPUT_MAX_SLOTS) count = SELECTOR_INPUT_MAX_SLOTS;
    int32_t values[SELECTOR_INPUT_MAX_SLOTS + 1] = {0};
    values[0] = ABS_MT_TRACKING_ID;
    if (ioctl(device->fd, EVIOCGMTSLOTS(sizeof(values)), values) < 0) return false;
    device->slot_min = first;
    device->current_slot = current;
    device->mt_available = true;
    bool held = false;
    for (int i = 0; i < count; i++) {
        device->slot_tracking_id[i] = values[i + 1];
        device->slot_active[i] = values[i + 1] >= 0;
        held = held || device->slot_active[i];
    }
    if (held) {
        device->ignore_touch_until_up = true;
        device->pending_cancel = true;
    }
    return true;
}

static void query_initial_state(selector_input_t * input, selector_input_device_t * device) {
    unsigned long keys[(KEY_MAX + sizeof(unsigned long) * 8) /
                       (sizeof(unsigned long) * 8)];
    memset(keys, 0, sizeof(keys));
    if (ioctl(device->fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
        static const unsigned int key_codes[KEY_KIND_COUNT] = {
            KEY_POWER, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_PLAYPAUSE
        };
        for (int kind = 0; kind < KEY_KIND_COUNT; kind++) {
            unsigned int code = key_codes[kind];
            bool down = ((keys[code / (8 * sizeof(unsigned long))] >>
                         (code % (8 * sizeof(unsigned long)))) & 1UL) != 0;
            if (kind == KEY_KIND_PLAY) {
                static const unsigned int play_codes[] = { KEY_PLAY, KEY_PLAYCD, KEY_PAUSECD };
                for (size_t i = 0; i < sizeof(play_codes) / sizeof(play_codes[0]); i++) {
                    code = play_codes[i];
                    down = down || ((keys[code / (8 * sizeof(unsigned long))] >>
                                     (code % (8 * sizeof(unsigned long)))) & 1UL) != 0;
                }
            }
            if (down) feed_key(input, device, key_codes[kind], 1, monotonic_now_ms(), true);
        }
        if (device->kind == DEVICE_TOUCH) {
            bool touching = ((keys[BTN_TOUCH / (8 * sizeof(unsigned long))] >>
                             (BTN_TOUCH % (8 * sizeof(unsigned long)))) & 1UL) != 0;
            if (touching) {
                device->ignore_touch_until_up = true;
                device->pending_cancel = true;
                device->btn_touch = true;
            }
        }
    } else if (device->kind == DEVICE_KEYS) {
        /* If initial state cannot be queried, never interpret a release as a
         * fresh activation until each key has been observed up. */
        for (int kind = 0; kind < KEY_KIND_COUNT; kind++)
            device->key_ignore_until_release[kind] = true;
        device->pending_cancel = true;
    } else if (device->kind == DEVICE_TOUCH) {
        device->ignore_touch_until_up = true;
        device->pending_cancel = true;
    }
    if (device->kind == DEVICE_TOUCH && !query_touch_slots(device)) {
        device->ignore_touch_until_up = true;
        device->pending_cancel = true;
    }
}

static bool add_device(selector_input_t * input, const char * path) {
    if (input->device_count >= SELECTOR_INPUT_MAX_DEVICES) return false;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    char name[128] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        close(fd);
        return false;
    }
    name[sizeof(name) - 1] = '\0';
    selector_device_kind_t kind;
    if (!whitelisted_name(name, &kind)) {
        close(fd);
        return false;
    }
    selector_input_device_t * device = &input->devices[input->device_count];
    memset(device, 0, sizeof(*device));
    device->fd = fd;
    device->live = true;
    device->kind = kind;
    snprintf(device->path, sizeof(device->path), "%s", path);
    snprintf(device->name, sizeof(device->name), "%s", name);
#if !defined(BOARD_R3PROII) && !defined(BOARD_R3II_2025)
    /* The built-in R1 Hynitron driver exposes ABS ranges of 720x1280, while
     * its actual ABS_MT_POSITION values are already panel-pixel coordinates.
     * Keep this exception tied to that exact board/device pair. */
    device->pixel_coordinates = kind == DEVICE_TOUCH && strcmp(name, "hyn_ts") == 0;
#endif
    device->x_max = input->width - 1;
    device->y_max = input->height - 1;
    if (kind == DEVICE_TOUCH) {
        bool mt_x = query_abs_range(fd, ABS_MT_POSITION_X, &device->x_min, &device->x_max);
        bool mt_y = query_abs_range(fd, ABS_MT_POSITION_Y, &device->y_min, &device->y_max);
        bool single_x = false, single_y = false;
        if (!(mt_x && mt_y)) {
            single_x = query_abs_range(fd, ABS_X, &device->x_min, &device->x_max);
            single_y = query_abs_range(fd, ABS_Y, &device->y_min, &device->y_max);
        }
        device->have_x = (mt_x && mt_y) || (single_x && single_y);
        device->have_y = device->have_x;
        bool has_btn_touch = has_evdev_code(fd, EV_KEY, BTN_TOUCH);
        device->has_btn_touch = has_btn_touch;
        bool has_tracking = has_evdev_code(fd, EV_ABS, ABS_MT_TRACKING_ID);
        bool has_mt_slots = has_evdev_code(fd, EV_ABS, ABS_MT_SLOT);
        device->slot_capable = has_tracking && has_mt_slots;
        bool has_contact = has_btn_touch || has_tracking;
        if (!device->have_x || !device->have_y || !has_contact) {
            close(fd);
            memset(device, 0, sizeof(*device));
            device->fd = -1;
            return false;
        }
    } else {
        bool has_button = has_evdev_code(fd, EV_KEY, KEY_POWER) ||
                          has_evdev_code(fd, EV_KEY, KEY_VOLUMEUP) ||
                          has_evdev_code(fd, EV_KEY, KEY_VOLUMEDOWN) ||
                          has_evdev_code(fd, EV_KEY, KEY_PLAYPAUSE) ||
                          has_evdev_code(fd, EV_KEY, KEY_PLAY) ||
                          has_evdev_code(fd, EV_KEY, KEY_PLAYCD) ||
                          has_evdev_code(fd, EV_KEY, KEY_PAUSECD);
        if (!has_button) {
            close(fd);
            memset(device, 0, sizeof(*device));
            device->fd = -1;
            return false;
        }
    }
    input->device_count++;
    query_initial_state(input, device);
    return true;
}

bool selector_input_open(selector_input_t * input) {
    if (!input || input->test_mode) return false;
    selector_input_close(input);
    DIR * dir = opendir("/dev/input");
    if (!dir) return false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL && input->device_count < SELECTOR_INPUT_MAX_DEVICES) {
        if (strncmp(entry->d_name, "event", 5) != 0 || entry->d_name[5] == '\0') continue;
        bool valid = true;
        for (const char * p = entry->d_name + 5; *p; p++)
            if (*p < '0' || *p > '9') { valid = false; break; }
        if (!valid) continue;
        char path[64];
        int n = snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (n < 0 || (size_t) n >= sizeof(path)) continue;
        (void) add_device(input, path);
    }
    closedir(dir);
    if (input->device_count == 0) {
        selector_input_close(input);
        return false;
    }
    return true;
}

void selector_input_close(selector_input_t * input) {
    if (!input) return;
    for (size_t i = 0; i < input->device_count; i++) {
        selector_input_device_t * device = &input->devices[i];
        if (device->live && device->fd >= 0) close(device->fd);
        memset(device, 0, sizeof(*device));
        device->fd = -1;
    }
    input->device_count = 0;
    input->active_callback = NULL;
    input->active_userdata = NULL;
}

void selector_input_destroy(selector_input_t * input) {
    if (!input) return;
    selector_input_close(input);
    free(input);
}

static bool read_touch_bitmap(int fd, unsigned long * keys, size_t size) {
    memset(keys, 0, size);
    return ioctl(fd, EVIOCGKEY(size), keys) >= 0;
}

static void recover_after_dropped(selector_input_t * input, selector_input_device_t * device,
                                  uint64_t when_ms) {
    for (int i = 0; i < KEY_KIND_COUNT; i++) {
        if (device->key_down[i]) device->key_ignore_until_release[i] = true;
        device->key_down[i] = false;
    }
    device->slot_active[0] = false;
    memset(device->slot_active, 0, sizeof(device->slot_active));
    device->btn_touch = false;
    device->type_a_contacts = 0;
    device->type_a_contact_payload = false;
    device->touch_down = false;
    device->ignore_touch_until_up = true;
    cancel_device(input, device, when_ms);
}

static bool key_bit(const unsigned long * keys, unsigned int code) {
    return ((keys[code / (8 * sizeof(unsigned long))] >>
             (code % (8 * sizeof(unsigned long)))) & 1UL) != 0;
}

static void refresh_touch_after_drop(selector_input_device_t * device) {
    if (device->kind != DEVICE_TOUCH) return;
    unsigned long keys[(KEY_MAX + sizeof(unsigned long) * 8) /
                       (sizeof(unsigned long) * 8)];
    bool keys_known = read_touch_bitmap(device->fd, keys, sizeof(keys));
    if (keys_known) device->btn_touch = key_bit(keys, BTN_TOUCH);
    memset(device->slot_active, 0, sizeof(device->slot_active));
    bool slots_known = !device->slot_capable || query_touch_slots(device);
    if (!keys_known || !slots_known) {
        device->ignore_touch_until_up = true;
        return;
    }
    device->ignore_touch_until_up = device->btn_touch || active_contacts(device) > 0;
}

static void process_record(selector_input_t * input, selector_input_device_t * device,
                           uint16_t type, uint16_t code, int32_t value, uint64_t when_ms) {
    if (type == EV_SYN && code == SYN_DROPPED) {
        device->drop_until_report = true;
        recover_after_dropped(input, device, when_ms);
        return;
    }
    if (device->drop_until_report) {
        if (type == EV_SYN && code == SYN_REPORT) {
            device->drop_until_report = false;
            /* Reconcile held keys with kernel state. Anything still down is
             * suppressed until its real release; a lost release can never
             * become an activation. */
            unsigned long keys[(KEY_MAX + sizeof(unsigned long) * 8) /
                               (sizeof(unsigned long) * 8)];
            static const unsigned int codes[KEY_KIND_COUNT] = {
                KEY_POWER, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_PLAYPAUSE
            };
            if (read_touch_bitmap(device->fd, keys, sizeof(keys))) {
                for (int k = 0; k < KEY_KIND_COUNT; k++) {
                    bool down = key_bit(keys, codes[k]);
                    if (k == KEY_KIND_PLAY)
                        down = down || key_bit(keys, KEY_PLAY) || key_bit(keys, KEY_PLAYCD) || key_bit(keys, KEY_PAUSECD);
                    device->key_down[k] = false;
                    device->key_ignore_until_release[k] = down;
                }
            } else {
                for (int k = 0; k < KEY_KIND_COUNT; k++) {
                    device->key_down[k] = false;
                    device->key_ignore_until_release[k] = true;
                }
            }
            refresh_touch_after_drop(device);
        }
        return;
    }
    if (type == EV_SYN && code == SYN_REPORT) {
        if (device->kind == DEVICE_TOUCH) {
            report_touch(input, device, device->touch_changed, when_ms);
            device->type_a_contacts = 0;
            device->type_a_contact_payload = false;
        }
        return;
    }
    if (device->kind == DEVICE_TOUCH && type == EV_SYN && code == SYN_MT_REPORT) {
        /* Type-A drivers may emit an empty SYN_MT_REPORT as a release
         * separator. Count only contacts that carried an MT payload. */
        if (device->type_a_contact_payload) device->type_a_contacts++;
        device->type_a_contact_payload = false;
        device->touch_changed = true;
        return;
    }
    if (device->kind == DEVICE_KEYS && type == EV_KEY) {
        feed_key(input, device, code, value, when_ms, false);
        return;
    }
    if (device->kind != DEVICE_TOUCH) return;
    if (type == EV_KEY && code == BTN_TOUCH) {
        device->has_btn_touch = true;
        device->btn_touch = value != 0;
        device->touch_changed = true;
    } else if (type == EV_ABS) {
        switch (code) {
            case ABS_MT_SLOT:
                if (device->slot_capable || device->synthetic) {
                    device->slot_capable = true;
                    device->mt_available = true;
                    device->current_slot = value;
                }
                break;
            case ABS_MT_TRACKING_ID:
                if (value >= 0) device->type_a_contact_payload = true;
                if (device->current_slot >= device->slot_min &&
                    device->current_slot - device->slot_min < SELECTOR_INPUT_MAX_SLOTS) {
                    size_t slot = (size_t) (device->current_slot - device->slot_min);
                    device->slot_tracking_id[slot] = value;
                    device->slot_active[slot] = value >= 0;
                    if (value < 0) { device->slot_have_x[slot] = false; device->slot_have_y[slot] = false; }
                }
                device->touch_changed = true;
                break;
            case ABS_MT_POSITION_X:
                device->type_a_contact_payload = true;
                if (!device->mt_available) {
                    device->raw_x = value;
                } else if (device->current_slot >= device->slot_min &&
                    device->current_slot - device->slot_min < SELECTOR_INPUT_MAX_SLOTS) {
                    size_t slot = (size_t) (device->current_slot - device->slot_min);
                    device->slot_x[slot] = value;
                    device->slot_have_x[slot] = true;
                }
                device->touch_changed = true;
                break;
            case ABS_MT_POSITION_Y:
                device->type_a_contact_payload = true;
                if (!device->mt_available) {
                    device->raw_y = value;
                } else if (device->current_slot >= device->slot_min &&
                    device->current_slot - device->slot_min < SELECTOR_INPUT_MAX_SLOTS) {
                    size_t slot = (size_t) (device->current_slot - device->slot_min);
                    device->slot_y[slot] = value;
                    device->slot_have_y[slot] = true;
                }
                device->touch_changed = true;
                break;
            case ABS_X:
                device->raw_x = value;
                device->touch_changed = true;
                break;
            case ABS_Y:
                device->raw_y = value;
                device->touch_changed = true;
                break;
            default:
                break;
        }
    }
}

static void close_lost_device(selector_input_t * input, selector_input_device_t * device,
                              uint64_t when_ms) {
    if (device->live && device->fd >= 0) close(device->fd);
    device->live = false;
    device->fd = -1;
    device->pending_cancel = false;
    cancel_device(input, device, when_ms);
}

static selector_input_device_t * get_test_device(selector_input_t * input,
                                                 selector_device_kind_t kind) {
    for (size_t i = 0; i < input->device_count; i++) {
        selector_input_device_t * device = &input->devices[i];
        if (device->live && device->synthetic && device->kind == kind) return device;
    }
    if (input->device_count >= SELECTOR_INPUT_MAX_DEVICES) return NULL;
    selector_input_device_t * device = &input->devices[input->device_count++];
    memset(device, 0, sizeof(*device));
    device->fd = -1;
    device->live = true;
    device->synthetic = true;
    device->kind = kind;
    device->x_min = input->test_x_min;
    device->x_max = input->test_x_max;
    device->y_min = input->test_y_min;
    device->y_max = input->test_y_max;
    return device;
}

int selector_input_poll(selector_input_t * input, int timeout_ms,
                        selector_input_event_cb callback, void * userdata) {
    if (!input || !callback) return -1;
    input->active_callback = callback;
    input->active_userdata = userdata;
    drain_queue(input, callback, userdata);
    uint64_t now = monotonic_now_ms();
    for (size_t i = 0; i < input->device_count; i++) {
        selector_input_device_t * device = &input->devices[i];
        if (device->pending_cancel) {
            device->pending_cancel = false;
            queue_event(input, SELECTOR_INPUT_CANCEL, 0, 0, 0, now);
        }
    }
    drain_queue(input, callback, userdata);
    if (input->test_mode) {
        input->active_callback = NULL;
        input->active_userdata = NULL;
        return 0;
    }

    struct pollfd fds[SELECTOR_INPUT_MAX_DEVICES];
    size_t indexes[SELECTOR_INPUT_MAX_DEVICES];
    nfds_t nfds = 0;
    for (size_t i = 0; i < input->device_count; i++) {
        if (!input->devices[i].live) continue;
        fds[nfds] = (struct pollfd){ .fd = input->devices[i].fd, .events = POLLIN };
        indexes[nfds++] = i;
    }
    if (nfds == 0) {
        input->active_callback = NULL;
        input->active_userdata = NULL;
        return -1;
    }
    int ready = poll(fds, nfds, timeout_ms < 0 ? 0 : timeout_ms);
    if (ready < 0) {
        input->active_callback = NULL;
        input->active_userdata = NULL;
        if (errno == EINTR) return 1;
        return 0;
    }

    size_t records = 0;
    bool more = false;
    for (nfds_t i = 0; i < nfds; i++) {
        short revents = fds[i].revents;
        if (!(revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) continue;
        selector_input_device_t * device = &input->devices[indexes[i]];
        if (!device->live) continue;
        if (revents & POLLNVAL) {
            close_lost_device(input, device, monotonic_now_ms());
            continue;
        }
        if (revents & POLLIN) {
            size_t device_records = 0;
            while (device_records < SELECTOR_INPUT_READ_RECORDS &&
                   records < SELECTOR_INPUT_MAX_BATCH) {
                uint8_t bytes[SELECTOR_INPUT_READ_RECORDS * SELECTOR_EVDEV_RECORD_SIZE];
                size_t prefix = device->partial_size;
                size_t capacity_records = SELECTOR_INPUT_READ_RECORDS - device_records;
                size_t batch_remaining = SELECTOR_INPUT_MAX_BATCH - records;
                if (capacity_records > batch_remaining) capacity_records = batch_remaining;
                size_t capacity = capacity_records * SELECTOR_EVDEV_RECORD_SIZE;
                if (capacity <= prefix) capacity = prefix + SELECTOR_EVDEV_RECORD_SIZE;
                if (prefix) memcpy(bytes, device->partial_record, prefix);
                ssize_t count = read(device->fd, bytes + prefix, capacity - prefix);
                if (count < 0 && errno == EINTR) { more = true; break; }
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (count <= 0) {
                    if (count == 0 && !(revents & (POLLHUP | POLLERR)))
                        close_lost_device(input, device, monotonic_now_ms());
                    break;
                }
                size_t total = prefix + (size_t) count;
                size_t complete = total / SELECTOR_EVDEV_RECORD_SIZE;
                size_t trailing = total % SELECTOR_EVDEV_RECORD_SIZE;
                device->partial_size = trailing;
                if (trailing) memcpy(device->partial_record,
                                     bytes + complete * SELECTOR_EVDEV_RECORD_SIZE, trailing);
                for (size_t n = 0; n < complete; n++) {
                    selector_evdev_record_t record;
                    memcpy(&record, bytes + n * SELECTOR_EVDEV_RECORD_SIZE, sizeof(record));
                    process_record(input, device, record.type, record.code, record.value, monotonic_now_ms());
                    device_records++;
                    records++;
                    if (records >= SELECTOR_INPUT_MAX_BATCH) {
                        more = true;
                        break;
                    }
                }
                if (records >= SELECTOR_INPUT_MAX_BATCH) break;
                if (complete == 0 && trailing == 0) break;
            }
            if (device_records >= SELECTOR_INPUT_READ_RECORDS) more = true;
        }
        if (device->live && (revents & (POLLERR | POLLHUP)))
            close_lost_device(input, device, monotonic_now_ms());
        if (more) break;
    }
    drain_queue(input, callback, userdata);
    size_t live = 0;
    for (size_t i = 0; i < input->device_count; i++) if (input->devices[i].live) live++;
    input->active_callback = NULL;
    input->active_userdata = NULL;
    if (live == 0) return -1;
    return more ? 1 : 0;
}

bool selector_input_inject_record(selector_input_t * input,
                                  selector_input_test_device_t kind,
                                  uint16_t type, uint16_t code, int32_t value,
                                  uint64_t monotonic_ms) {
    if (!input || !input->test_mode ||
        (kind != SELECTOR_INPUT_TEST_KEYS && kind != SELECTOR_INPUT_TEST_TOUCH)) return false;
    selector_input_device_t * device = get_test_device(input,
        kind == SELECTOR_INPUT_TEST_KEYS ? DEVICE_KEYS : DEVICE_TOUCH);
    if (!device) return false;
    process_record(input, device, type, code, value, monotonic_ms);
    return true;
}

bool selector_input_inject_key(selector_input_t * input, unsigned int key,
                               int value, uint64_t monotonic_ms,
                               bool startup_held) {
    if (!input || key_kind(key) < 0 || !input->test_mode) return false;
    selector_input_device_t * device = get_test_device(input, DEVICE_KEYS);
    if (!device) return false;
    feed_key(input, device, key, value, monotonic_ms, startup_held);
    return true;
}

bool selector_input_inject_touch(selector_input_t * input, int raw_x, int raw_y,
                                 unsigned int contacts, bool changed,
                                 uint64_t monotonic_ms, bool startup_held) {
    if (!input || contacts > SELECTOR_INPUT_MAX_SLOTS || !input->test_mode) return false;
    selector_input_device_t * device = get_test_device(input, DEVICE_TOUCH);
    if (!device) return false;
    inject_touch_to_device(input, device, raw_x, raw_y, contacts, changed,
                           monotonic_ms, startup_held);
    return true;
}

bool selector_input_set_test_touch_range(selector_input_t * input,
                                         int x_min, int x_max,
                                         int y_min, int y_max) {
    if (!input || !input->test_mode || x_max <= x_min || y_max <= y_min) return false;
    input->test_x_min = x_min;
    input->test_x_max = x_max;
    input->test_y_min = y_min;
    input->test_y_max = y_max;
    for (size_t i = 0; i < input->device_count; i++) {
        selector_input_device_t * device = &input->devices[i];
        if (!device->synthetic || device->kind != DEVICE_TOUCH) continue;
        device->x_min = x_min;
        device->x_max = x_max;
        device->y_min = y_min;
        device->y_max = y_max;
    }
    return true;
}

void selector_input_inject_syn_dropped(selector_input_t * input,
                                       uint64_t monotonic_ms) {
    if (!input || !input->test_mode) return;
    bool canceled = false;
    for (size_t i = 0; i < input->device_count; i++) {
        selector_input_device_t * device = &input->devices[i];
        if (!device->live || !device->synthetic) continue;
        for (int kind = 0; kind < KEY_KIND_COUNT; kind++)
            if (device->key_down[kind]) device->key_ignore_until_release[kind] = true;
        bool had_touch = device->touch_down || active_contacts(device) > 0;
        cancel_device(input, device, monotonic_ms);
        device->ignore_touch_until_up = had_touch;
        canceled = true;
    }
    if (!canceled) queue_event(input, SELECTOR_INPUT_CANCEL, 0, 0, 0, monotonic_ms);
}
