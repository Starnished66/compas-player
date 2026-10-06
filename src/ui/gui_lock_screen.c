#include "gui_lock_screen.h"
#include "i18n.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "gui_theme.h"
#include "gui_player.h"
#include "app_clock.h"
#include "assets.h"
#include "screen_builders.h"
#include "backlight.h"
#include "gui.h"
#include "fallback_font.h"
#include "battery.h"
#include "frosted_glass.h"
#include "albumart.h"
#include "cover_decode.h"
#include "image_thumb.h"
#include <pthread.h>
#include <stdatomic.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static lv_obj_t * lock_screen = NULL;
static lv_obj_t * lock_image_obj = NULL;
static lv_obj_t * lock_clock_label = NULL;
static lv_obj_t * lock_swipe_hint = NULL;
static lv_obj_t * lock_date_label, * lock_battery_label;
static lv_obj_t * lock_backdrop, * lock_metadata, * lock_metadata_image;
static lv_obj_t * lock_title, * lock_artist;
static gui_lock_screen_image_fit_t current_fit;
static lv_image_dsc_t lock_photo_dsc, lock_backdrop_dsc;
static uint16_t * lock_photo_pixels;
static uint8_t * lock_backdrop_pixels;
static char lock_photo_path[256];
static char lock_cached_photo_path[256];
static bool lock_photo_cache_valid;
static gui_lock_screen_image_fit_t lock_cached_photo_fit;
static gui_lock_screen_mode_t lock_cached_art_mode;
static uint64_t lock_cached_cover_generation;
static pthread_t lock_art_thread;
static bool lock_art_running;
static atomic_bool lock_art_done, lock_art_cancel;
static lv_timer_t * lock_art_timer;
typedef struct {
    gui_lock_screen_mode_t mode;
    gui_lock_screen_image_fit_t fit;
    uint64_t cover_generation;
    char path[256];
    uint16_t * pixels;
    int width, height;
    uint8_t * backdrop;
} lock_art_job_t;
static lock_art_job_t lock_art_job;
static void update_lock_art(void);
static void stop_lock_art(void);
typedef struct {
    lv_obj_t * image;
    lv_timer_t * timer;
    lv_image_dsc_t descriptor;
    uint16_t * pixels;
} lock_preview_t;
static lock_preview_t * lock_preview;
static void refresh_lock_preview(void);
static void refresh_lock_preview_async_cb(void * unused);
static lv_timer_t * lock_clock_timer = NULL;
static lv_timer_t * lock_touch_timer = NULL;

static gui_lock_screen_mode_t current_mode = LOCK_SCREEN_MODE_OFF;
static bool current_clock_24h = true;

static void stop_timers(void);

static bool lock_swipe_was_pressed = false;
static bool lock_swipe_candidate = false;
static bool lock_swipe_tracking = false;
static bool lock_swipe_just_confirmed = false;
static int32_t lock_swipe_touch_start_x = 0;
static int32_t lock_swipe_touch_start_y = 0;
static int32_t lock_swipe_last_v = 0;
static int32_t lock_swipe_last_velocity = 0;
static slide_transition_ctx_t * lock_swipe_ctx = NULL;
static slide_transition_ctx_t * lock_settle_ctx = NULL;
#define LOCK_SWIPE_DEADZONE 20
#define LOCK_SWIPE_SETTLE_MS 200

lv_obj_t * gui_lock_screen_get_screen(void) {
    return lock_screen;
}

bool gui_lock_screen_is_showing(void) {
    return lock_screen != NULL && lv_screen_active() == lock_screen;
}

static void update_clock_display(void) {
    if (!lock_clock_label) return;
    struct tm tm_info;
    app_clock_localtime(&tm_info);
    char buf[16];
    strftime(buf, sizeof(buf), current_clock_24h ? "%H:%M" : "%I:%M", &tm_info);
    if (strcmp(lv_label_get_text(lock_clock_label), buf) != 0)
        lv_label_set_text(lock_clock_label, buf);
    const char * days[] = { TR("Sunday"), TR("Monday"), TR("Tuesday"), TR("Wednesday"), TR("Thursday"), TR("Friday"), TR("Saturday") };
    const char * months[] = { TR("January"), TR("February"), TR("March"), TR("April"), TR("May"), TR("June"), TR("July"), TR("August"), TR("September"), TR("October"), TR("November"), TR("December") };
    snprintf(buf, sizeof(buf), "%d", tm_info.tm_mday);
    char date[96];
    snprintf(date, sizeof(date), "%s, %s %s", days[tm_info.tm_wday], buf, months[tm_info.tm_mon]);
    if (strcmp(lv_label_get_text(lock_date_label), date) != 0)
        lv_label_set_text(lock_date_label, date);
    int percent = battery_get_display_percent();
    if (percent >= 0) snprintf(buf, sizeof(buf), "%d%%", percent);
    else buf[0] = '\0';
    if (strcmp(lv_label_get_text(lock_battery_label), buf) != 0)
        lv_label_set_text(lock_battery_label, buf);
    if (lock_metadata) {
        char title[256], artist[256], album[256];
        double duration;
        bool loaded = gui_plugin_get_now_playing(title, sizeof(title), artist, sizeof(artist), album, sizeof(album), &duration);
        if (loaded) {
            if (strcmp(lv_label_get_text(lock_title), title) != 0) lv_label_set_text(lock_title, title);
            if (strcmp(lv_label_get_text(lock_artist), artist) != 0) lv_label_set_text(lock_artist, artist);
            lv_obj_remove_flag(lock_metadata, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(lock_metadata, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lock_clock_timer_cb(lv_timer_t * timer) {
    lock_preview_t * preview = lv_timer_get_user_data(timer);
    if (preview) {
        if (preview != lock_preview || !backlight_screen_is_on() ||
            lv_obj_get_screen(preview->image) != lv_screen_active()) return;
        char previous[16];
        snprintf(previous, sizeof(previous), "%s", lv_label_get_text(lock_clock_label));
        update_clock_display();
        if (strcmp(previous, lv_label_get_text(lock_clock_label)) != 0) refresh_lock_preview();
        return;
    }
    if (!backlight_screen_is_on()) { lv_timer_pause(timer); return; }
    update_clock_display();
}

/* Compute the same 256-based scaling used by album-art lock screens. The
 * custom image is first loaded at natural size so the R1 LVGL build can
 * provide its dimensions without newer image-decoder APIs. */
static void fit_lock_image(lv_obj_t * image, gui_lock_screen_image_fit_t fit) {
    if (fit == LOCK_SCREEN_IMAGE_FIT_NATURAL) {
        lv_obj_set_size(image, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_align(image, LV_ALIGN_CENTER);
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_DEFAULT);
        lv_image_set_scale(image, 256);
        return;
    }

    lv_obj_set_size(image, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(image, LV_ALIGN_CENTER);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_DEFAULT);
    lv_image_set_scale(image, 256);
    lv_obj_update_layout(image);

    int32_t image_w = lv_obj_get_width(image);
    int32_t image_h = lv_obj_get_height(image);
    int32_t screen_w = lv_obj_get_width(lock_screen);
    int32_t screen_h = lv_obj_get_height(lock_screen);
    uint32_t scale = 256;
    if (image_w > 0 && image_h > 0 && screen_w > 0 && screen_h > 0) {
        bool cover = fit == LOCK_SCREEN_IMAGE_FIT_COVER;
        uint32_t scale_x = cover
            ? ((uint32_t) screen_w * 256U + (uint32_t) image_w - 1U) / (uint32_t) image_w
            : ((uint32_t) screen_w * 256U) / (uint32_t) image_w;
        uint32_t scale_y = cover
            ? ((uint32_t) screen_h * 256U + (uint32_t) image_h - 1U) / (uint32_t) image_h
            : ((uint32_t) screen_h * 256U) / (uint32_t) image_h;
        scale = cover ? (scale_x > scale_y ? scale_x : scale_y)
                      : (scale_x < scale_y ? scale_x : scale_y);
        if (scale == 0) scale = 1;
    }

    lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
    lv_obj_set_align(image, LV_ALIGN_CENTER);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_scale(image, scale);
}

static bool lock_art_should_cancel(void * unused) {
    (void)unused;
    return atomic_load(&lock_art_cancel);
}

/* JPEG/PNG photo decode, same accommodation as the album thumbnail worker. */
#define LOCK_ART_THREAD_STACK_SIZE (4 * 1024 * 1024)

static void * lock_art_worker(void * unused) {
    (void)unused;
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "lockart");
#endif
    install_thread_crash_altstack(); /* see its own comment (main.c) */
    lock_art_job_t * job = &lock_art_job;
    if (job->mode == LOCK_SCREEN_MODE_IMAGE) {
        uint8_t * data = NULL;
        uint32_t size = 0;
        if (albumart_load_file_ex(job->path, &data, &size, 16U * 1024U * 1024U,
                                  ARTWORK_PRIO_PLAYER) == ALBUMART_LOAD_OK) {
            int native_w, native_h;
            if (image_thumb_native_size(data, size, &native_w, &native_h)) {
                if (job->fit == LOCK_SCREEN_IMAGE_FIT_COVER) {
                    /* Decode directly to the destination crop: thin images
                     * cannot overflow LVGL's zoom range or leave bars. */
                    job->width = BOARD_SCREEN_WIDTH;
                    job->height = BOARD_SCREEN_HEIGHT;
                } else if (job->fit == LOCK_SCREEN_IMAGE_FIT_NATURAL &&
                           native_w <= MAX_PLAYER_COVER_SIDE && native_h <= MAX_PLAYER_COVER_SIDE) {
                    job->width = native_w;
                    job->height = native_h;
                } else {
                    image_thumb_fit(native_w, native_h, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT,
                                    &job->width, &job->height);
                }
                cover_decode_to_rgb565_ex(data, size, job->width, job->height,
                                         ARTWORK_PRIO_PLAYER, lock_art_should_cancel, NULL,
                                         &job->pixels);
            }
            free(data);
        }
    }
    if (job->pixels && !lock_art_should_cancel(NULL)) {
        job->backdrop = frosted_glass_blur_rgb565(job->pixels, job->width, job->height,
                        BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT, 5, 3, 3, 4, false);
    }
    atomic_store_explicit(&lock_art_done, true, memory_order_release);
    return NULL;
}

static void stop_lock_art(void) {
    if (lock_art_timer) {
        lv_timer_delete(lock_art_timer);
        lock_art_timer = NULL;
    }
    if (lock_art_running) {
        atomic_store(&lock_art_cancel, true);
        pthread_join(lock_art_thread, NULL);
        lock_art_running = false;
    }
    free(lock_art_job.pixels);
    free(lock_art_job.backdrop);
    memset(&lock_art_job, 0, sizeof(lock_art_job));
}

static void set_lock_descriptor(lv_image_dsc_t * descriptor, const void * pixels,
                                int width, int height) {
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor->header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor->header.w = width;
    descriptor->header.h = height;
    descriptor->header.stride = width * 2;
    descriptor->data = pixels;
    descriptor->data_size = (uint32_t)width * height * 2;
}

static void lock_art_poll_cb(lv_timer_t * timer) {
    (void)timer;
    if (!atomic_load_explicit(&lock_art_done, memory_order_acquire) || gui_navigation_transition_in_progress()) return;
    pthread_join(lock_art_thread, NULL);
    lock_art_running = false;
    if (lock_art_job.mode == LOCK_SCREEN_MODE_IMAGE && lock_art_job.pixels) {
        snprintf(lock_cached_photo_path, sizeof(lock_cached_photo_path), "%s", lock_art_job.path);
        lock_photo_cache_valid = lock_art_job.backdrop != NULL;
        lock_cached_photo_fit = lock_art_job.fit;
        lv_image_set_src(lock_image_obj, NULL);
        lv_image_cache_drop(&lock_photo_dsc);
        free(lock_photo_pixels);
        lock_photo_pixels = lock_art_job.pixels;
        lock_art_job.pixels = NULL;
        set_lock_descriptor(&lock_photo_dsc, lock_photo_pixels, lock_art_job.width, lock_art_job.height);
        lv_image_set_src(lock_image_obj, &lock_photo_dsc);
        fit_lock_image(lock_image_obj, current_fit);
        lv_obj_remove_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (lock_art_job.backdrop) {
        lv_image_set_src(lock_backdrop, NULL);
        lv_image_set_src(lock_metadata_image, NULL);
        lv_image_cache_drop(&lock_backdrop_dsc);
        free(lock_backdrop_pixels);
        lock_backdrop_pixels = lock_art_job.backdrop;
        lock_art_job.backdrop = NULL;
        set_lock_descriptor(&lock_backdrop_dsc, lock_backdrop_pixels, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
        lv_image_set_src(lock_backdrop, &lock_backdrop_dsc);
        fit_lock_image(lock_backdrop, LOCK_SCREEN_IMAGE_FIT_COVER);
        if (current_fit == LOCK_SCREEN_IMAGE_FIT_CONTAIN)
            lv_obj_remove_flag(lock_backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(lock_metadata_image, &lock_backdrop_dsc);
        lv_obj_set_size(lock_metadata_image, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
        lv_obj_update_layout(lock_metadata);
        lv_obj_set_pos(lock_metadata_image, -lv_obj_get_x(lock_metadata), -lv_obj_get_y(lock_metadata));
        lv_obj_remove_flag(lock_metadata_image, LV_OBJ_FLAG_HIDDEN);
        lock_cached_art_mode = lock_art_job.mode;
        lock_cached_cover_generation = lock_art_job.cover_generation;
    }
    if (lock_art_job.mode == LOCK_SCREEN_MODE_IMAGE && !lock_photo_cache_valid && !lock_photo_dsc.data &&
        (gui_lock_screen_is_showing() || (lock_preview && gui_navigation_contains(lv_obj_get_screen(lock_preview->image)))))
        show_info_toast(TR("Could not load lock screen photo"));
    stop_lock_art();
    lv_obj_invalidate(lock_screen);
    refresh_lock_preview();
}

static void update_lock_art(void) {
    uint64_t generation = 0;
    const lv_image_dsc_t * cover = gui_player_get_current_cover_dsc(&generation);
    /* Rebuilding settings or waking must not cancel identical work and
     * synchronously wait for the same photo read/blur a second time. */
    if (lock_art_running && lock_art_job.mode == current_mode &&
        ((current_mode == LOCK_SCREEN_MODE_ALBUM_ART && lock_art_job.cover_generation == generation) ||
         (current_mode == LOCK_SCREEN_MODE_IMAGE && lock_art_job.fit == current_fit &&
          strcmp(lock_art_job.path, lock_photo_path) == 0))) return;
    bool cached = lock_cached_art_mode == current_mode && lock_backdrop_dsc.data &&
        ((current_mode == LOCK_SCREEN_MODE_ALBUM_ART && cover && cover->data &&
          lock_cached_cover_generation == generation) ||
         (current_mode == LOCK_SCREEN_MODE_IMAGE && lock_photo_cache_valid && lock_photo_dsc.data &&
          strcmp(lock_cached_photo_path, lock_photo_path) == 0 && lock_cached_photo_fit == current_fit));
    if (cached) {
        if (current_mode == LOCK_SCREEN_MODE_IMAGE) {
            lv_image_set_src(lock_image_obj, &lock_photo_dsc);
            fit_lock_image(lock_image_obj, current_fit);
            lv_obj_remove_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);
        }
        if (lock_backdrop_dsc.data) {
            if (current_fit == LOCK_SCREEN_IMAGE_FIT_CONTAIN) lv_obj_remove_flag(lock_backdrop, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(lock_backdrop, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(lock_metadata_image, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    stop_lock_art();
    lv_obj_add_flag(lock_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lock_metadata_image, LV_OBJ_FLAG_HIDDEN);
    if (current_mode == LOCK_SCREEN_MODE_CLOCK) return;
    lock_cached_art_mode = LOCK_SCREEN_MODE_OFF;
    lock_photo_cache_valid = false;
    if (current_mode == LOCK_SCREEN_MODE_IMAGE) {
        lv_image_set_src(lock_image_obj, NULL);
        lv_image_cache_drop(&lock_photo_dsc);
        free(lock_photo_pixels);
        lock_photo_pixels = NULL;
        memset(&lock_photo_dsc, 0, sizeof(lock_photo_dsc));
    }
    lock_art_job.mode = current_mode;
    lock_art_job.fit = current_fit;
    lock_art_job.cover_generation = generation;
    if (current_mode == LOCK_SCREEN_MODE_IMAGE) {
        snprintf(lock_art_job.path, sizeof(lock_art_job.path), "%s", lock_photo_path);
    } else {
        if (!cover || !cover->data) return;
        lock_art_job.width = cover->header.w;
        lock_art_job.height = cover->header.h;
        size_t bytes = (size_t)cover->header.w * cover->header.h * 2;
        if (cover->header.cf != LV_COLOR_FORMAT_RGB565 || cover->header.stride != cover->header.w * 2 ||
            !bytes || bytes > cover->data_size) return;
        lock_art_job.pixels = malloc(bytes);
        if (!lock_art_job.pixels) return;
        memcpy(lock_art_job.pixels, cover->data, bytes);
    }
    atomic_store(&lock_art_done, false);
    atomic_store(&lock_art_cancel, false);
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized &&
        pthread_attr_setstacksize(&attr, LOCK_ART_THREAD_STACK_SIZE) == 0)
        attr_ptr = &attr;
    int create_rc = pthread_create(&lock_art_thread, attr_ptr, lock_art_worker, NULL);
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        stop_lock_art();
        return;
    }
    lock_art_running = true;
    lock_art_timer = lv_timer_create(lock_art_poll_cb, 40, NULL);
}

static void lock_settle_done_cb(lv_anim_t * a) {
    lock_settle_ctx = NULL;
    slide_transition_done_cb(a);
}

static void lock_touch_timer_cb(lv_timer_t * timer) {
    (void) timer;
    if (!gui_lock_screen_is_showing()) return;
    if (!backlight_screen_is_on()) { lv_timer_pause(timer); return; }

    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED);
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t screen_height = lv_display_get_vertical_resolution(lv_display_get_default());

    if (pressed && !lock_swipe_was_pressed) {
        lock_swipe_candidate = true;
        lock_swipe_touch_start_x = p.x;
        lock_swipe_touch_start_y = p.y;
        lock_swipe_tracking = false;
    }

    if (pressed && lock_swipe_candidate && !lock_swipe_tracking) {
        int32_t dx = p.x - lock_swipe_touch_start_x;
        int32_t dy = p.y - lock_swipe_touch_start_y;
        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;

        if (adx >= BOARD_SCALE_PX(LOCK_SWIPE_DEADZONE) || ady >= BOARD_SCALE_PX(LOCK_SWIPE_DEADZONE)) {
            if (dy < 0 && ady > adx) {
                lv_obj_t * target = gui_navigation_get_screen_at(gui_navigation_get_depth() - 2);
                if (target) {
                    lock_swipe_ctx = begin_slide_transition_ex(target, true, true, true);
                    if (lock_swipe_ctx) {
                        lock_swipe_ctx->commit = false;
                        lock_swipe_tracking = true;
                        lock_swipe_just_confirmed = true;
                        lock_swipe_last_v = 0;
                        lock_swipe_last_velocity = 0;
                        lv_indev_wait_release(indev);
                    }
                }
            } else if (adx > ady && !lv_indev_get_scroll_obj(indev)) {
                lv_indev_wait_release(indev);
            }
            lock_swipe_candidate = false;
        }
    }

    /* Deliberately a separate `if`, not `else if` chained to the deadzone-
     * confirm block above -- on the exact tick the deadzone confirms and
     * sets lock_swipe_tracking, this block must ALSO run so lock_swipe_
     * last_v/last_velocity are sampled from the real touch position right
     * away, not left at the confirm block's own zero-initialization. A fast
     * flick that crosses the deadzone and releases on the very next tick
     * would otherwise see last_v/last_velocity still 0 at release and
     * wrongly fall through to the halfway-position cancel path despite the
     * confirmed upward movement. just_confirmed still defers only the frame
     * PRESENTATION (slide_transition_anim_x_cb) to the next tick, not this
     * position sampling -- matching gui_shell.c's home-swipe/back-swipe. */
    if (pressed && lock_swipe_tracking) {
        int32_t v = p.y - lock_swipe_touch_start_y;
        if (v < -screen_height) v = -screen_height;
        if (v > 0) v = 0;

        int32_t delta = v - lock_swipe_last_v;
        if (delta != 0) lock_swipe_last_velocity = delta;
        lock_swipe_last_v = v;

        if (lock_swipe_just_confirmed) {
            lock_swipe_just_confirmed = false;
        } else {
            slide_transition_anim_x_cb(lock_swipe_ctx, v);
        }
    }

    if (!pressed && lock_swipe_was_pressed && lock_swipe_tracking) {
        lock_swipe_tracking = false;
        int32_t current_v = lock_swipe_last_v;
        bool commit = (lock_swipe_last_velocity < 0) ? true
                     : (lock_swipe_last_velocity > 0) ? false
                     : (current_v < -screen_height / 2);

        slide_transition_ctx_t * settle_ctx = lock_swipe_ctx;
        lock_swipe_ctx = NULL;
        lock_swipe_candidate = false;
        lock_swipe_just_confirmed = false;
        lock_swipe_was_pressed = false;

        settle_ctx->commit = commit;

        if (commit) {
            stop_timers();
            nav_pop_stack_only();
        }

        lock_settle_ctx = settle_ctx;

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, settle_ctx);
        lv_anim_set_user_data(&a, settle_ctx);
        lv_anim_set_values(&a, current_v, commit ? -screen_height : 0);
        lv_anim_set_duration(&a, gui_anim_ms(LOCK_SWIPE_SETTLE_MS));
        lv_anim_set_exec_cb(&a, slide_transition_anim_x_cb);
        lv_anim_set_completed_cb(&a, lock_settle_done_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    lock_swipe_was_pressed = pressed;
}

void gui_lock_screen_swipe_recover(void * ctx) {
    slide_transition_ctx_t * sctx = (slide_transition_ctx_t *) ctx;
    if (sctx == lock_swipe_ctx) lock_swipe_ctx = NULL;
    if (sctx == lock_settle_ctx) lock_settle_ctx = NULL;
    lock_swipe_tracking = false;
    lock_swipe_candidate = false;
    lock_swipe_just_confirmed = false;
    lock_swipe_was_pressed = false;
}

void gui_lock_screen_reset_drag_state(void) {
    if (lock_swipe_ctx) {
        slide_transition_cancel(&lock_swipe_ctx);
    }
    if (lock_settle_ctx && !lock_settle_ctx->commit) {
        slide_transition_cancel(&lock_settle_ctx);
    }
    lock_swipe_tracking = false;
    lock_swipe_candidate = false;
    lock_swipe_just_confirmed = false;
    lock_swipe_was_pressed = false;
}

static void start_timers(void) {
    /* Symmetric, not just a conditional start -- gui_lock_screen_show() can
     * be called again with a DIFFERENT mode while already showing (e.g. a
     * second screen_woke fires before the user dismisses), and this must
     * leave lock_clock_timer matching the NEW mode either way. A one-sided
     * "start if clock" here previously left a stale timer running forever
     * (until the eventual hide/teardown) after switching away from clock
     * mode without an intervening hide(). */
    if (current_mode == LOCK_SCREEN_MODE_CLOCK ||
        current_mode == LOCK_SCREEN_MODE_IMAGE ||
        current_mode == LOCK_SCREEN_MODE_ALBUM_ART) {
        if (!lock_clock_timer) {
            lock_clock_timer = lv_timer_create(lock_clock_timer_cb, 1000, NULL);
        }
    } else if (lock_clock_timer) {
        lv_timer_delete(lock_clock_timer);
        lock_clock_timer = NULL;
    }
    if (!lock_touch_timer) {
        lock_touch_timer = lv_timer_create(lock_touch_timer_cb, LV_DEF_REFR_PERIOD, NULL);
    }
    if (lock_clock_timer) lv_timer_resume(lock_clock_timer);
    if (lock_touch_timer) lv_timer_resume(lock_touch_timer);
}

static void stop_timers(void) {
    if (lock_clock_timer) {
        lv_timer_delete(lock_clock_timer);
        lock_clock_timer = NULL;
    }
    if (lock_touch_timer) {
        lv_timer_delete(lock_touch_timer);
        lock_touch_timer = NULL;
    }
}

static void lock_screen_unloaded_cb(lv_event_t * event) {
    (void)event;
    stop_timers();
    lv_async_call(refresh_lock_preview_async_cb, NULL);
    /* Worker completion can still populate the bounded cache while hidden. */
}

bool gui_lock_screen_has_background_work(bool include_photos) {
    return lock_art_running && (include_photos || lock_art_job.mode != LOCK_SCREEN_MODE_IMAGE);
}

static void build_lock_screen_if_needed(void) {
    if (lock_screen) return;

    lock_screen = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(lock_screen, 0, 0);
    lv_obj_set_style_border_width(lock_screen, 0, 0);
    lv_obj_add_style(lock_screen, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(lock_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(lock_screen, lock_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    lock_backdrop = lv_image_create(lock_screen);
    lv_obj_add_flag(lock_backdrop, LV_OBJ_FLAG_HIDDEN);
    lock_image_obj = lv_image_create(lock_screen);
    lv_obj_align(lock_image_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);

    /* Readable text without an opaque artwork card or black letterboxing. */
    lv_obj_t * top_dim = lv_obj_create(lock_screen);
    lv_obj_remove_style_all(top_dim);
    lv_obj_set_size(top_dim, LV_PCT(100), BOARD_SCREEN_HEIGHT / 3);
    lv_obj_set_style_bg_color(top_dim, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(top_dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(top_dim, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(top_dim, LV_OPA_60, 0);
    lv_obj_set_style_bg_grad_opa(top_dim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_dir(top_dim, LV_GRAD_DIR_VER, 0);
    lv_obj_t * bottom_dim = lv_obj_create(lock_screen);
    lv_obj_remove_style_all(bottom_dim);
    lv_obj_set_size(bottom_dim, LV_PCT(100), BOARD_SCREEN_HEIGHT / 2);
    lv_obj_align(bottom_dim, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom_dim, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(bottom_dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bottom_dim, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(bottom_dim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(bottom_dim, LV_OPA_80, 0);
    lv_obj_set_style_bg_grad_dir(bottom_dim, LV_GRAD_DIR_VER, 0);

    lock_clock_label = lv_label_create(lock_screen);
    lv_obj_add_style(lock_clock_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_align(lock_clock_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lock_clock_label, &app_font_display, 0);
    lv_obj_align(lock_clock_label, LV_ALIGN_TOP_MID, 0, BOARD_SCREEN_HEIGHT / 10);
    lv_obj_add_flag(lock_clock_label, LV_OBJ_FLAG_HIDDEN);

    lock_swipe_hint = lv_label_create(lock_screen);
    lv_label_set_text(lock_swipe_hint, TR("Swipe up to unlock"));
    lv_obj_add_style(lock_swipe_hint, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(lock_swipe_hint, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_color(lock_swipe_hint, lv_color_white(), 0);
    lv_obj_set_style_text_align(lock_swipe_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lock_swipe_hint, lv_pct(90));
    lv_obj_align(lock_swipe_hint, LV_ALIGN_BOTTOM_MID, 0, -BOARD_SCALE_PX(34));
    lock_date_label = lv_label_create(lock_screen);
    lv_obj_set_style_text_font(lock_date_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_color(lock_date_label, lv_color_white(), 0);
    lv_obj_set_width(lock_date_label, LV_PCT(92));
    lv_obj_set_style_text_align(lock_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lock_date_label, LV_LABEL_LONG_DOT);
    lv_obj_align(lock_date_label, LV_ALIGN_TOP_MID, 0, BOARD_SCREEN_HEIGHT / 18);
    lock_battery_label = lv_label_create(lock_screen);
    lv_obj_set_style_text_font(lock_battery_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_text_color(lock_battery_label, lv_color_white(), 0);
    lv_obj_align(lock_battery_label, LV_ALIGN_TOP_MID, 0, BOARD_SCREEN_HEIGHT / 10 + app_font_display.line_height + BOARD_SCALE_PX(8));
    lv_obj_t * arrow = lv_label_create(lock_screen);
    lv_label_set_text(arrow, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(arrow, LV_FONT_DEFAULT, 0);
    lv_obj_add_style(arrow, gui_theme_accent_style(), 0);
    lv_obj_align(arrow, LV_ALIGN_BOTTOM_MID, 0, -BOARD_SCALE_PX(82));
    lv_obj_t * handle = lv_obj_create(lock_screen);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, BOARD_SCALE_PX(100), BOARD_SCALE_PX(4));
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_add_style(handle, gui_theme_accent_style(), 0);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -BOARD_SCALE_PX(14));
    lock_metadata = lv_obj_create(lock_screen);
    lv_obj_set_size(lock_metadata, LV_PCT(90), BOARD_SCALE_PX(110));
    lv_obj_set_style_pad_all(lock_metadata, 0, 0);
    lv_obj_set_style_border_width(lock_metadata, 0, 0);
    lv_obj_set_style_radius(lock_metadata, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_clip_corner(lock_metadata, true, 0);
    lv_obj_set_style_bg_color(lock_metadata, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lock_metadata, LV_OPA_60, 0);
    lv_obj_remove_flag(lock_metadata, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(lock_metadata, LV_ALIGN_BOTTOM_MID, 0, -BOARD_SCALE_PX(124));
    lock_metadata_image = lv_image_create(lock_metadata);
    lv_obj_add_flag(lock_metadata_image, LV_OBJ_FLAG_HIDDEN);
    lock_title = lv_label_create(lock_metadata);
    lv_obj_set_style_text_font(lock_title, &app_font_player_title, 0);
    lv_obj_set_style_text_color(lock_title, lv_color_white(), 0);
    lv_obj_set_width(lock_title, LV_PCT(90));
    lv_obj_set_style_text_align(lock_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lock_title, LV_LABEL_LONG_DOT);
    lv_obj_align(lock_title, LV_ALIGN_TOP_MID, 0, BOARD_SCALE_PX(12));
    lock_artist = lv_label_create(lock_metadata);
    lv_obj_set_style_text_font(lock_artist, &app_font_player_meta, 0);
    lv_obj_set_style_text_color(lock_artist, lv_color_hex(0xC2C7CE), 0);
    lv_obj_set_width(lock_artist, LV_PCT(90));
    lv_obj_set_style_text_align(lock_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lock_artist, LV_LABEL_LONG_DOT);
    lv_obj_align(lock_artist, LV_ALIGN_BOTTOM_MID, 0, -BOARD_SCALE_PX(16));
}

static bool prepare_lock_screen(const gui_lock_screen_options_t * options) {
    if (!options || options->mode == LOCK_SCREEN_MODE_OFF) {
        return false;
    }

    build_lock_screen_if_needed();

    current_mode = options->mode;
    current_clock_24h = options->clock_24h;
    lv_obj_set_style_text_color(lock_clock_label, lv_color_white(), 0);
    current_fit = options->image_fit;
    if (current_mode == LOCK_SCREEN_MODE_ALBUM_ART && current_fit == LOCK_SCREEN_IMAGE_FIT_NATURAL)
        current_fit = LOCK_SCREEN_IMAGE_FIT_COVER;
    lv_obj_add_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);
    if (current_mode == LOCK_SCREEN_MODE_ALBUM_ART) {
        lv_obj_set_style_text_color(lock_clock_label, lv_color_white(), 0);
        const lv_image_dsc_t * cover = gui_player_get_current_cover_dsc(NULL);
        lv_image_set_src(lock_image_obj, cover && cover->data ? (const void *)cover : (const void *)asset_path("playing_plane/default_cover_565.png"));
        fit_lock_image(lock_image_obj, current_fit);
        lv_obj_remove_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);
    } else if (current_mode == LOCK_SCREEN_MODE_IMAGE) {
        snprintf(lock_photo_path, sizeof(lock_photo_path), "%s", options->image_path);

    }
    update_clock_display();
    lv_obj_remove_flag(lock_clock_label, LV_OBJ_FLAG_HIDDEN);
    update_lock_art();

    return true;
}

bool gui_lock_screen_show(const gui_lock_screen_options_t * options) {
    if (!prepare_lock_screen(options)) return false;
    start_timers();
    if (lv_screen_active() != lock_screen) nav_push(lock_screen);
    return true;
}

static void lock_preview_deleted_cb(lv_event_t * event) {
    lock_preview_t * preview = lv_event_get_user_data(event);
    if (lock_preview == preview) lock_preview = NULL;
    if (preview->timer) lv_timer_delete(preview->timer);
    lv_image_set_src(preview->image, NULL);
    lv_image_cache_drop(&preview->descriptor);
    free(preview->pixels);
    free(preview);
}

static void refresh_lock_preview(void) {
    if (!lock_preview || !lock_screen || gui_lock_screen_is_showing()) return;
    lv_draw_buf_t * snapshot = lv_snapshot_take(lock_screen, LV_COLOR_FORMAT_RGB565);
    if (!snapshot) return;
    int width = BOARD_SCALE_PX(144);
    int height = BOARD_SCREEN_HEIGHT * width / BOARD_SCREEN_WIDTH;
    uint16_t * packed = NULL;
    const uint16_t * source = (const uint16_t *)snapshot->data;
    if (snapshot->header.stride != snapshot->header.w * 2U) {
        packed = malloc((size_t)snapshot->header.w * snapshot->header.h * 2);
        if (!packed) { lv_draw_buf_destroy(snapshot); return; }
        for (uint32_t y = 0; y < snapshot->header.h; ++y)
            memcpy(packed + y * snapshot->header.w, snapshot->data + y * snapshot->header.stride,
                   snapshot->header.w * 2U);
        source = packed;
    }
    uint16_t * pixels = cover_resize_rgb565(source, snapshot->header.w, snapshot->header.h, width, height);
    free(packed);
    lv_draw_buf_destroy(snapshot);
    if (!pixels) return;
    lv_image_set_src(lock_preview->image, NULL);
    lv_image_cache_drop(&lock_preview->descriptor);
    free(lock_preview->pixels);
    lock_preview->pixels = pixels;
    set_lock_descriptor(&lock_preview->descriptor, pixels, width, height);
    lv_image_set_src(lock_preview->image, &lock_preview->descriptor);
    gui_navigation_invalidate_back_snapshot(lv_obj_get_screen(lock_preview->image));
}

static void refresh_lock_preview_async_cb(void * unused) {
    (void)unused;
    refresh_lock_preview();
}

lv_obj_t * gui_lock_screen_create_preview(lv_obj_t * parent, const gui_lock_screen_options_t * options) {
    if (!parent || gui_lock_screen_is_showing() || !prepare_lock_screen(options)) return NULL;
    lock_preview_t * preview = calloc(1, sizeof(*preview));
    if (!preview) return NULL;
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), BOARD_SCREEN_HEIGHT * BOARD_SCALE_PX(144) / BOARD_SCREEN_WIDTH + BOARD_SCALE_PX(32));
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    preview->image = lv_image_create(card);
    lv_obj_center(preview->image);
    lv_obj_set_style_radius(preview->image, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_clip_corner(preview->image, true, 0);
    lv_obj_set_style_border_width(preview->image, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_border_color(preview->image, lv_color_hex(GUI_COLOR_BORDER), 0);
    lv_obj_add_event_cb(preview->image, lock_preview_deleted_cb, LV_EVENT_DELETE, preview);
    lock_preview = preview;
    preview->timer = lv_timer_create(lock_clock_timer_cb, 1000, preview);
    refresh_lock_preview();
    return card;
}

void gui_lock_screen_refresh(bool artwork_changed) {
    bool preview_live = lock_preview && gui_navigation_contains(lv_obj_get_screen(lock_preview->image));
    if ((!gui_lock_screen_is_showing() && !preview_live) || !lock_image_obj) return;
    if (gui_lock_screen_is_showing() && backlight_screen_is_on()) start_timers();
    update_clock_display();
    if (artwork_changed && current_mode == LOCK_SCREEN_MODE_ALBUM_ART) {
        const lv_image_dsc_t * cover = gui_player_get_current_cover_dsc(NULL);
        lv_image_set_src(lock_image_obj, cover && cover->data ? (const void *)cover : (const void *)asset_path("playing_plane/default_cover_565.png"));
        fit_lock_image(lock_image_obj, current_fit);
        update_lock_art();
    }
    /* Coalesce metadata changes and keep snapshot/resize out of the
     * display's REFR_READY callback. */
    lv_async_call_cancel(refresh_lock_preview_async_cb, NULL);
    lv_async_call(refresh_lock_preview_async_cb, NULL);
}

void gui_lock_screen_init(void) {
    lock_screen = NULL;
    lock_image_obj = NULL;
    lock_clock_label = NULL;
    lock_swipe_hint = NULL;
    lock_date_label = lock_battery_label = NULL;
    lock_backdrop = lock_metadata = lock_metadata_image = NULL;
    lock_title = lock_artist = NULL;
    lock_clock_timer = NULL;
    lock_touch_timer = NULL;
    current_mode = LOCK_SCREEN_MODE_OFF;
}

/* Called from gui_soft_reload() (gui_reload.c), after gui_navigation_teardown()
 * has already zeroed nav_stack/nav_depth -- calling nav_pop() here (as an
 * earlier version of this function did) would read nav_stack[-1], the exact
 * out-of-bounds class of crash already found and fixed once in this reload
 * path for the Plugin Manager. A reload rebuilds the whole screen stack from
 * scratch (gui_navigation_init() loads Home again further down the same
 * sequence), so the lock screen doesn't need to navigate anywhere on its way
 * out -- it only needs to release its own owned resources. */
void gui_lock_screen_teardown(void) {
    stop_timers();
    stop_lock_art();
    lv_async_call_cancel(refresh_lock_preview_async_cb, NULL);
    lock_preview = NULL;
    if (lock_screen) {
        lv_obj_delete(lock_screen);
    }
    lv_image_cache_drop(&lock_photo_dsc);
    lv_image_cache_drop(&lock_backdrop_dsc);
    free(lock_photo_pixels);
    free(lock_backdrop_pixels);
    lock_photo_pixels = NULL;
    lock_backdrop_pixels = NULL;
    lock_photo_cache_valid = false;
    lock_cached_art_mode = LOCK_SCREEN_MODE_OFF;
    memset(&lock_photo_dsc, 0, sizeof(lock_photo_dsc));
    memset(&lock_backdrop_dsc, 0, sizeof(lock_backdrop_dsc));
    gui_lock_screen_init();
}
