#include "backlight.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define BACKLIGHT_CLASS_DIR "/sys/class/backlight"

/* Same node power_suspend.c already blanks/unblanks around full suspend
 * (confirmed working there on real hardware) -- reused here for a plain
 * screen-timeout off too, instead of just zeroing PWM brightness. Blanking
 * the framebuffer powers down the panel driver itself (and, on this
 * hardware, the touch controller sharing its power rail) rather than
 * leaving the panel/touch powered with the backlight LED merely dimmed to
 * nothing, so it saves more power. Touch is never relied on to wake the
 * screen (see gui.c's own comment: only the power button does), so losing
 * touch while off is expected, not a regression. */
#define SYSFS_FB_BLANK "/sys/class/graphics/fb0/blank"

/* Doesn't hardcode "backlight_pwm0" -- same reasoning as battery.c's dynamic
 * power_supply scan, in case a different unit/firmware names its backlight
 * device differently. Unlike power_supply, everything under this class
 * genuinely is a backlight, so no type filtering is needed -- first entry
 * found wins. Returns false (leaving `out` untouched) if the class has no
 * entries at all, normal on host. */
static bool find_backlight_device(char * out, size_t out_size) {
    static char cached_device[64];
    if (cached_device[0]) {
        snprintf(out, out_size, "%s", cached_device);
        return true;
    }
    DIR * dir = opendir(BACKLIGHT_CLASS_DIR);
    if (!dir) return false;

    bool found = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        size_t len = strnlen(entry->d_name, sizeof(cached_device));
        if (len >= sizeof(cached_device) || len >= out_size) continue;
        memcpy(out, entry->d_name, len + 1);
        memcpy(cached_device, entry->d_name, len + 1);
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

static int read_int_attr(const char * device_name, const char * attr) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/%s", BACKLIGHT_CLASS_DIR, device_name, attr);

    FILE * f = fopen(path, "r");
    if (!f) return -1;
    int value;
    bool ok = fscanf(f, "%d", &value) == 1;
    fclose(f);
    return ok ? value : -1;
}

/* max_brightness is a fixed device property; cache its first valid value. */
static int brightness_fd = -1;
static int last_written_raw = -1;
static pthread_mutex_t backlight_io_mutex = PTHREAD_MUTEX_INITIALIZER;

static int get_max_brightness(const char * device_name) {
    static int cached_max = 0;
    if (cached_max <= 0) cached_max = read_int_attr(device_name, "max_brightness");
    return cached_max;
}

/* Converts logical percent (0-100) directly to/from the safe raw range
 * [BACKLIGHT_MIN_PERCENT, BACKLIGHT_SAFE_MAX_PERCENT] of max_brightness.
 * Capping below 100% of max_brightness prevents hardware PWM glitching at
 * 100% duty cycle, and direct mapping avoids rounding discrepancy on non-round
 * max_brightness values. */
#define BACKLIGHT_SAFE_MAX_PERCENT 99

static int logical_to_raw(int logical, int max) {
    if (logical < 0) logical = 0;
    if (logical > 100) logical = 100;
    int min_raw = (int) (((long) BACKLIGHT_MIN_PERCENT * max) / 100);
    int max_raw = (int) (((long) BACKLIGHT_SAFE_MAX_PERCENT * max) / 100);
    return min_raw + (int) (((long) (max_raw - min_raw) * logical) / 100);
}

static int raw_to_logical(int raw, int max) {
    int min_raw = (int) (((long) BACKLIGHT_MIN_PERCENT * max) / 100);
    int max_raw = (int) (((long) BACKLIGHT_SAFE_MAX_PERCENT * max) / 100);
    if (raw <= min_raw) return 0;
    if (raw >= max_raw) return 100;
    return (int) (((long) (raw - min_raw) * 100) / (max_raw - min_raw));
}

int backlight_get_percent(void) {
    pthread_mutex_lock(&backlight_io_mutex);
    char device_name[64];
    if (!find_backlight_device(device_name, sizeof(device_name))) {
        pthread_mutex_unlock(&backlight_io_mutex);
        return -1;
    }

    int max = get_max_brightness(device_name);
    int cur = read_int_attr(device_name, "brightness");
    int result = (max > 0 && cur >= 0) ? raw_to_logical(cur, max) : -1;
    pthread_mutex_unlock(&backlight_io_mutex);
    return result;
}

static bool backlight_write_percent(int percent, bool lit) {
    bool written = false;
    pthread_mutex_lock(&backlight_io_mutex);
    char device_name[64];
    if (!find_backlight_device(device_name, sizeof(device_name))) { written = true; goto done; }

    int max = get_max_brightness(device_name);
    if (max <= 0) { written = true; goto done; }
    int value = lit ? logical_to_raw(percent, max) : 0;

    if (value == last_written_raw) { written = true; goto done; }

    if (brightness_fd < 0) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s/brightness", BACKLIGHT_CLASS_DIR, device_name);
        brightness_fd = open(path, O_WRONLY | O_CLOEXEC);
        if (brightness_fd < 0) goto done;
    }

    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", value);
    if (n <= 0) goto done;
    if (lseek(brightness_fd, 0, SEEK_SET) < 0) {
        close(brightness_fd);
        brightness_fd = -1;
        goto done;
    }
    if (write(brightness_fd, buf, (size_t) n) != n) {
        close(brightness_fd);
        brightness_fd = -1;
        goto done;
    }
    last_written_raw = value;
    written = true;

done:
    pthread_mutex_unlock(&backlight_io_mutex);
    return written;
}

/* Guards requested/applied screen state and restored brightness across
 * the GUI and the process-lifetime hardware worker. */
static pthread_mutex_t screen_power_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool screen_on = true;
static bool screen_visible = true;
static bool screen_preparing;
static bool screen_prepared;
static uint64_t screen_power_generation;
static bool screen_dimmed = false;
/* Restored on the next turn-on -- seeded to a sensible default matching the
 * old hw_buttons.c hardcoded BACKLIGHT_ON_LEVEL, only actually used if we
 * never captured a real prior brightness (e.g. backlight_get_percent()
 * failed the first time we turned the screen off). Logical percent, same
 * scale as backlight_get_percent()'s own return value. */
static int restore_percent = 80;
static bool restore_percent_set = false;

static pthread_once_t brightness_worker_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t brightness_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t brightness_worker_cond = PTHREAD_COND_INITIALIZER;
static bool brightness_worker_ready = false;
static bool brightness_worker_pending = false;

/* fb0/blank is a distinct sysfs node from anything backlight_io_mutex
 * protects (brightness_fd, last_written_raw, the cached device name/max) --
 * no lock needed here. fb_blanked tracks the last value written so a run of
 * on/off or dim/undim requests (e.g. a live brightness-slider drag while
 * the screen is already on) doesn't re-blank or re-unblank redundantly. */
static bool fb_blanked = false;
static bool fb_blank_known;

static bool write_fb_blank(bool blank) {
    if (fb_blank_known && blank == fb_blanked) return true;
    FILE * f = fopen(SYSFS_FB_BLANK, "w");
    if (!f) return false;
    bool ok = fprintf(f, blank ? "4" : "0") == 1;
    if (fclose(f) != 0) ok = false;
    if (ok) { fb_blanked = blank; fb_blank_known = true; }
    return ok;
}

static void apply_requested_brightness(void) {
    pthread_mutex_lock(&screen_power_mutex);
    bool on = screen_on;
    bool preparing = screen_preparing;
    uint64_t generation = screen_power_generation;
    int normal = restore_percent;
    int target = screen_dimmed && normal > 5 ? 5 : normal;
    pthread_mutex_unlock(&screen_power_mutex);
    if (preparing) {
        /* A blanked panel rejects page flips. Unblank with the PWM truly off
         * so the GUI can present its selected wake surface before revealing it. */
#ifdef HOST_BUILD
        bool ready = true;
#else
        fb_blank_known = false; /* Suspend may have blanked outside this worker. */
        bool ready = backlight_write_percent(target, false) && write_fb_blank(false);
#endif
        pthread_mutex_lock(&screen_power_mutex);
        if (screen_preparing && !screen_on && screen_power_generation == generation) {
            screen_prepared = ready;
            if (!ready) screen_preparing = false; /* GUI can retry a failed write. */
        }
        pthread_mutex_unlock(&screen_power_mutex);
    } else if (on) {
#ifdef HOST_BUILD
        bool visible = true;
#else
        if (!write_fb_blank(false))
            fprintf(stderr, "Screen wake: panel unblank failed\n");
        bool visible = backlight_write_percent(target, true);
#endif
        pthread_mutex_lock(&screen_power_mutex);
        if (screen_on && !screen_preparing && screen_power_generation == generation)
            screen_visible = screen_visible || visible;
        pthread_mutex_unlock(&screen_power_mutex);
    } else {
        (void) write_fb_blank(true);
    }
}

static void * brightness_worker_main(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&brightness_worker_mutex);
        while (!brightness_worker_pending)
            pthread_cond_wait(&brightness_worker_cond, &brightness_worker_mutex);
        brightness_worker_pending = false;
        pthread_mutex_unlock(&brightness_worker_mutex);
        apply_requested_brightness();
    }
    return NULL;
}

static void start_brightness_worker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, brightness_worker_main, NULL) == 0) {
        pthread_detach(thread);
        brightness_worker_ready = true;
    }
}

static void request_brightness_apply(void) {
    pthread_once(&brightness_worker_once, start_brightness_worker);
    if (!brightness_worker_ready) {
        apply_requested_brightness();
        return;
    }
    pthread_mutex_lock(&brightness_worker_mutex);
    brightness_worker_pending = true;
    pthread_cond_signal(&brightness_worker_cond);
    pthread_mutex_unlock(&brightness_worker_mutex);
}

void backlight_set_normal_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pthread_mutex_lock(&screen_power_mutex);
    restore_percent = percent;
    restore_percent_set = true;
    screen_dimmed = false;
    bool write_now = screen_on;
    pthread_mutex_unlock(&screen_power_mutex);
    if (write_now) (void) backlight_write_percent(percent, true);
}

void backlight_request_normal_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pthread_mutex_lock(&screen_power_mutex);
    restore_percent = percent;
    restore_percent_set = true;
    screen_dimmed = false;
    pthread_mutex_unlock(&screen_power_mutex);
    request_brightness_apply();
}

void backlight_set_dimmed(bool dimmed) {
    pthread_mutex_lock(&screen_power_mutex);
    if (!screen_on || screen_dimmed == dimmed) {
        pthread_mutex_unlock(&screen_power_mutex);
        return;
    }
    screen_dimmed = dimmed;
    pthread_mutex_unlock(&screen_power_mutex);
    request_brightness_apply();
}

bool backlight_screen_is_on(void) {
    pthread_mutex_lock(&screen_power_mutex);
    bool result = screen_on;
    pthread_mutex_unlock(&screen_power_mutex);
    return result;
}

bool backlight_screen_is_visible(void) {
    pthread_mutex_lock(&screen_power_mutex);
    bool result = screen_on && screen_visible;
    pthread_mutex_unlock(&screen_power_mutex);
    return result;
}

void backlight_prepare_screen_on(void) {
    pthread_mutex_lock(&screen_power_mutex);
    if (screen_on || screen_preparing) {
        pthread_mutex_unlock(&screen_power_mutex);
        return;
    }
    screen_preparing = true;
    screen_visible = false;
    screen_prepared = false;
    screen_power_generation++;
    pthread_mutex_unlock(&screen_power_mutex);
    request_brightness_apply();
}

bool backlight_screen_is_prepared(void) {
    pthread_mutex_lock(&screen_power_mutex);
    bool ready = screen_preparing && screen_prepared && !screen_on;
    pthread_mutex_unlock(&screen_power_mutex);
    return ready;
}

void backlight_set_screen_on(bool on) {
    pthread_mutex_lock(&screen_power_mutex);
    if (on == screen_on && !screen_preparing) {
        pthread_mutex_unlock(&screen_power_mutex);
        if (on) request_brightness_apply(); /* Retry a failed reveal write. */
        return;
    }
    bool was_on = screen_on;
    screen_on = on;
    screen_visible = false;
    screen_preparing = screen_prepared = false;
    screen_power_generation++;

    if (!on) {
        /* Save current brightness level before screen turns off so it can be restored.
         * Any non-negative reading is valid (negative indicates read failure). */
        if (was_on && !screen_dimmed && !restore_percent_set) {
            int current = backlight_get_percent();
            if (current >= 0) {
                restore_percent = current;
                restore_percent_set = true;
            }
        }
        screen_dimmed = false;
    }
    pthread_mutex_unlock(&screen_power_mutex);
    request_brightness_apply();
}
