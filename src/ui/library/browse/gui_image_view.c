#include "gui_image_view.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../../library/artwork/cache/image_thumb.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_shell.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __linux__
  #include <sys/prctl.h>
#endif

#define IMAGE_MAX_BYTES (8u * 1024u * 1024u)
#define IMAGE_WORKER_STACK (4 * 1024 * 1024) /* same as the album cover decode worker */

/* One decode. The worker owns it until done is set; the UI thread joins the
 * worker and frees it. A cancelled job's pixels are never shown. */
typedef struct {
    atomic_bool done;
    atomic_bool cancel;
    pthread_t thread;
    unsigned id; /* the request it serves */
    char path[PATH_MAX + NAME_MAX + 2];
    int max_w, max_h;
    uint16_t * pixels;
    int w, h;
} image_job_t;

static lv_obj_t * screen;
static lv_obj_t * title_label;
static lv_obj_t * body;
static lv_obj_t * image;
static lv_obj_t * status_label;
static int32_t body_w, body_h; /* set once; the body is not laid out yet when the first decode starts */

static char dir[PATH_MAX];
static char ** names; /* the folder's images, in File Manager order */
static int name_count;
static int current;
static bool open_;

static lv_image_dsc_t dsc;
static uint16_t * shown_pixels;

static image_job_t * job;
static lv_timer_t * timer;
static unsigned wanted; /* bumped by each request; older results are dropped */

bool gui_image_view_is_image_name(const char * name) {
    static const char * const extensions[] = { ".jpg", ".jpeg", ".png", ".bmp" };
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
        if (strcasecmp(ext, extensions[i]) == 0) return true;
    return false;
}

static bool job_cancelled(void * user) {
    return atomic_load(&((image_job_t *) user)->cancel);
}

/* Weak: defined in main.c, absent from tests that link without it. */
extern void install_thread_crash_altstack(void) __attribute__((weak));

static void * image_thread(void * arg) {
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "imageview");
#endif
    if (install_thread_crash_altstack) install_thread_crash_altstack();
    image_job_t * j = arg;
    FILE * f = fopen(j->path, "rb");
    uint8_t * data = NULL;
    size_t len = 0;
    if (f && fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && (unsigned long) size <= IMAGE_MAX_BYTES && fseek(f, 0, SEEK_SET) == 0)
            data = malloc((size_t) size);
        while (data && len < (size_t) size && !job_cancelled(j)) {
            size_t want = (size_t) size - len < 65536 ? (size_t) size - len : 65536;
            size_t n = fread(data + len, 1, want, f);
            if (n == 0) break;
            len += n;
        }
        if (data && len != (size_t) size) len = 0;
    }
    if (f) fclose(f);
    int o = len ? image_exif_orientation(data, len) : 1;
    bool swap = o >= 5;
    if (len && !job_cancelled(j) &&
        image_thumb_decode(data, len, swap ? j->max_h : j->max_w, swap ? j->max_w : j->max_h,
                           ARTWORK_PRIO_PLAYER, job_cancelled, j, &j->pixels, &j->w, &j->h, NULL) &&
        o != 1) {
        int w = 0, h = 0;
        uint16_t * upright = image_rgb565_orient(j->pixels, j->w, j->h, o, &w, &h);
        free(j->pixels);
        j->pixels = upright; /* NULL on failure: shown as "Could not open this image" */
        j->w = w;
        j->h = h;
    }
    free(data);
    atomic_store(&j->done, true);
    return NULL;
}

/* The image no longer points at the buffer before it is freed. */
static void release_shown(void) {
    if (image) lv_image_set_src(image, NULL);
    lv_image_cache_drop(&dsc);
    free(shown_pixels);
    shown_pixels = NULL;
}

static void show_pixels(uint16_t * pixels, int w, int h) {
    release_shown();
    shown_pixels = pixels;
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = (uint32_t) w;
    dsc.header.h = (uint32_t) h;
    dsc.header.stride = (uint32_t) w * 2;
    dsc.data_size = (uint32_t) w * (uint32_t) h * 2;
    dsc.data = (const uint8_t *) pixels;
    lv_image_set_src(image, &dsc);
    lv_obj_center(image);
}

static void start_job(void);

static void job_timer_cb(lv_timer_t * t) {
    (void) t;
    if (job && !atomic_load(&job->done)) return;
    if (!job) { /* a follow-up decode could not start */
        lv_timer_delete(timer);
        timer = NULL;
        return;
    }
    pthread_join(job->thread, NULL);
    bool current_request = open_ && job->id == wanted;
    if (current_request && job->pixels) {
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        show_pixels(job->pixels, job->w, job->h);
    } else {
        free(job->pixels);
        if (current_request) lv_label_set_text(status_label, TR("Could not open this image"));
    }
    free(job);
    job = NULL;
    if (open_ && !current_request) {
        start_job(); /* the user moved on while this one ran */
    } else {
        lv_timer_delete(timer);
        timer = NULL;
    }
}

static void start_job(void) {
    job = calloc(1, sizeof(*job));
    pthread_attr_t attr;
    bool attr_ok = pthread_attr_init(&attr) == 0;
    if (job) {
        job->id = wanted;
        snprintf(job->path, sizeof(job->path), "%s/%s", dir, names[current]);
        job->max_w = body_w;
        job->max_h = body_h;
        if (attr_ok) pthread_attr_setstacksize(&attr, IMAGE_WORKER_STACK);
        if (pthread_create(&job->thread, attr_ok ? &attr : NULL, image_thread, job) != 0) {
            free(job);
            job = NULL;
        }
    }
    if (attr_ok) pthread_attr_destroy(&attr);
    if (job) {
        if (!timer) timer = lv_timer_create(job_timer_cb, 30, NULL);
    } else {
        lv_label_set_text(status_label, TR("Could not open this image"));
    }
}

/* Shows names[index]: a decode already running is cancelled and the new one
 * starts when it has ended. */
static void request_image(int index) {
    current = index;
    wanted++;
    char title[PATH_MAX + 32];
    snprintf(title, sizeof(title), "%s (%d / %d)", names[index], index + 1, name_count);
    lv_label_set_text(title_label, title);
    screen_title_fit(title_label);
    release_shown();
    lv_label_set_text(status_label, TR("Loading..."));
    lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    if (job) atomic_store(&job->cancel, true);
    else start_job();
}

static void free_names(void) {
    for (int i = 0; i < name_count; i++) free(names[i]);
    free(names);
    names = NULL;
    name_count = 0;
}

static void close_state(void) {
    open_ = false;
    wanted++;
    if (job) atomic_store(&job->cancel, true); /* the timer reaps it */
    release_shown();
    free_names();
}

/* However the viewer was left (back, the home swipe, a hardware key), it is
 * closed once it is no longer on the navigation stack. */
static void screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    if (open_ && !gui_navigation_contains(screen)) close_state();
}

static void back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) nav_pop();
}

/* The shell's back and player swipes do not run on this screen's body (see
 * the dead zone in build_screen), so the gesture only steps through images. */
static void body_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE || !open_) return;
    lv_indev_t * indev = lv_event_get_indev(e);
    if (!indev) indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t d = lv_indev_get_gesture_dir(indev);
    if (d != LV_DIR_LEFT && d != LV_DIR_RIGHT) return;
    lv_indev_wait_release(indev);
    int next = current + (d == LV_DIR_LEFT ? 1 : -1);
    if (next >= 0 && next < name_count) request_image(next);
}

static void screen_delete_cb(lv_event_t * e) {
    (void) e;
    unregister_swipe_dead_zone(body);
    screen = title_label = body = image = status_label = NULL;
}

static void build_screen(void) {
    screen = lv_obj_create(NULL);
    lv_obj_add_style(screen, &style_theme_screen_bg, 0);
    lv_obj_add_event_cb(screen, screen_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    /* finalize_screen_navigation() is not used: its swipe-right = back would
     * take the swipes that step through the images. */
    title_label = build_screen_header(screen, "", back_cb, NULL, NULL);

    lv_display_t * disp = lv_display_get_default();
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    body = lv_obj_create(screen);
    lv_obj_set_pos(body, 0, top);
    body_w = lv_display_get_horizontal_resolution(disp);
    body_h = lv_display_get_vertical_resolution(disp) - top;
    lv_obj_set_size(body, body_w, body_h);
    lv_obj_set_style_bg_color(body, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, body_event_cb, LV_EVENT_GESTURE, NULL);
    /* The shell's back and player swipes are polled from raw touches, not
     * from LVGL events, and skip only the screens they name (the text viewer
     * among them) and presses inside a dead zone. */
    register_swipe_dead_zone(body);

    image = lv_image_create(body);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(image);
    status_label = lv_label_create(body);
    lv_obj_add_style(status_label, &style_theme_text_muted, 0);
    lv_obj_center(status_label);
}

bool gui_image_view_open(const char * path) {
    if (gui_navigation_transition_in_progress()) return false;
    /* Covered by another screen and then dropped from the stack, it got no unload event. */
    if (open_ && gui_navigation_contains(screen)) return false;
    if (open_) close_state();
    const char * slash = strrchr(path, '/');
    if (!slash || (size_t) (slash - path) >= sizeof(dir)) return false;
    snprintf(dir, sizeof(dir), "%.*s", (int) (slash - path), path);
    file_browser_index_t * index;
    unsigned count;
    if (!file_browser_index_open_all(dir, &index, &count)) return false;
    free_names();
    names = calloc(count ? count : 1, sizeof(*names));
    int found = -1;
    for (unsigned i = 0; names && i < count; i++) {
        char name[NAME_MAX + 1];
        bool is_dir;
        if (!file_browser_index_entry_name(index, i, name, sizeof(name), &is_dir) || is_dir ||
            !gui_image_view_is_image_name(name))
            continue;
        names[name_count] = strdup(name);
        if (!names[name_count]) break;
        if (strcmp(name, slash + 1) == 0) found = name_count;
        name_count++;
    }
    file_browser_index_close(index);
    if (found < 0) {
        free_names();
        return false;
    }
    if (!screen) build_screen();
    open_ = true;
    nav_push(screen);
    request_image(found);
    return true;
}

void gui_image_view_teardown(void) {
    open_ = false;
    if (job) {
        atomic_store(&job->cancel, true);
        pthread_join(job->thread, NULL);
        free(job->pixels);
        free(job);
        job = NULL;
    }
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    release_shown();
    free_names();
    if (screen) lv_obj_delete(screen);
}
