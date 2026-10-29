#include "gui_about_info.h"
#include "../appearance/brand_marks.h"
#include "../appearance/gui_theme.h"
#include "../text/i18n/i18n.h"
#include "../../core/system/runtime/app_version.h"
#include "../../core/system/runtime/board_config.h"
#include "../../hardware/indicators/backlight.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_muted;

#define ABOUT_REFRESH_MS 2000
#define INTERNAL_PATH "/usr/data"
#define SD_PATH "/usr/data/mnt/sd_0"

enum { ROW_MEMORY, ROW_INTERNAL, ROW_SD, ROW_COUNT };

/* UI-thread state. */
static lv_obj_t * about_scr;
static lv_obj_t * logo;
static lv_obj_t * row_obj[ROW_COUNT];
static lv_obj_t * value_label[ROW_COUNT];
static lv_timer_t * refresh_timer;

/* Worker: fills `result`, then publishes with `done`. The UI thread starts it
 * only when no earlier one is still running and never waits for a stuck one. */
typedef struct {
    bool ok[ROW_COUNT];
    uint64_t free_bytes[ROW_COUNT];
    uint64_t total_bytes[ROW_COUNT];
} usage_t;
static pthread_t worker;
static bool worker_active;
static atomic_bool worker_done;
static usage_t result;

/* "18.9 MB", "119 GB": one decimal below 100, none from 100 up. */
static void format_size(char * out, size_t cap, uint64_t bytes) {
    static const char * const units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double) bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0 || v >= 99.95) snprintf(out, cap, "%.0f %s", v, units[u]);
    else snprintf(out, cap, "%.1f %s", v, units[u]);
}

static bool read_fs(const char * path, uint64_t * free_b, uint64_t * total_b) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0 || vfs.f_blocks == 0) return false;
    *free_b = (uint64_t) vfs.f_bavail * vfs.f_frsize;
    *total_b = (uint64_t) vfs.f_blocks * vfs.f_frsize;
    return true;
}

static bool read_meminfo(uint64_t * free_b, uint64_t * total_b) {
    FILE * f = fopen("/proc/meminfo", "r");
    if (!f) return false;
    char line[128];
    unsigned long long avail = 0, total = 0;
    bool have_avail = false, have_total = false;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %llu", &avail) == 1) have_avail = true;
        else if (sscanf(line, "MemTotal: %llu", &total) == 1) have_total = true;
    }
    fclose(f);
    if (!have_avail || !have_total || total == 0) return false;
    *free_b = avail * 1024ULL;
    *total_b = total * 1024ULL;
    return true;
}

/* The card counts as present only when something is mounted at its mount
 * point, so an empty slot never makes statvfs() describe the root filesystem. */
static bool sd_mounted(void) {
    FILE * f = fopen("/proc/mounts", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char dev[256], dir[256];
        if (sscanf(line, "%255s %255s", dev, dir) == 2 && strcmp(dir, SD_PATH) == 0) found = true;
    }
    fclose(f);
    return found;
}

static void * worker_main(void * arg) {
    (void) arg;
    usage_t r;
    memset(&r, 0, sizeof(r));
    r.ok[ROW_MEMORY] = read_meminfo(&r.free_bytes[ROW_MEMORY], &r.total_bytes[ROW_MEMORY]);
    r.ok[ROW_INTERNAL] = read_fs(INTERNAL_PATH, &r.free_bytes[ROW_INTERNAL], &r.total_bytes[ROW_INTERNAL]);
    r.ok[ROW_SD] = sd_mounted() && read_fs(SD_PATH, &r.free_bytes[ROW_SD], &r.total_bytes[ROW_SD]);
    result = r;
    atomic_store_explicit(&worker_done, true, memory_order_release);
    return NULL;
}

static void apply_result(void) {
    for (int i = 0; i < ROW_COUNT; i++) {
        if (!row_obj[i]) continue;
        if (i == ROW_SD) {
            /* No card: the line is hidden rather than showing a placeholder. */
            if (result.ok[i]) lv_obj_remove_flag(row_obj[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(row_obj[i], LV_OBJ_FLAG_HIDDEN);
        }
        char a[24], b[24], text[80];
        if (result.ok[i]) {
            format_size(a, sizeof(a), result.free_bytes[i]);
            format_size(b, sizeof(b), result.total_bytes[i]);
            snprintf(text, sizeof(text), TR("%s free of %s"), a, b);
        } else {
            snprintf(text, sizeof(text), "-");
        }
        lv_label_set_text(value_label[i], text);
    }
}

static bool screen_shown(void) {
    return about_scr && lv_screen_active() == about_scr && backlight_screen_is_on();
}

static void start_worker(void) {
    if (worker_active) return;
    atomic_store_explicit(&worker_done, false, memory_order_relaxed);
    if (pthread_create(&worker, NULL, worker_main, NULL) == 0) worker_active = true;
}

static void refresh_cb(lv_timer_t * timer) {
    if (worker_active) {
        if (!atomic_load_explicit(&worker_done, memory_order_acquire)) return;
        pthread_join(worker, NULL);
        worker_active = false;
        if (about_scr) apply_result();
    }
    if (!screen_shown()) {
        lv_timer_pause(timer);
        return;
    }
    start_worker();
}

static void screen_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) {
        if (logo && value_label[0]) {
            lv_obj_set_style_image_recolor(logo, lv_obj_get_style_text_color(value_label[0], LV_PART_MAIN), 0);
        }
        if (refresh_timer) {
            lv_timer_reset(refresh_timer);
            lv_timer_resume(refresh_timer);
        }
        start_worker();
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        /* The timer reaps a worker that is still running, then pauses itself. */
        if (refresh_timer && !worker_active) lv_timer_pause(refresh_timer);
    } else if (code == LV_EVENT_DELETE) {
        about_scr = NULL;
        logo = NULL;
        memset(row_obj, 0, sizeof(row_obj));
        memset(value_label, 0, sizeof(value_label));
        /* refresh_timer lives on: it reaps an unfinished worker, then idles. */
    }
}

static lv_obj_t * add_line(lv_obj_t * parent, const char * name, const char * value, lv_obj_t ** value_out) {
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_BODY);
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, BOARD_SCALE_PX(12), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * key = lv_label_create(row);
    lv_label_set_text(key, name);
    lv_obj_set_width(key, lv_pct(30));
    lv_label_set_long_mode(key, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(key, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(key, font, 0);

    lv_obj_t * val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_flex_grow(val, 1);
    lv_label_set_long_mode(val, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(val, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(val, font, 0);
    if (value_out) *value_out = val;
    return row;
}

void gui_about_info_attach(lv_obj_t * scr, lv_obj_t * list) {
    if (!scr || !list) return;
    about_scr = scr;

    /* The wordmark: 260 px on a 480 px panel, scaled with the panel width. */
    int32_t w = BOARD_SCALE_PX(260);
    int32_t h = (w * brand_wordmark.header.h + brand_wordmark.header.w / 2) / brand_wordmark.header.w;
    lv_obj_t * holder = lv_obj_create(list);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, lv_pct(100), h + BOARD_SCALE_PX(40));
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    logo = lv_image_create(holder);
    lv_image_set_src(logo, &brand_wordmark);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_image_set_scale(logo, (uint32_t) (LV_SCALE_NONE * w / brand_wordmark.header.w));
    lv_obj_set_size(logo, w, h);
    lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * info = lv_obj_create(list);
    lv_obj_remove_style_all(info);
    lv_obj_set_width(info, lv_pct(90));
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, BOARD_SCALE_PX(10), 0);
    lv_obj_set_style_pad_bottom(info, BOARD_SCALE_PX(24), 0);
    lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    add_line(info, TR("Version"), app_version_label(), NULL);
    add_line(info, TR_C("device info", "Build"), app_build_identifier(), NULL);
    add_line(info, TR("Device"), app_board_display_name(), NULL);
    row_obj[ROW_MEMORY] = add_line(info, TR("Memory"), "-", &value_label[ROW_MEMORY]);
    row_obj[ROW_INTERNAL] = add_line(info, TR("Internal"), "-", &value_label[ROW_INTERNAL]);
    row_obj[ROW_SD] = add_line(info, TR("SD card"), "-", &value_label[ROW_SD]);
    lv_obj_add_flag(row_obj[ROW_SD], LV_OBJ_FLAG_HIDDEN);

    lv_obj_move_to_index(holder, 0);
    lv_obj_move_to_index(info, 1);

    if (!refresh_timer) {
        refresh_timer = lv_timer_create(refresh_cb, ABOUT_REFRESH_MS, NULL);
        if (refresh_timer) lv_timer_pause(refresh_timer);
    }
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_DELETE, NULL);
}
