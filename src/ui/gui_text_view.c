#include "gui_text_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board_config.h"
#include "gui_navigation.h"
#include "gui_theme.h"
#include "screen_builders.h"
#include "utf8_util.h"
#include "src/misc/lv_text_private.h"

static lv_obj_t * s_screen;
static lv_obj_t * s_body;
static lv_obj_t * s_content_label;
static lv_obj_t * s_image; /* picture pages, hidden on text pages */
static char ** s_images;   /* owned copies of opts.images */
static int s_image_count;
static lv_obj_t * s_footer_label;
static lv_obj_t * s_title_label;

static lv_timer_t * s_timer;

static char * s_text;
static uint32_t s_text_len;
static uint32_t * s_page_off;
static uint32_t s_measured;
static bool s_scan_done;
static bool s_pages_truncated;

static int s_current_page;
static int s_last_reported_page;
static int s_last_reported_pages;

static int s_target_pending_page;
static bool s_target_pending_offset;
static size_t s_target_offset;

static int s_lines_per_page = 1;
static int32_t s_content_width = 1;
static int32_t s_body_height = 1;
static int32_t s_screen_width = BOARD_SCREEN_WIDTH;
static const lv_font_t * s_font;

static gui_text_view_turn_fn s_on_turn;
static gui_text_view_close_fn s_on_close;
static void * s_user;

static bool s_open;
static bool s_initial_turn_pending;
static unsigned s_generation; /* bumped per show, so a stale timer tick stops after Lua */
static bool s_closing;
static bool s_teardown;
static bool s_gesture_consumed;

/* on_close runs from a timer once no slide transition is in flight: never
 * inside a plugin's own call, and a view it opens cannot be unloaded by the
 * transition that closed the previous one. */
#define PENDING_CLOSE_MAX 4
typedef struct {
    gui_text_view_close_fn fn;
    void * user;
    int page;
    size_t offset;
} pending_close_t;

static pending_close_t s_pending_close[PENDING_CLOSE_MAX];
static int s_pending_close_count;
static lv_timer_t * s_close_timer;

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

static void update_footer(void) {
    if (!s_footer_label) return;
    char buf[64];
    if (s_scan_done) {
        if (s_pages_truncated) {
            snprintf(buf, sizeof(buf), "%d / %d+", s_current_page, (int) s_measured);
        } else {
            snprintf(buf, sizeof(buf), "%d / %d", s_current_page, (int) s_measured);
        }
    } else {
        snprintf(buf, sizeof(buf), "%d / ...", s_current_page);
    }
    lv_label_set_text(s_footer_label, buf);
}

#define TV_MARKER '\x1b'

/* Length of the picture marker at off ("<ESC><1-3 digits><ESC>" naming an
 * existing image), or 0 when there is none. */
static uint32_t marker_at(uint32_t off, int * out_index) {
    if (!s_text || s_image_count <= 0 || off >= s_text_len || s_text[off] != TV_MARKER) return 0;
    uint32_t p = off + 1;
    int index = 0, digits = 0;
    while (p < s_text_len && digits < 3 && s_text[p] >= '0' && s_text[p] <= '9') {
        index = index * 10 + (s_text[p] - '0');
        p++;
        digits++;
    }
    if (digits == 0 || p >= s_text_len || s_text[p] != TV_MARKER || index >= s_image_count) return 0;
    if (out_index) *out_index = index;
    return p + 1 - off;
}

static void images_free(void) {
    for (int i = 0; i < s_image_count; i++) free(s_images[i]);
    free(s_images);
    s_images = NULL;
    s_image_count = 0;
}

/* Shows images[index] scaled to fit the page body, centered. */
static void show_image(int index) {
    if (!s_image) return;
    char src[600];
    snprintf(src, sizeof(src), "S:%s", s_images[index]);
    lv_image_header_t header;
    if (lv_image_decoder_get_info(src, &header) != LV_RESULT_OK || header.w == 0 || header.h == 0) {
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_image_set_src(s_image, src);
    int64_t scale_w = (int64_t) s_content_width * 256 / header.w;
    int64_t scale_h = (int64_t) s_body_height * 256 / header.h;
    int64_t scale = scale_w < scale_h ? scale_w : scale_h;
    lv_image_set_scale(s_image, (uint32_t) (scale < 1 ? 1 : scale));
    lv_obj_center(s_image);
    lv_obj_remove_flag(s_image, LV_OBJ_FLAG_HIDDEN);
}

static void show_page(int page) {
    if (!s_content_label || !s_text || !s_page_off) return;
    if (page < 1 || page > (int) s_measured) return;

    s_current_page = page;

    uint32_t start_off = s_page_off[page - 1];
    uint32_t end_off = s_page_off[page];

    int image_index;
    if (marker_at(start_off, &image_index)) {
        lv_obj_add_flag(s_content_label, LV_OBJ_FLAG_HIDDEN);
        show_image(image_index);
        update_footer();
        return;
    }
    if (s_image) lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_content_label, LV_OBJ_FLAG_HIDDEN);

    if (end_off < s_text_len) {
        char saved = s_text[end_off];
        s_text[end_off] = '\0';
        lv_label_set_text(s_content_label, s_text + start_off);
        s_text[end_off] = saved;
    } else {
        lv_label_set_text(s_content_label, s_text + start_off);
    }

    update_footer();
}

/* True while a requested page or offset is not measured yet. Turns wait so
 * they are not applied to the provisional page. Closing still reports
 * s_target_offset, so callers must not clear these flags first. */
static bool seek_still_pending(void) {
    return s_target_pending_page > 0 || s_target_pending_offset;
}

static void fire_on_turn(void) {
    if (!s_open || !s_on_turn || !s_page_off) return;
    /* A provisional page shown while seeking must not overwrite a saved position */
    if (seek_still_pending()) return;
    s_initial_turn_pending = false;

    gui_text_view_turn_fn fn = s_on_turn;
    void * user = s_user;
    int page = s_current_page;
    int pages = (int) s_measured;
    size_t byte_offset = (page >= 1) ? (size_t) s_page_off[page - 1] : 0;

    s_last_reported_page = page;
    s_last_reported_pages = pages;

    fn(page, pages, byte_offset, user);
}

static pending_close_t pop_pending_close(void) {
    pending_close_t pc = s_pending_close[0];
    s_pending_close_count--;
    memmove(&s_pending_close[0], &s_pending_close[1], (size_t) s_pending_close_count * sizeof(s_pending_close[0]));
    return pc;
}

static void close_timer_cb(lv_timer_t * timer) {
    if (timer != s_close_timer) return;
    if (gui_navigation_transition_in_progress()) return;
    while (s_pending_close_count > 0) {
        pending_close_t pc = pop_pending_close();
        pc.fn(pc.page, pc.offset, pc.user);
        if (timer != s_close_timer) return; /* teardown ran inside the callback */
        if (gui_navigation_transition_in_progress()) return;
    }
    lv_timer_delete(s_close_timer);
    s_close_timer = NULL;
}

static void queue_close(gui_text_view_close_fn fn, void * user, int page, size_t offset) {
    if (!fn) return;
    if (s_pending_close_count == PENDING_CLOSE_MAX) {
        /* Each entry needs its own open and close, so this is not reached in
         * practice; deliver the oldest now rather than drop it. */
        pending_close_t pc = pop_pending_close();
        pc.fn(pc.page, pc.offset, pc.user);
    }
    s_pending_close[s_pending_close_count++] = (pending_close_t) { fn, user, page, offset };
    if (!s_close_timer) {
        s_close_timer = lv_timer_create(close_timer_cb, 16, NULL);
    }
    if (!s_close_timer) {
        while (s_pending_close_count > 0) {
            pending_close_t pc = pop_pending_close();
            pc.fn(pc.page, pc.offset, pc.user);
        }
    }
}

static void view_close_internal(bool from_unload) {
    if (s_closing || !s_open) return;
    s_closing = true;

    /* Timer lifetime: delete on close so background measurement ticks do not leak. */
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }

    int page = s_current_page;
    size_t byte_offset = (s_page_off && page >= 1) ? (size_t) s_page_off[page - 1] : 0;
    if (s_target_pending_offset) {
        byte_offset = s_target_offset; /* closed before the resume seek finished */
    }
    gui_text_view_close_fn on_close_cb = s_on_close;
    void * user_data = s_user;

    s_on_turn = NULL;
    s_on_close = NULL;
    s_user = NULL;
    s_open = false;
    s_initial_turn_pending = false;
    s_target_pending_page = 0;
    s_target_pending_offset = false;

    if (s_text) {
        free(s_text);
        s_text = NULL;
    }
    if (s_page_off) {
        free(s_page_off);
        s_page_off = NULL;
    }
    images_free();

    /* Unload vs back: the unload path was triggered because navigation already happened
     * (e.g. swipe-up-to-home), so calling nav_pop() would pop the wrong screen. Only
     * pop when closing via header back or prev-from-page-1. */
    if (!from_unload) {
        nav_pop();
    }

    s_closing = false;

    queue_close(on_close_cb, user_data, page, byte_offset);
}

/* A screen pushed on top (lock screen, drawer Wi-Fi) only covers the view;
 * it closes once its stack entry is gone. */
static void screen_unloaded_event_cb(lv_event_t * e) {
    (void) e;
    if (s_closing || s_teardown) return;
    if (s_open && !gui_navigation_contains(s_screen)) {
        view_close_internal(true);
    }
}

static void header_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    view_close_internal(false);
}

/* lv_text_get_next_word() reads until NUL. With LV_TXT_LINE_BREAK_LONG_LEN at 0,
 * a word that does not fit the remaining width is rejected no matter how far
 * the word continues, but the scan still walks the rest of it. A temporary NUL
 * bounds that walk. If this line consumes the whole window, the NUL may have
 * cut a word that still fits, so the window grows and the line is retried.
 * The label wraps the same bytes, so the break stays the one it will draw. */
#if LV_TXT_LINE_BREAK_LONG_LEN > 0
#error "text view line measurement assumes LV_TXT_LINE_BREAK_LONG_LEN is 0"
#endif
static uint32_t measure_one_line(char * text, uint32_t text_len, uint32_t off,
                                 int32_t content_width, const lv_font_t * font) {
    uint32_t rem = text_len - off;
    uint32_t window = 64;
    uint32_t prev_n = 0;
    uint32_t step = 0;

    while (rem > 0) {
        uint32_t n = window > rem ? rem : window;
        if (n < rem) {
            while (n > 0 && (((uint8_t) text[off + n]) & 0xC0) == 0x80) {
                n--;
            }
            if (n == 0) {
                n = 1;
                while (n < rem && (((uint8_t) text[off + n]) & 0xC0) == 0x80) {
                    n++;
                }
            }
        }
        if (n <= prev_n && n < rem) {
            n = prev_n + 1;
            while (n < rem && (((uint8_t) text[off + n]) & 0xC0) == 0x80) {
                n++;
            }
        }

        char saved = 0;
        if (n < rem) {
            saved = text[off + n];
            text[off + n] = '\0';
        }

        lv_text_attributes_t attr;
        lv_text_attributes_init(&attr);
        attr.letter_space = 0;
        attr.line_space = 0;
        attr.max_width = content_width;
        attr.text_flags = LV_TEXT_FLAG_NONE;
        step = lv_text_get_next_line(text + off, n, font, NULL, &attr);
        if (step > n) step = n;

        if (n < rem) text[off + n] = saved;
        if (n == rem || step < n) return step;

        prev_n = n;
        if (window >= rem || window > (UINT32_MAX / 2)) {
            window = rem;
        } else {
            window *= 2;
            if (window <= n) window = n + 1;
            if (window > rem) window = rem;
        }
    }
    return step;
}

static uint32_t measure_text_page(char * text, uint32_t text_len, uint32_t start,
                                  int lines_per_page, int32_t content_width, const lv_font_t * font);

/* A picture marker is a page of its own; a text page ends before the next
 * marker. Everything else is measured by measure_text_page(). */
static uint32_t measure_one_page(char * text, uint32_t text_len, uint32_t start,
                                 int lines_per_page, int32_t content_width, const lv_font_t * font) {
    uint32_t marker = marker_at(start, NULL);
    if (marker) return start + marker;
    uint32_t window = text_len;
    if (s_image_count > 0 && start + 1 < text_len) {
        const char * esc = memchr(text + start + 1, TV_MARKER, text_len - start - 1);
        while (esc) {
            uint32_t at = (uint32_t) (esc - text);
            if (marker_at(at, NULL)) {
                window = at;
                break;
            }
            esc = at + 1 < text_len ? memchr(esc + 1, TV_MARKER, text_len - at - 1) : NULL;
        }
    }
    return measure_text_page(text, window, start, lines_per_page, content_width, font);
}

static uint32_t measure_text_page(char * text, uint32_t text_len, uint32_t start,
                                  int lines_per_page, int32_t content_width, const lv_font_t * font) {
    if (start >= text_len) {
        return text_len;
    }
    uint32_t off = start;
    uint32_t line_start = start;
    for (int line = 0; line < lines_per_page && off < text_len; line++) {
        line_start = off;
        uint32_t step = measure_one_line(text, text_len, off, content_width, font);
        if (step == 0) {
            if (off < text_len) {
                off++;
                while (off < text_len && (((uint8_t) text[off]) & 0xC0) == 0x80) {
                    off++;
                }
            }
        } else {
            off += step;
            if (off > text_len) {
                off = text_len;
            }
        }
    }

    /* show_page() writes a NUL at the page end before the label wraps it.
     * Glyph advance includes kerning with the next character, so removing
     * that character can push the last glyph onto a line the body clips.
     * Measure the last line as the label will see it and back up while the
     * terminated text no longer ends where this page does. */
    while (off > line_start && off < text_len) {
        char saved = text[off];
        text[off] = '\0';
        uint32_t step = measure_one_line(text, off, line_start, content_width, font);
        text[off] = saved;
        if (step == 0 || line_start + step >= off) break;
        off = line_start + step;
    }
    return off;
}

/* The cap stores one real page per slot. The unread suffix is not folded
 * into the last page: that page would be taller than the screen, so its
 * tail would be clipped and Next could not reach it. */
static void note_page_cap(void) {
    if (!s_scan_done && s_measured >= GUI_TEXT_VIEW_MAX_PAGES) {
        s_pages_truncated = true;
        s_scan_done = true;
    }
}

static int find_page_for_offset(size_t offset) {
    if (s_measured <= 1) return 1;
    int lo = 0;
    int hi = (int) s_measured - 1;
    int best = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (s_page_off[mid] <= offset) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best + 1;
}

static bool handle_next_page(void) {
    if (!s_open) return false;
    if (seek_still_pending()) return false;

    if (s_current_page < (int) s_measured) {
        show_page(s_current_page + 1);
        fire_on_turn();
        return true;
    }

    if (s_scan_done) {
        return false;
    }

    if (s_measured < GUI_TEXT_VIEW_MAX_PAGES) {
        uint32_t start_off = s_page_off[s_measured];
        uint32_t next_off = measure_one_page(s_text, s_text_len, start_off,
                                             s_lines_per_page, s_content_width, s_font);
        s_measured++;
        s_page_off[s_measured] = next_off;
        if (next_off >= s_text_len) {
            s_scan_done = true;
        }
    }
    note_page_cap();

    /* Timer lifetime: delete when measurement scan completes. */
    if (s_scan_done && s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }

    if (s_current_page < (int) s_measured) {
        show_page(s_current_page + 1);
        fire_on_turn();
        return true;
    }

    update_footer();
    if (s_scan_done && (int) s_measured != s_last_reported_pages) {
        fire_on_turn();
    }
    return false;
}

static bool handle_prev_page(void) {
    if (!s_open) return false;
    if (s_current_page > 1) {
        if (seek_still_pending()) return false;
        show_page(s_current_page - 1);
        fire_on_turn();
        return true;
    }

    /* Page 1 closes even mid-seek. The pending offset is still set, so
     * view_close_internal reports the requested position. */
    view_close_internal(false);
    return true;
}

static void body_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_gesture_consumed = false;
        return;
    }

    if (code == LV_EVENT_GESTURE) {
        lv_indev_t * indev = lv_event_get_indev(e);
        if (!indev) indev = lv_indev_active();
        if (!indev) return;

        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_LEFT) {
            if (handle_next_page()) {
                s_gesture_consumed = true;
                lv_indev_wait_release(indev);
            }
        } else if (dir == LV_DIR_RIGHT) {
            s_gesture_consumed = true;
            lv_indev_wait_release(indev);
            handle_prev_page();
        }
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        if (s_gesture_consumed) {
            return;
        }
        lv_indev_t * indev = lv_event_get_indev(e);
        if (!indev) indev = lv_indev_active();
        if (!indev) return;

        lv_point_t p = { 0, 0 };
        lv_indev_get_point(indev, &p);

        lv_obj_t * body = lv_event_get_target(e);
        int32_t width = lv_obj_get_width(body);
        if (width <= 0) width = s_screen_width;

        if (p.x < width / 3) {
            handle_prev_page();
        } else if (p.x >= (2 * width) / 3) {
            handle_next_page();
        }
        return;
    }
}

static void scan_timer_cb(lv_timer_t * timer) {
    if (timer != s_timer || !s_open) return;
    unsigned gen = s_generation;

    /* Background scan budget: limit each 16 ms timer tick to 6 ms of CLOCK_MONOTONIC
     * to keep UI animations and touch responsiveness intact while paginating. */
    uint64_t tick_start_us = get_time_us();
    bool page_changed = false;

    while (!s_scan_done && s_measured < GUI_TEXT_VIEW_MAX_PAGES) {
        if (get_time_us() - tick_start_us >= 6000ULL) {
            break;
        }
        uint32_t start_off = s_page_off[s_measured];
        uint32_t next_off = measure_one_page(s_text, s_text_len, start_off,
                                             s_lines_per_page, s_content_width, s_font);
        s_measured++;
        s_page_off[s_measured] = next_off;
        if (next_off >= s_text_len) {
            s_scan_done = true;
            break;
        }
    }
    note_page_cap();

    if (s_target_pending_page > 0) {
        if (s_measured >= (uint32_t) s_target_pending_page) {
            show_page(s_target_pending_page);
            s_target_pending_page = 0;
            page_changed = true;
            fire_on_turn();
            if (!s_open || gen != s_generation) return;
        } else if (s_scan_done) {
            show_page((int) s_measured);
            s_target_pending_page = 0;
            page_changed = true;
            fire_on_turn();
            if (!s_open || gen != s_generation) return;
        }
    } else if (s_target_pending_offset) {
        if (s_page_off[s_measured] > s_target_offset || s_scan_done) {
            int p = find_page_for_offset(s_target_offset);
            s_target_pending_offset = false;
            if (p != s_current_page) {
                show_page(p);
                page_changed = true;
                fire_on_turn();
                if (!s_open || gen != s_generation) return;
            }
        }
    }

    if (s_initial_turn_pending) {
        fire_on_turn();
        if (!s_open || gen != s_generation) return;
        page_changed = true;
    }

    /* Timer lifetime: delete when measurement scan completes. */
    if (s_scan_done) {
        if (s_timer) {
            lv_timer_del(s_timer);
            s_timer = NULL;
        }
    }

    update_footer();

    if (s_scan_done && !page_changed) {
        if ((int) s_measured != s_last_reported_pages) {
            fire_on_turn();
        }
    }
}

static int32_t text_view_display_height(void) {
    lv_display_t * disp = lv_display_get_default();
    int32_t screen_height = disp ? lv_display_get_vertical_resolution(disp) : BOARD_SCREEN_HEIGHT;
    if (screen_height <= 0) screen_height = BOARD_SCREEN_HEIGHT;
    return screen_height;
}

/* Font Size swaps the subtext font in place and does not rebuild this
 * screen, so each open has to resize the footer and body before pages
 * are measured. Keep the footer above the home-indicator content inset. */
static void text_view_apply_chrome(const lv_font_t * subtext_font) {
    int32_t footer_height = lv_font_get_line_height(subtext_font) + BOARD_SCALE_PX(8);
    s_body_height = text_view_display_height() - STATUS_BAR_CLEARANCE - TITLE_ROW_HEIGHT - footer_height -
                    HOME_INDICATOR_CONTENT_INSET;
    if (s_body_height < 1) s_body_height = 1;
    if (s_body) {
        lv_obj_set_size(s_body, s_screen_width, s_body_height);
    }
    if (s_footer_label) {
        lv_obj_set_size(s_footer_label, s_screen_width, footer_height);
        lv_obj_set_pos(s_footer_label, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + s_body_height);
        lv_obj_set_style_text_font(s_footer_label, subtext_font, 0);
    }
}

static lv_obj_t * build_text_view_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    if (!scr) return NULL;

    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* Why finalize_screen_navigation is not used: finalize_screen_navigation()
     * installs the app-wide swipe-right = nav_pop handler, which would steal
     * page turns and pop this screen without invoking on_close. */
    s_title_label = build_screen_header(scr, "", header_back_cb, NULL, NULL);

    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_unloaded_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_display_t * disp = lv_display_get_default();
    s_screen_width = disp ? lv_display_get_horizontal_resolution(disp) : BOARD_SCREEN_WIDTH;
    if (s_screen_width <= 0) s_screen_width = BOARD_SCREEN_WIDTH;

    s_content_width = s_screen_width - 2 * BOARD_SCALE_PX(16);
    if (s_content_width < 1) s_content_width = 1;

    s_body = lv_obj_create(scr);
    if (!s_body) {
        lv_obj_delete(scr);
        return NULL;
    }
    lv_obj_set_pos(s_body, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT);
    lv_obj_set_style_bg_opa(s_body, 0, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_outline_width(s_body, 0, 0);
    lv_obj_set_style_pad_left(s_body, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_right(s_body, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_top(s_body, 0, 0);
    lv_obj_set_style_pad_bottom(s_body, 0, 0);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_body, body_event_cb, LV_EVENT_ALL, NULL);

    s_font = gui_theme_font(GUI_FONT_ROLE_BODY);
    s_content_label = lv_label_create(s_body);
    if (!s_content_label) {
        lv_obj_delete(scr);
        return NULL;
    }
    lv_label_set_long_mode(s_content_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_content_label, s_content_width);
    lv_obj_add_style(s_content_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(s_content_label, s_font, 0);
    lv_obj_set_style_pad_all(s_content_label, 0, 0);
    lv_obj_set_style_border_width(s_content_label, 0, 0);
    lv_obj_set_style_text_letter_space(s_content_label, 0, 0);
    lv_obj_set_style_text_line_space(s_content_label, 0, 0);
    lv_obj_remove_flag(s_content_label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_content_label, "");

    s_image = lv_image_create(s_body);
    if (s_image) {
        lv_obj_remove_flag(s_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    }

    s_footer_label = lv_label_create(scr);
    if (!s_footer_label) {
        lv_obj_delete(scr);
        return NULL;
    }
    lv_obj_add_style(s_footer_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_align(s_footer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s_footer_label, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_pad_bottom(s_footer_label, BOARD_SCALE_PX(4), 0);
    lv_label_set_text(s_footer_label, "");
    text_view_apply_chrome(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));

    return scr;
}

bool gui_text_view_init(void) {
    if (s_screen) {
        gui_text_view_teardown();
    }
    s_screen = build_text_view_screen();
    if (!s_screen) {
        return false;
    }
    return true;
}

void gui_text_view_teardown(void) {
    s_teardown = true;
    s_closing = true;

    /* Timer lifetime: delete in teardown so leaked timer hazard is avoided. */
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    /* Pending on_close calls are dropped; plugin_manager_deinit() releases
     * their Lua references right after. */
    if (s_close_timer) {
        lv_timer_del(s_close_timer);
        s_close_timer = NULL;
    }
    s_pending_close_count = 0;

    if (s_text) {
        free(s_text);
        s_text = NULL;
    }
    if (s_page_off) {
        free(s_page_off);
        s_page_off = NULL;
    }

    s_on_turn = NULL;
    s_on_close = NULL;
    s_user = NULL;
    s_open = false;
    s_initial_turn_pending = false;
    images_free();

    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
        s_body = NULL;
        s_content_label = NULL;
        s_image = NULL;
        s_footer_label = NULL;
        s_title_label = NULL;
    }

    s_closing = false;
    s_teardown = false;
}

lv_obj_t * gui_text_view_get_screen(void) {
    return s_screen;
}

/* A covered view is already unloaded, so removing its stack entry does not
 * deliver another unload event. on_close stays queued until any slide in
 * progress has finished. */
static void close_if_covered_entry_gone(void) {
    if (!s_open || !s_screen) return;
    if (lv_screen_active() == s_screen) return;
    if (gui_navigation_contains(s_screen)) return;
    view_close_internal(true);
}

void gui_text_view_note_nav_removed(void) {
    if (s_teardown) return;
    close_if_covered_entry_gone();
}

bool gui_text_view_is_open(void) {
    close_if_covered_entry_gone();
    return s_open;
}

bool gui_text_view_show(const char * title, const void * text, size_t text_len,
                        const gui_text_view_opts * opts,
                        gui_text_view_turn_fn on_turn, gui_text_view_close_fn on_close,
                        void * user) {
    if (gui_text_view_is_open() || !s_screen || gui_navigation_transition_in_progress()) {
        return false;
    }

    if (s_text) {
        free(s_text);
        s_text = NULL;
    }
    if (s_page_off) {
        free(s_page_off);
        s_page_off = NULL;
    }
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }

    char title_buf[128];
    utf8_truncate_safe(title_buf, title ? title : "", sizeof(title_buf));
    if (s_title_label) {
        lv_label_set_text(s_title_label, title_buf);
    }

    size_t text_len_copied = (text && text_len > 0) ? text_len : 0;
    if (text_len_copied > GUI_TEXT_VIEW_MAX_BYTES) {
        text_len_copied = GUI_TEXT_VIEW_MAX_BYTES;
    }

    s_text = malloc(text_len_copied + 1);
    if (!s_text) {
        return false;
    }
    /* Cut a longer text on a character boundary, not mid-sequence. */
    utf8_truncate_safe_bounded(s_text, text_len_copied + 1, text, text ? text_len : 0);
    utf8_sanitize(s_text);
    s_text_len = (uint32_t) strlen(s_text); /* sanitize stops at an embedded NUL */

    images_free();
    int image_count = opts && opts->images ? opts->image_count : 0;
    if (image_count > GUI_TEXT_VIEW_MAX_IMAGES) image_count = GUI_TEXT_VIEW_MAX_IMAGES;
    if (image_count > 0) {
        s_images = calloc((size_t) image_count, sizeof(*s_images));
        if (!s_images) {
            free(s_text);
            s_text = NULL;
            return false;
        }
        for (int i = 0; i < image_count; i++) {
            s_images[i] = strdup(opts->images[i] ? opts->images[i] : "");
            if (!s_images[i]) {
                s_image_count = i;
                images_free();
                free(s_text);
                s_text = NULL;
                return false;
            }
        }
        s_image_count = image_count;
    }

    s_page_off = malloc((GUI_TEXT_VIEW_MAX_PAGES + 1) * sizeof(uint32_t));
    if (!s_page_off) {
        free(s_text);
        s_text = NULL;
        images_free();
        return false;
    }

    s_font = gui_theme_font(GUI_FONT_ROLE_BODY);
    lv_obj_set_style_text_font(s_content_label, s_font, 0);
    text_view_apply_chrome(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));

    int32_t line_height = lv_font_get_line_height(s_font);
    s_lines_per_page = line_height > 0 ? (s_body_height / line_height) : 1;
    if (s_lines_per_page < 1) s_lines_per_page = 1;

    s_on_turn = on_turn;
    s_on_close = on_close;
    s_user = user;
    s_gesture_consumed = false;

    int target_page = 1;
    bool has_target_page = false;
    size_t target_offset = 0;
    bool has_target_offset = false;

    if (opts) {
        if (opts->has_page) {
            has_target_page = true;
            target_page = opts->page < 1 ? 1 : opts->page;
        } else if (opts->has_offset) {
            has_target_offset = true;
            target_offset = opts->offset;
            if (target_offset > s_text_len) {
                target_offset = s_text_len;
            } else if (target_offset < s_text_len) {
                while (target_offset > 0 && (((uint8_t) s_text[target_offset]) & 0xC0) == 0x80) {
                    target_offset--;
                }
            }
        }
    }

    s_page_off[0] = 0;
    s_measured = 0;
    s_scan_done = false;
    s_pages_truncated = false;

    /* Synchronous measurement budget: cap at 8 ms of CLOCK_MONOTONIC to avoid
     * blocking the UI thread on show while ensuring page 1 is always ready. */
    uint64_t start_us = get_time_us();
    while (!s_scan_done && s_measured < GUI_TEXT_VIEW_MAX_PAGES) {
        if (s_measured >= 1) {
            bool target_reached = false;
            if (has_target_page) {
                if (s_measured >= (uint32_t) target_page) {
                    target_reached = true;
                }
            } else if (has_target_offset) {
                if (s_page_off[s_measured] > target_offset || s_scan_done) {
                    target_reached = true;
                }
            } else {
                target_reached = true;
            }
            if (target_reached) {
                break;
            }
            if (get_time_us() - start_us >= 8000ULL) {
                break;
            }
        }

        uint32_t start_off = s_page_off[s_measured];
        uint32_t next_off = measure_one_page(s_text, s_text_len, start_off,
                                             s_lines_per_page, s_content_width, s_font);
        s_measured++;
        s_page_off[s_measured] = next_off;
        if (next_off >= s_text_len) {
            s_scan_done = true;
            break;
        }
    }
    note_page_cap();

    int initial_page = 1;
    s_target_pending_page = 0;
    s_target_pending_offset = false;

    if (has_target_page) {
        if (s_measured >= (uint32_t) target_page) {
            initial_page = target_page;
        } else {
            initial_page = s_measured >= 1 ? (int) s_measured : 1;
            if (!s_scan_done) {
                s_target_pending_page = target_page;
            }
        }
    } else if (has_target_offset) {
        initial_page = find_page_for_offset(target_offset);
        if (!s_scan_done && s_page_off[s_measured] <= target_offset) {
            s_target_pending_offset = true;
            s_target_offset = target_offset;
        }
    } else {
        initial_page = 1;
    }

    show_page(initial_page);

    s_open = true;
    s_initial_turn_pending = true;
    s_generation++;
    nav_push(s_screen);

    /* Timer lifetime: runs in 16 ms intervals until the scan completes. It also
     * delivers the first on_turn, so no Lua runs inside the caller's call. */
    s_timer = lv_timer_create(scan_timer_cb, 16, NULL);

    return true;
}
