#define _GNU_SOURCE
#include "selector_controller.h"
#include "selector_request.h"
#include "../render/selector_ui.h"
#include "selector_input.h"
#include "../../../app/compas_loader.h"
#include "../../../display/fb_draw.h"
#include "../../../scan/scanner.h"
#include "../../../storage/sd_ready.h"
#include "../../../../core/storage/sd_fsck_run.h"
#include "../../../../core/process/subprocess.h"
#include "../../../../hardware/usb/usb_mode_control.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum screen { SCREEN_SELECTOR, SCREEN_TOOLS, SCREEN_SCRIPTS, SCREEN_NOTICE, SCREEN_CONFIRM_REPAIR };
struct selector {
    enum screen screen;
    int selected, scroll, pending;
    bool rockbox, cancelled, dirty, done, choose_rockbox, contact, dragged;
    int touch_x, touch_y, last_y, hit;
    uint64_t deadline;
    selector_input_t *input;
    char message[160];
    char scripts[SELECTOR_UI_MAX_LIST_ITEMS][NAME_MAX + 1];
    const char *script_labels[SELECTOR_UI_MAX_LIST_ITEMS];
    size_t script_count;
};

static uint64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static bool read_nonempty(const char *path, char *out, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, out, capacity - 1);
    close(fd);
    if (n <= 0) return false;
    out[n] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return out[0] && strcmp(out, "none");
}
static bool adb_active(void) {
    char value[160];
    if (read_nonempty("/sys/kernel/config/usb_gadget/adb_demo/UDC", value, sizeof(value))) return true;
    char functions[128];
    return read_nonempty("/sys/class/android_usb/android0/functions", functions, sizeof(functions)) &&
           strstr(functions, "adb") &&
           read_nonempty("/sys/class/android_usb/android0/enable", value, sizeof(value)) && !strcmp(value, "1");
}
/* Developer entry should retain the user's explicit ADB opt-in, which is
 * otherwise restored only after the player starts. Never enable it for an
 * ordinary boot or for a saved Storage/DAC preference. */
static bool saved_adb_requested(void) {
    FILE *file = fopen("/usr/data/.compas/settings.txt", "r");
    if (!file) return false;
    char line[256];
    bool requested = false;
    for (unsigned lines = 0; lines < 512 && fgets(line, sizeof(line), file); lines++) {
        if (!strchr(line, '\n') && !feof(file)) {
            int c;
            do { c = fgetc(file); } while (c != '\n' && c != EOF);
            continue;
        }
        if (strncmp(line, "usb_mode=", 9)) continue;
        char *end;
        errno = 0;
        long mode = strtol(line + 9, &end, 10);
        while (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t') end++;
        requested = errno == 0 && end != line + 9 && *end == 0 && mode == USB_MODE_ADB;
        break;
    }
    fclose(file);
    return requested;
}
static bool set_adb(bool enabled) {
    char *argv[] = { "/etc/init.d/T90adb", enabled ? "start" : "stop", NULL };
    int code = -1;
    if (!subprocess_run_checked_group(argv, NULL, 0, 15000, &code) || code) return false;
    /* The stock service starts adbd asynchronously; wait for actual binding
     * rather than reporting failure immediately after its shell exits. */
    uint64_t deadline = now_ms() + 2000;
    do {
        if (adb_active() == enabled) return true;
        usleep(20000);
    } while (now_ms() < deadline);
    return false;
}
static void present(void) {
    size_t stride;
    const uint16_t *pixels = selector_ui_pixels(&stride);
    if (fb_draw_blit_rgb565(pixels, FB_WIDTH, FB_HEIGHT, stride)) fb_flush();
}
static void notice(struct selector *s, const char *message) {
    snprintf(s->message, sizeof(s->message), "%s", message);
    s->screen = SCREEN_NOTICE; s->selected = 0; s->scroll = 0; s->dirty = true;
}
static void progress(const char *title, const char *message) {
    selector_ui_draw_progress(title, message, 0); present();
}
static int count(const struct selector *s) {
    if (s->screen == SCREEN_SELECTOR) return s->rockbox ? 3 : 2;
    if (s->screen == SCREEN_TOOLS) return 7;
    if (s->screen == SCREEN_SCRIPTS) return (int)s->script_count;
    return 1;
}
static int hit(const struct selector *s, int x, int y) {
    if (s->screen == SCREEN_SELECTOR) return selector_ui_hit_test_selector_available(x, y, s->rockbox);
    if (s->screen == SCREEN_TOOLS) return selector_ui_hit_test_tools(x, y, s->scroll);
    if (s->screen == SCREEN_SCRIPTS)
        return selector_ui_hit_test_list(x, y, s->script_count, s->scroll);
    return selector_ui_hit_test_list(x, y, 1, 0);
}
static int max_scroll(const struct selector *s) {
    if (s->screen == SCREEN_TOOLS) return selector_ui_tools_max_scroll();
    if (s->screen == SCREEN_SCRIPTS) return selector_ui_list_max_scroll(s->script_count);
    return 0;
}
static void clamp_scroll(struct selector *s) {
    int maximum = max_scroll(s);
    if (s->scroll < 0) s->scroll = 0;
    if (s->scroll > maximum) s->scroll = maximum;
}
static void back(struct selector *s) {
    bool from_tools = s->screen == SCREEN_TOOLS;
    s->screen = from_tools ? SCREEN_SELECTOR : SCREEN_TOOLS;
    s->selected = from_tools ? (s->rockbox ? 2 : 1) : 0; s->scroll = 0; s->dirty = true;
}
static void activate(struct selector *s, int item) {
    if (item == SELECTOR_UI_HIT_BACK) { back(s); return; }
    if (item < 0 || item >= count(s)) return;
    s->selected = item; s->cancelled = true; s->dirty = true;
    if (s->screen == SCREEN_SELECTOR) {
        if (item == 0 || (s->rockbox && item == 1)) {
            s->done = true; s->choose_rockbox = item == 1;
        } else { s->screen = SCREEN_TOOLS; s->selected = 0; s->scroll = 0; }
    } else if (s->screen == SCREEN_NOTICE) back(s);
    else if (s->screen == SCREEN_CONFIRM_REPAIR) s->pending = 8;
    else s->pending = s->screen == SCREEN_SCRIPTS ? 100 + item : item + 1;
}
static void input_event(const selector_input_event_t *event, void *context) {
    struct selector *s = context;
    if (s->done || s->pending) return;
    /* Any intentional interaction irrevocably stops this boot's countdown. */
    s->cancelled = true; s->dirty = true;
    if (event->type == SELECTOR_INPUT_CANCEL) { s->contact = false; return; }
    if (event->type == SELECTOR_INPUT_NAV_UP || event->type == SELECTOR_INPUT_NAV_DOWN) {
        s->contact = false;
        int total = count(s);
        if (!total && s->screen != SCREEN_SELECTOR) s->selected = SELECTOR_UI_HIT_BACK;
        if (total) {
            s->selected += event->type == SELECTOR_INPUT_NAV_UP ? -1 : 1;
            /* Header Back participates in physical navigation on submenus. */
            int minimum = s->screen == SCREEN_SELECTOR ? 0 : SELECTOR_UI_HIT_BACK;
            if (s->selected == -1) s->selected += event->type == SELECTOR_INPUT_NAV_UP ? -1 : 1;
            if (s->selected < minimum) s->selected = total - 1;
            if (s->selected >= total) s->selected = minimum;
            if (s->screen == SCREEN_TOOLS)
                s->scroll = selector_ui_ensure_visible_tools(s->selected, s->scroll);
            else if (s->screen == SCREEN_SCRIPTS)
                s->scroll = selector_ui_ensure_visible_list(s->script_count, s->selected, s->scroll);
        }
    } else if (event->type == SELECTOR_INPUT_ACTIVATE) activate(s, s->selected);
    else if (event->type == SELECTOR_INPUT_TOUCH_DOWN) {
        s->contact = true; s->dragged = false;
        s->touch_x = event->x; s->touch_y = s->last_y = event->y;
        s->hit = hit(s, event->x, event->y);
    } else if (event->type == SELECTOR_INPUT_TOUCH_MOVE && s->contact) {
        if (abs(event->x - s->touch_x) > 12 || abs(event->y - s->touch_y) > 12) s->dragged = true;
        if (s->dragged) { s->scroll += s->last_y - event->y; clamp_scroll(s); }
        s->last_y = event->y;
    } else if (event->type == SELECTOR_INPUT_TOUCH_UP && s->contact) {
        s->contact = false;
        if (!s->dragged && abs(event->x - s->touch_x) <= 12 &&
            abs(event->y - s->touch_y) <= 12 && hit(s, event->x, event->y) == s->hit) activate(s, s->hit);
    }
}
static void draw(struct selector *s) {
    if (s->screen == SCREEN_SELECTOR) {
        uint64_t now = now_ms();
        unsigned remaining = now < s->deadline ? (unsigned)((s->deadline - now + 999) / 1000) : 0;
        selector_ui_draw_selector(s->selected, s->rockbox, remaining, s->cancelled, adb_active());
    } else if (s->screen == SCREEN_TOOLS) selector_ui_draw_tools(s->selected, s->scroll, adb_active());
    else if (s->screen == SCREEN_SCRIPTS)
        selector_ui_draw_list("Run script", s->script_labels, s->script_count, s->selected, s->scroll,
                              s->script_count ? NULL : "No .sh scripts in SD/");
    else if (s->screen == SCREEN_CONFIRM_REPAIR) {
        const char *items[] = { "Repair SD card" };
        selector_ui_draw_list("Repair SD card", items, 1, s->selected, 0,
                              "Checks and repairs files. Keep power connected.");
    } else {
        const char *items[] = { "Back" };
        selector_ui_draw_list("Tools", items, 1, s->selected, 0, s->message);
    }
    present(); s->dirty = false;
}
static int compare_names(const void *a, const void *b) { return strcmp(a, b); }
static void scripts_load(struct selector *s) {
    s->script_count = 0;
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) && s->script_count < SELECTOR_UI_MAX_LIST_ITEMS) {
            size_t length = strlen(entry->d_name);
            struct stat st;
            if (length < 4 || strcmp(entry->d_name + length - 3, ".sh") ||
                fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
                !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 1024 * 1024) continue;
            snprintf(s->scripts[s->script_count++], NAME_MAX + 1, "%s", entry->d_name);
        }
        closedir(dir);
    }
    qsort(s->scripts, s->script_count, sizeof(s->scripts[0]), compare_names);
    for (size_t i = 0; i < s->script_count; i++) s->script_labels[i] = s->scripts[i];
    s->screen = SCREEN_SCRIPTS; s->selected = s->script_count ? 0 : SELECTOR_UI_HIT_BACK; s->scroll = 0; s->dirty = true;
}
/* The chosen file is pinned and passed as shell stdin, never interpolated
 * into command text or reopened by pathname. All inherited SD handles close. */
static bool script_run(const char *name) {
    int dir = open(SD_MOUNT_POINT, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return false;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    close(dir);
    struct stat st;
    if (fd < 0) return false;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 1024 * 1024) {
        close(fd); return false;
    }
    pid_t child = fork();
    if (!child) {
        (void)setpgid(0, 0);
        if (dup2(fd, STDIN_FILENO) < 0 || chdir("/")) _exit(126);
        int log = open("/usr/data/selector-script.log", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_NONBLOCK, 0600);
        struct stat log_stat;
        if (log < 0 || fstat(log, &log_stat) || !S_ISREG(log_stat.st_mode)) _exit(126);
        if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) _exit(126);
        long limit = sysconf(_SC_OPEN_MAX);
        if (limit < 0 || limit > 65536) limit = 65536;
        for (int i = 3; i < limit; i++) close(i);
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    close(fd);
    if (child < 0) return false;
    (void)setpgid(child, child);
    uint64_t deadline = now_ms() + 60000;
    int status;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            /* Scripts are bounded menu jobs. A shell's successful exit
             * cannot leave background writers racing a later SD repair. */
            (void)kill(-child, SIGTERM);
            if (kill(-child, 0) == 0) {
                usleep(200000);
                (void)kill(-child, SIGKILL);
            }
            return success;
        }
        if (result < 0 && errno != EINTR) return false;
        if (now_ms() >= deadline) {
            (void)kill(-child, SIGTERM); usleep(200000); (void)kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return false;
        }
        usleep(20000);
    }
}
static void repair(struct selector *s) {
    progress("Repair SD card", "Checking the SD card. Do not disconnect power.");
    sd_repair_kick_result_t kicked = sd_manual_repair_kick(NULL);
    if (kicked.started || sd_repair_needs_remount()) {
        /* Never kill/time-limit a filesystem checker. Ignore all actions
         * until it finishes and the normal mount helper has returned. */
        while (!sd_repair_needs_remount()) usleep(100000);
        mount_sd_card_if_needed();
        bool readonly = true;
        bool mounted = sd_repair_current_readonly(&readonly);
        sd_repair_complete(mounted, readonly);
    }
    sd_repair_note_t note = sd_repair_take_note();
    if (note == SD_REPAIR_NOTE_REPAIRED) notice(s, "SD card checked and mounted successfully.");
    else if (note == SD_REPAIR_NOTE_NEEDS_COMPUTER || note == SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER)
        notice(s, "This card needs a computer check. No repair was started.");
    else notice(s, "SD card repair could not finish. Check the card on a computer.");
}
static bool remount_card(void) {
    bool readonly;
    if (sd_repair_current_readonly(&readonly)) {
        sync();
        if (umount(SD_MOUNT_POINT)) return false;
        if (sd_repair_current_readonly(&readonly)) return false;
    }
    mount_sd_card_if_needed();
    return sd_repair_current_readonly(&readonly) && !readonly;
}
static void action(struct selector *s) {
    int pending = s->pending; s->pending = 0;
    if (pending == 2) { scripts_load(s); return; }
    if (pending == 4) { s->screen = SCREEN_CONFIRM_REPAIR; s->selected = 0; s->dirty = true; return; }
    /* Close/reopen input around blocking tools so queued repeats/releases
     * cannot activate another command after the tool returns. */
    selector_input_close(s->input);
    s->contact = false;
    if (pending == 1) {
        bool before = adb_active();
        progress("ADB", before ? "Stopping ADB…" : "Starting ADB…");
        if (set_adb(!before)) { s->screen = SCREEN_TOOLS; s->selected = 0; s->dirty = true; }
        else notice(s, "ADB did not change state. Check Developer Options.");
    } else if (pending == 3) {
        progress("Remount SD", "Mounting the SD card…");
        notice(s, remount_card() ? "SD card mounted successfully." : "SD card could not be remounted.");
    } else if (pending == 8) repair(s);
    else if (pending >= 100 && (size_t)(pending - 100) < s->script_count) {
        progress("Run script", "Running the selected script…");
        notice(s, script_run(s->scripts[pending - 100]) ? "Script finished." : "Script failed or timed out.");
    } else if (pending == 5 || pending == 6 || pending == 7) {
        if (pending == 7) {
            char *argv[] = { "/usr/bin/bootmode.sh", "Recovery", NULL };
            int code;
            if (!subprocess_run_checked_group(argv, NULL, 0, 5000, &code) || code) {
                notice(s, "Could not select recovery mode.");
                goto reopen;
            }
        }
        selector_ui_shutdown(); fb_close();
        if (pending == 6) compas_loader_poweroff();
        else compas_loader_restart();
    }
reopen:
    if (!selector_input_open(s->input)) s->done = true;
}

bool selector_controller_run(bool rockbox_available) {
    bool forced = selector_request_consume();
    if (!forced && !rockbox_available) return false;
    struct selector *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->rockbox = rockbox_available; s->cancelled = forced; s->dirty = true;
    if (!selector_ui_init(FB_WIDTH, FB_HEIGHT, NULL, NULL) || !fb_open()) goto finished;
    if (forced && !adb_active() && saved_adb_requested()) {
        progress("ADB", "Restoring Developer Options ADB…");
        (void)set_adb(true);
    }
    s->input = selector_input_create(FB_WIDTH, FB_HEIGHT);
    if (!s->input || !selector_input_open(s->input)) goto finished;
    /* Same panel brightness as the player's normal boot, before any
     * Rockbox backlight worker exists. Failure leaves the current level. */
    char maximum[32];
    if (read_nonempty("/sys/class/backlight/backlight_pwm0/max_brightness", maximum, sizeof(maximum))) {
        char *end;
        long max = strtol(maximum, &end, 10);
        if (*end == 0 && max > 0 && max <= 65535) {
            int light = open("/sys/class/backlight/backlight_pwm0/brightness", O_WRONLY | O_CLOEXEC);
            if (light >= 0) {
                char level[32];
                int length = snprintf(level, sizeof(level), "%ld\n", (max * 4 + 4) / 5);
                (void)write(light, level, (size_t)length);
                close(light);
            }
        }
    }
    s->deadline = now_ms() + 3000;
    draw(s);
    unsigned shown_seconds = 3;
    while (!s->done) {
        int polled = selector_input_poll(s->input, 50, input_event, s);
        if (polled < 0) break;
        if (s->pending) action(s);
        /* A bounded drain that still has pending input cannot auto-boot. */
        if (!polled && !s->cancelled && now_ms() >= s->deadline) s->done = true;
        uint64_t now = now_ms();
        unsigned seconds = now < s->deadline ? (unsigned)((s->deadline - now + 999) / 1000) : 0;
        if (!s->cancelled && seconds != shown_seconds) s->dirty = true;
        shown_seconds = seconds;
        if (s->dirty && !s->done) draw(s);
    }
finished:
    /* Selection is complete. Do not leave actionable menu artwork visible
     * while the player is already starting and opening its own input fds. */
    if (selector_ui_pixels(NULL)) {
        if (s->choose_rockbox) {
            selector_ui_draw_starting(true);
            present();
        } else {
            if (!fb_draw_startup_background())
                fb_fill(fb_rgb(0x12, 0x12, 0x12));
            fb_flush();
        }
    }
    selector_input_destroy(s->input);
    bool result = s->choose_rockbox;
    free(s); selector_ui_shutdown(); fb_close();
    return result;
}
