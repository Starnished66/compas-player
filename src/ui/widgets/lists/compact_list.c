#include "../builders/screen_builders.h"
#include "../builders/screen_builders_internal.h"
#include "../../assets/assets.h"
#include "../../../core/system/logging/debug_log.h"
#include "../../shell/gui_navigation.h"
#include "../../text/i18n/i18n.h"

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Compact list (virtualized) ---- */

/* Matches the old flex layout's own spacing (pad_top=4, pad_gap=4) --
 * explicit here instead since a virtualized list positions every row by
 * hand (lv_obj_set_pos), not through LVGL's flex engine. */
#define COMPACT_LIST_TOP_PAD GUI_ROW_GAP

/* Real row objects that exist at once, reused (repositioned + relabeled)
 * as the list scrolls, regardless of how many items the list actually
 * has -- generous enough to cover the visible viewport (~8 rows at this
 * screen height) plus overscan on both sides so a fast flick doesn't
 * visibly pop rows in/out, without ever approaching the thousands-of-
 * objects cost this whole scheme exists to avoid. */
#define COMPACT_LIST_POOL_SIZE 20

/* Paged mode -- see compact_list_set_paged_provider()'s own doc comment
 * (screen_builders.h, also where compact_list_fetch_page_cb_t/COMPACT_LIST_
 * LABEL_MAX are declared). One cached page comfortably covers the visible
 * pool plus generous overscan without re-fetching on every single-row
 * scroll -- matches the original DB-load scoping doc's own "64-128 records
 * per cached page" recommendation. Labels are copied here (fixed-size
 * buffers, not pointers) since a paged fetch's source rows (song_row_t/
 * group_row_t) are only ever short-lived stack values -- there is no
 * long-lived backing array to point into the way eager mode's items[] can. */
#define COMPACT_LIST_PAGE_CACHE_SIZE 128

typedef struct {
    compact_list_item_t * items; /* owned copy -- see build_compact_list_screen()'s own doc comment. Eager mode only (fetch_page == NULL); untouched/stale in paged mode. */
    int item_count;
    compact_list_click_cb_t on_click;
    compact_list_click_cb_t on_long_press; /* NULL for a list with no long-press action (e.g. Artists/Albums name rows) */
    /* Tracks whether a long press fired so the subsequent release click can be suppressed. */
    bool long_press_fired;
    int32_t row_height;
    int32_t requested_row_height;
    int32_t row_stride;
    uint32_t magic; /* COMPACT_LIST_MAGIC while alive; cleared on delete (stale-list guard) */
    lv_obj_t * self; /* the list widget this data belongs to */
    void * next_live; /* intrusive live-list registry link (compact_list_virtual_data_t *) */
    int window_start; /* index currently shown by rows[0]; -1 forces the first update to actually run */
    int refresh_index; /* >=0 requests a targeted repaint without disturbing the recycled window */
    lv_obj_t * rows[COMPACT_LIST_POOL_SIZE];
    lv_obj_t * leading_images[COMPACT_LIST_POOL_SIZE];
    lv_obj_t * trailing_images[COMPACT_LIST_POOL_SIZE];
    lv_obj_t * subtitle_labels[COMPACT_LIST_POOL_SIZE];
    lv_obj_t * action_circles[COMPACT_LIST_POOL_SIZE];
    void * row_ctx[COMPACT_LIST_POOL_SIZE]; /* opaque to this struct, freed alongside it -- see compact_list_row_ctx_t below */
    int row_logical_index[COMPACT_LIST_POOL_SIZE]; /* item currently represented by each recycled slot; -1 when unused */
    uint64_t row_artwork_key[COMPACT_LIST_POOL_SIZE]; /* artwork key of the item in each slot; 0 when none */
    lv_obj_t * spacer; /* repositioned by compact_list_set_items() when item_count changes */
    lv_obj_t * message;
    lv_obj_t * now_playing_bar; /* NULL if this list wasn't built with enable_now_playing -- see compact_list_set_now_playing() */
    int now_playing_index; /* -1 = nothing playing/matching in this list */

    /* NULL fetch_page = eager mode (default, existing behavior above
     * unchanged). Non-NULL = paged mode: item_count still holds the
     * logical total (from compact_list_set_paged_provider()'s own
     * total_count arg), but rows are fetched into cache_labels[] on demand
     * as the list scrolls, never all materialized at once. */
    compact_list_fetch_page_cb_t fetch_page;
    void * provider_ctx;
    int cache_start; /* requested logical offset; -1 = cache empty/invalid, forces a fetch on next window update */
    int cache_count; /* how many returned rows are valid in cache_rows[] */
    int cache_requested_count; /* requested range covered by the completed fetch, even if the provider returned fewer rows */
    compact_list_page_row_t cache_rows[COMPACT_LIST_PAGE_CACHE_SIZE];
    compact_list_row_decorator_cb_t row_decorator;
    void * row_decorator_ctx;
    compact_list_click_cb_t on_trailing_click;

    /* Background page fetch -- see compact_list_ensure_cache()'s own
     * comment for why this moved off the scroll-event/UI thread. NULL
     * when no fetch is currently in flight for this list; at most one at a
     * time (see that function's own reasoning). pending_job_started_at is
     * only meaningful while pending_job is non-NULL. poll_timer is created
     * once per list (build_compact_list_widget()) and paused whenever
     * nothing is pending, so it costs nothing while idle. */
    struct compact_list_fetch_job_s * pending_job;
    struct timespec pending_job_started_at;
    lv_timer_t * poll_timer;
} compact_list_virtual_data_t;

/* Bar width only -- height always matches LIST_ROW_HEIGHT (see
 * compact_list_set_now_playing()). Thin enough to read as an accent stripe,
 * not a second column competing with the row's own content. */
#define COMPACT_LIST_NOW_PLAYING_BAR_WIDTH 5

typedef struct {
    compact_list_virtual_data_t * data;
    int pool_slot;
    int logical_index;
} compact_list_row_ctx_t;

static void compact_list_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    compact_list_row_ctx_t * ctx = (compact_list_row_ctx_t *) lv_event_get_user_data(e);
    if (ctx->data->long_press_fired) { /* see compact_list_row_long_press_cb()'s own comment */
        ctx->data->long_press_fired = false;
        return;
    }
    int index = ctx->logical_index;
    if (index < 0 || index >= ctx->data->item_count) return; /* this pool slot is currently a hidden, contentless row (list shorter than the pool) -- shouldn't be reachable, guarded anyway */
    ctx->data->on_click(index);
}

/* Same index resolution as compact_list_row_click_cb() above, for the
 * optional long-press action (song context menu -- Add to Queue/Add to
 * Playlist) -- only ever attached (see the row-pool setup below) when the
 * caller actually passed an on_long_press, so this is never invoked for a
 * list that doesn't have one. */
static void compact_list_row_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    compact_list_row_ctx_t * ctx = (compact_list_row_ctx_t *) lv_event_get_user_data(e);
    ctx->data->long_press_fired = true;
    int index = ctx->logical_index;
    if (index < 0 || index >= ctx->data->item_count) return;
    ctx->data->on_long_press(index);
}

static void compact_list_trailing_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    compact_list_row_ctx_t * ctx = (compact_list_row_ctx_t *) lv_event_get_user_data(e);
    int index = ctx->logical_index;
    if (ctx->data->on_trailing_click && index >= 0 && index < ctx->data->item_count)
        ctx->data->on_trailing_click(index);
}

/* How often compact_list_poll_fetch_cb() below checks a pending job for
 * completion -- a DB fetch takes at least several ms, so polling every
 * frame (like the 60fps drag timers elsewhere in this file's sibling gui.c)
 * would be pure waste; this is still fast enough that the delay between a
 * fetch actually landing and the list repainting is imperceptible. */
#define COMPACT_LIST_FETCH_POLL_MS 50

/* Real-device precedent: a stuck SD-card read can land in an uninterruptible
 * kernel wait that nothing but abandoning it escapes -- see gui.c's own
 * LIBRARY_SCAN_FILE_TIMEOUT_MS comment for the same class of fault on this
 * hardware (confirmed there via /proc/<pid>/task/<tid>/status+wchan showing
 * the thread parked in __bread_gfp after a card came back from an unclean
 * unmount). Same budget here, same tolerance: past this, compact_list_poll_
 * fetch_cb() stops waiting on the job and detaches it instead of joining,
 * leaking one thread and one small allocation for the rare, genuinely-
 * corrupted-card case rather than ever blocking scrolling on it. */
#define COMPACT_LIST_FETCH_TIMEOUT_MS 5000L

/* One in-flight background page fetch -- heap-allocated separately from
 * compact_list_virtual_data_t (not embedded in it) so it can safely outlive
 * the list/data if the read is still running when the list is deleted, or
 * handed a new provider, before this one finishes (see compact_list_set_
 * paged_provider()'s/compact_list_set_items()'s own handling, and compact_
 * list_delete_event_cb()'s). The worker thread below touches only its own
 * fields here via fetch_page()'s own metadata_db.c call (thread-safe on its
 * own METADATA_DB_GUARD) -- never any LVGL object or compact_list_virtual_
 * data_t field -- so there is no cross-thread UI-safety concern; the poll
 * timer is the only place a finished job's result is ever applied, and it
 * always runs on the UI thread like every other LVGL timer callback. */
typedef struct compact_list_fetch_job_s {
    pthread_t thread;
    compact_list_fetch_page_cb_t fetch_page;
    void * provider_ctx;
    int offset;
    int count;
    compact_list_page_row_t rows[COMPACT_LIST_PAGE_CACHE_SIZE];
    /* -1 until the worker thread finishes; written last as the completion signal
     * polled by the UI thread timer. atomic_int ensures well-ordered cross-thread access. */
    atomic_int result_count;
    /* Shared reference count between the worker thread and the UI thread.
     * Released via compact_list_job_release() so whichever thread finishes last
     * frees the job struct. */
    atomic_int refs;
} compact_list_fetch_job_t;

/* Releases one of a job's two references. Whichever caller decrements to zero frees the job. */
static void compact_list_job_release(compact_list_fetch_job_t * job) {
    if (atomic_fetch_sub(&job->refs, 1) == 1) free(job);
}

static void * compact_list_fetch_worker(void * arg) {
    compact_list_fetch_job_t * job = (compact_list_fetch_job_t *) arg;
    /* Providers written before optional visual fields leave them untouched. */
    memset(job->rows, 0, sizeof(job->rows));
    job->result_count = job->fetch_page(job->provider_ctx, job->offset, job->count, job->rows);
    compact_list_job_release(job); /* the worker's own reference */
    return NULL;
}

/* Abandons a background fetch job. If already completed, joins immediately;
 * otherwise detaches the worker thread. Releases the UI-side reference in either case. */
static void compact_list_abandon_job(compact_list_fetch_job_t * job) {
    if (job->result_count >= 0) {
        pthread_join(job->thread, NULL);
    } else {
        pthread_detach(job->thread);
    }
    compact_list_job_release(job);
}

/* Abandons data->pending_job (if any) and clears it -- called whenever a
 * list is about to point at a different provider/items[] entirely (compact_
 * list_set_items()/compact_list_set_paged_provider() below), so a fetch
 * still running against the OLD provider can never land later and overwrite
 * cache_labels[] with data for a list the caller has already replaced. The
 * abandoned job's result, if it ever arrives, is simply never looked at
 * again, but the allocation itself is still safely reclaimed via
 * compact_list_job_release() -- see compact_list_abandon_job()'s own
 * comment -- just triggered by "provider changed" instead of a timeout. */
static void compact_list_cancel_pending_job(compact_list_virtual_data_t * data) {
    if (!data->pending_job) return;
    compact_list_abandon_job(data->pending_job);
    data->pending_job = NULL;
    if (data->poll_timer) lv_timer_pause(data->poll_timer);
}

/* Centers a COMPACT_LIST_PAGE_CACHE_SIZE fetch around `first` with the same
 * overscan compact_list_ensure_cache() uses for both the sync first fill
 * and the later async scroll misses. Returns the row count to request
 * (0 at the empty-list edge); writes the logical start into *out_start. */
static int compact_list_page_range(const compact_list_virtual_data_t * data, int first, int * out_start) {
    int fetch_start = first - (COMPACT_LIST_PAGE_CACHE_SIZE - COMPACT_LIST_POOL_SIZE) / 2;
    if (fetch_start < 0) fetch_start = 0;
    int max_start = data->item_count - COMPACT_LIST_PAGE_CACHE_SIZE;
    if (max_start < 0) max_start = 0;
    if (fetch_start > max_start) fetch_start = max_start;

    int want = COMPACT_LIST_PAGE_CACHE_SIZE;
    if (fetch_start + want > data->item_count) want = data->item_count - fetch_start;
    if (want < 0) want = 0;
    *out_start = fetch_start;
    return want;
}

/* Paged mode only -- kicks off a background refetch of cache_rows[] via
 * fetch_page() if the completed request doesn't already cover the real-item
 * window starting at `first` AND no fetch is currently in flight for this list.
 * Centers the new cache page around `first` with generous overscan on both
 * sides so a scroll continuing in the same direction doesn't immediately
 * fall back out of the cached range and re-fetch again next frame. One
 * fetch_page() call, not one per row -- COMPACT_LIST_PAGE_CACHE_SIZE rows at
 * once.
 *
     * Initial fills use this same worker path as scroll misses: even a bounded
     * first page can fault slow SD-backed mmap pages and must never block LVGL.
     * At most one fetch is ever in flight
 * per list (the `pending_job` check below) -- a second cache miss arriving
 * while one's already running just waits for it, rather than piling up
 * concurrent reads against the same list; the next scroll tick (or
 * compact_list_poll_fetch_cb() once it lands) tries again if the window
 * has moved further since. */
static void compact_list_ensure_cache(lv_obj_t * list, compact_list_virtual_data_t * data, int first) {
    (void) list;
    /* The pool may extend past the final item. Missing nonexistent rows
     * must not cause a completed short/tail page to refetch every tick. */
    int window_end = first + LV_MIN(COMPACT_LIST_POOL_SIZE, data->item_count - first);
    bool covered = data->cache_start >= 0 && first >= data->cache_start &&
                    window_end <= data->cache_start + data->cache_requested_count;
    if (covered) return;
    if (data->pending_job) return;

    int fetch_start = 0;
    int want = compact_list_page_range(data, first, &fetch_start);

    if (want == 0) {
        data->cache_count = 0;
        data->cache_requested_count = 0;
        data->cache_start = fetch_start;
        return;
    }

    compact_list_fetch_job_t * job = malloc(sizeof(*job));
    if (!job) return; /* next scroll tick retries */
    job->fetch_page = data->fetch_page;
    job->provider_ctx = data->provider_ctx;
    job->offset = fetch_start;
    job->count = want;
    atomic_init(&job->result_count, -1);
    atomic_init(&job->refs, 2); /* worker's own reference + whichever UI-side path resolves it -- see compact_list_fetch_job_s's own doc comment */
    if (pthread_create(&job->thread, NULL, compact_list_fetch_worker, job) != 0) {
        DBG_LOG("compact_list: pthread_create FAILED offset=%d want=%d\n", fetch_start, want);
        free(job);
        return; /* next scroll tick retries */
    }

    data->pending_job = job;
    clock_gettime(CLOCK_MONOTONIC, &data->pending_job_started_at);
    if (data->poll_timer) lv_timer_resume(data->poll_timer);
}

/* Applies only the artwork/decorator portion of a bound row. Kept shared by
 * the full row bind and the cache-change fast path so both use identical
 * thumbnail padding and image geometry. */
static void compact_list_run_row_decorator(lv_obj_t * list, compact_list_virtual_data_t * data,
                                            int slot, int index, int64_t identity,
                                            uint64_t artwork_key, const char * artwork_name,
                                            bool is_action) {
    lv_obj_t * row = data->rows[slot];
    if (data->row_decorator)
        data->row_decorator(list, row, data->leading_images[slot], index, slot,
                            identity, artwork_key, artwork_name, data->row_decorator_ctx);
    if (is_action) {
        lv_obj_set_style_pad_left(row, BOARD_SCALE_PX(100), 0);
        lv_obj_add_flag(data->leading_images[slot], LV_OBJ_FLAG_HIDDEN);
    }
}

static void compact_list_align_row_decoration(compact_list_virtual_data_t * data,
                                               int slot, bool is_action) {
    lv_obj_t * row = data->rows[slot];
    int32_t pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
    int32_t pad_right = lv_obj_get_style_pad_right(row, LV_PART_MAIN);
    int32_t pad_top = lv_obj_get_style_pad_top(row, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
    if (is_action && !data->action_circles[slot])
        data->action_circles[slot] = decorate_play_all_row(row);
    if (data->action_circles[slot]) {
        if (is_action) {
            lv_obj_remove_flag(data->action_circles[slot], LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(data->action_circles[slot], LV_ALIGN_LEFT_MID,
                         BOARD_SCALE_PX(28) - pad_left, (pad_top - pad_bottom) / -2);
        } else lv_obj_add_flag(data->action_circles[slot], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(data->leading_images[slot], LV_ALIGN_LEFT_MID,
                 14 - pad_left, (pad_top - pad_bottom) / -2);
    lv_obj_align(data->trailing_images[slot], LV_ALIGN_RIGHT_MID,
                 pad_right - 14, (pad_top - pad_bottom) / -2);
}

/* Repositions/relabels the row pool so it covers the range of items
 * actually scrolled into (or near) view -- called once up front to
 * populate the initial view, then again on every LV_EVENT_SCROLL. Cheap
 * even though it touches every pool row on each call: COMPACT_LIST_POOL_SIZE
 * label-text-and-position updates is nothing like the cost of the
 * thousands of real LVGL objects this replaces, and the early return when
 * the window hasn't actually moved (the overwhelmingly common case between
 * scroll events on this device's touch sampling rate) skips even that --
 * except in paged mode, where a stale cache (e.g. right after compact_
 * list_set_paged_provider() refreshes the data with window_start reset to
 * -1) must still be checked even if the visible window itself hasn't
 * moved, so the cache check runs before that early return. */
/* Rows rebind while scrolling, so the Breeze lookup for their trailing
 * action (title-bar art such as the menu glyph) is cached; the text color
 * picks the pre-tinted variant. */
static const char * compact_trailing_src(const char * trailing_asset) {
    static char cached_asset[64];
    static const char * cached_src;
    static lv_color_t cached_text;
    lv_style_value_t text;
    lv_color_t color = lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &text) ==
                       LV_STYLE_RES_FOUND ? text.color : lv_color_white();
    if (cached_src && lv_color_eq(color, cached_text) && !strcmp(cached_asset, trailing_asset))
        return cached_src;
    const char * stock = asset_path(trailing_asset);
    const char * breeze = titlebar_breeze_icon(stock);
    snprintf(cached_asset, sizeof(cached_asset), "%s", trailing_asset);
    cached_text = color;
    cached_src = breeze ? breeze : stock;
    return cached_src;
}

static void compact_list_update_window(lv_obj_t * list, compact_list_virtual_data_t * data) {
#ifdef UI_PERF_TRACE
    struct timespec perf_start_ts;
    clock_gettime(CLOCK_MONOTONIC, &perf_start_ts);
#endif
    int32_t scroll_y = lv_obj_get_scroll_y(list);
    if (scroll_y < 0) scroll_y = 0;
    int first = (int) (scroll_y / data->row_stride) - COMPACT_LIST_POOL_SIZE / 4;
    if (first < 0) first = 0;
    int max_first = data->item_count - COMPACT_LIST_POOL_SIZE;
    if (max_first < 0) max_first = 0;
    if (first > max_first) first = max_first;

    if (data->fetch_page) compact_list_ensure_cache(list, data, first);

    int targeted_index = data->refresh_index;
    data->refresh_index = -1;
    if (first == data->window_start && targeted_index < 0) return;
    bool force_repaint = data->window_start < 0;
    data->window_start = first;

    /* Keep rows tied to logical_index % POOL_SIZE.  Advancing the window by
     * one row then repaints only the one slot entering at the far edge,
     * instead of relabelling/redecorating all 20 rows on every scroll tick.
     * A forced refresh (window_start == -1) still repaints the whole window
     * after a page fetch, theme change, or explicit refresh request. */
    int window_end = first + COMPACT_LIST_POOL_SIZE;
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        int old_index = data->row_logical_index[slot];
        if (old_index < first || old_index >= window_end || old_index >= data->item_count) {
            lv_obj_add_flag(data->rows[slot], LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(data->leading_images[slot], NULL);
            data->row_logical_index[slot] = -1;
            ((compact_list_row_ctx_t *) data->row_ctx[slot])->logical_index = -1;
        }
    }

    for (int index = first; index < window_end && index < data->item_count; index++) {
        int slot = index % COMPACT_LIST_POOL_SIZE;
        lv_obj_t * row = data->rows[slot];
        if (!force_repaint && data->row_logical_index[slot] == index && index != targeted_index) continue;
        data->row_logical_index[slot] = index;
        ((compact_list_row_ctx_t *) data->row_ctx[slot])->logical_index = index;
        lv_obj_set_y(row, COMPACT_LIST_TOP_PAD + index * data->row_stride);
        const char * label = NULL;
        int64_t identity = 0;
        const char * trailing_asset = NULL;
        const char * subtitle = NULL;
        bool is_action = false;
        uint64_t artwork_key = 0;
        const char * artwork_name = NULL;
        if (data->fetch_page) {
            int cache_idx = index - data->cache_start;
            bool cached = cache_idx >= 0 && cache_idx < data->cache_count;
            compact_list_page_row_t * info = cached ? &data->cache_rows[cache_idx] : NULL;
            /* Hide the slot until page data arrives to avoid rendering an empty card. */
            if (!info) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
                lv_image_set_src(data->leading_images[slot], NULL);
                data->row_logical_index[slot] = -1;
                data->row_artwork_key[slot] = 0;
                ((compact_list_row_ctx_t *) data->row_ctx[slot])->logical_index = -1;
                continue;
            }
            label = info->label;
            identity = info->identity;
            trailing_asset = info->trailing_asset[0] ? info->trailing_asset : NULL;
            subtitle = info->subtitle[0] ? info->subtitle : NULL;
            is_action = info->is_action;
            artwork_key = info->artwork_key;
            artwork_name = info->artwork_name[0] ? info->artwork_name : NULL;
        } else {
            label = data->items[index].label;
            identity = data->items[index].identity;
            trailing_asset = data->items[index].trailing_asset;
            subtitle = data->items[index].subtitle;
            is_action = data->items[index].is_action;
            artwork_key = data->items[index].artwork_key;
            artwork_name = data->items[index].artwork_name;
        }
        data->row_artwork_key[slot] = artwork_key;
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t * trailing = data->trailing_images[slot];
        if (trailing_asset && trailing_asset[0]) {
            lv_image_set_src(trailing, compact_trailing_src(trailing_asset));
            lv_obj_remove_flag(trailing, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_pad_right(row, 70, 0);
        } else {
            lv_obj_add_flag(trailing, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_pad_right(row, LIST_ROW_LABEL_INSET, 0);
        }
        /* Pad/image first: lv_label_set_text() measures against the current
         * content box, and the album-art decorator's pad_left=100 is what
         * keeps the title to the right of the 72px cover. Setting the text
         * before that pad made the name lay out under the image until a
         * later thumbnail refresh invalidated the label. */
        lv_obj_set_style_pad_left(row, LIST_ROW_LABEL_INSET, 0);
        compact_list_run_row_decorator(list, data, slot, index, identity,
                                       artwork_key, artwork_name, is_action);
        lv_obj_remove_style(row, &style_theme_card_bg, 0);
        if (is_action) lv_obj_add_style(row, &style_theme_card_bg, 0);
        /* Identity details use two lines on music rows. Reset on every
         * recycled slot so the selector/single-line rows retain centering.
         * Uses data->row_height and LIST_ROW_FONT directly rather than querying
         * widget geometry before layout calculation settles. */
        row_label_set_identity(row, data->subtitle_labels[slot], label, subtitle, data->row_height);

        /* The row itself is a label, so LVGL aligns child images against
         * its padded content box rather than against the visible card.
         * Accessory padding changes per row (84px for album art, 70px for
         * a quality badge); without compensating for it the image is
         * shifted inward by that same amount and overlaps the text. Keep
         * the visual insets relative to the physical row instead. The Y
         * correction likewise cancels list_row_style's asymmetric text
         * padding so both images remain vertically centred in the card. */
        compact_list_align_row_decoration(data, slot, is_action);
    }
    bool any_visible = false;
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; ++slot)
        if (!lv_obj_has_flag(data->rows[slot], LV_OBJ_FLAG_HIDDEN)) any_visible = true;
    if (any_visible) lv_obj_add_flag(data->message, LV_OBJ_FLAG_HIDDEN);
    else {
        const char * heading = data->item_count == 0 ? TR("No items") :
                               data->pending_job ? TR("Loading...") : TR("Unable to load items");
        const char * detail = data->item_count == 0 ? TR("There are no entries in this view.") :
                              data->pending_job ? TR("Your library is being loaded.") : TR("Leave this view and try again.");
        lv_label_set_text(lv_obj_get_child(data->message, 0), heading);
        lv_label_set_text(lv_obj_get_child(data->message, 1), detail);
        lv_obj_set_y(data->message, lv_obj_get_scroll_y(list) + COMPACT_LIST_TOP_PAD);
        lv_obj_remove_flag(data->message, LV_OBJ_FLAG_HIDDEN);
    }
#ifdef UI_PERF_TRACE
    struct timespec perf_end_ts;
    clock_gettime(CLOCK_MONOTONIC, &perf_end_ts);
    long perf_us = (perf_end_ts.tv_sec - perf_start_ts.tv_sec) * 1000000L +
                   (perf_end_ts.tv_nsec - perf_start_ts.tv_nsec) / 1000L;
    if (perf_us >= 1000) {
        printf("PERF compact_window us=%ld first=%d paged=%d items=%d\n",
               perf_us, first, data->fetch_page != NULL, data->item_count);
    }
#endif
}

/* Checks data->pending_job (set by compact_list_ensure_cache() above) for
 * completion, applies the result and repaints once it lands, and pauses
 * itself again once there's nothing left to watch -- see this timer's own
 * creation comment (build_compact_list_widget()) for why it's per-list and
 * starts paused rather than a single always-on global timer. */
static void compact_list_poll_fetch_cb(lv_timer_t * timer) {
    /* Real perf finding: this timer's own memcpy/repaint work (below) is
     * exactly the kind of variable-cost work that, landing in the same
     * lv_timer_handler() tick as a live gesture's own frame-present call,
     * pushes that tick over the 16.67ms vsync budget and produces visibly
     * uneven frame pacing mid-animation -- LVGL has no timer priority
     * scheduling in this version, and its timer list is LIFO (newest-
     * created runs first), so which one wins a given tick isn't something
     * this app otherwise controls. Deferring is free: a landed job just
     * sits ready a tick or two longer, picked up the instant the
     * transition ends, since this timer keeps rescheduling itself
     * regardless (nothing below ever runs to pause it while this guard is
     * what's returning early). */
    if (gui_navigation_transition_in_progress()) return;
    lv_obj_t * list = (lv_obj_t *) lv_timer_get_user_data(timer);
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    compact_list_fetch_job_t * job = data->pending_job;
    if (!job) {
        lv_timer_pause(timer);
        return;
    }

    if (job->result_count >= 0) {
        pthread_join(job->thread, NULL); /* already returned by the time result_count landed -- instant, not a real wait */
        data->pending_job = NULL;
        memcpy(data->cache_rows, job->rows, sizeof(data->cache_rows));
        data->cache_count = LV_MIN(job->result_count, LV_MIN(job->count, COMPACT_LIST_PAGE_CACHE_SIZE));
        data->cache_start = job->offset;
        data->cache_requested_count = job->count;
        compact_list_job_release(job); /* the UI-side reference -- see compact_list_fetch_job_s's own doc comment */

        data->window_start = -1; /* force a repaint even if the visible offset didn't move meanwhile */
        compact_list_update_window(list, data);
        /* Real bug caught here: compact_list_update_window() above calls
         * compact_list_ensure_cache() against the list's CURRENT live
         * scroll position, not the one this job was fetched for -- if the
         * user kept scrolling while this fetch was in flight, that call
         * can itself spawn a brand-new pending_job and resume this exact
         * timer for it, right before this line would unconditionally pause
         * it again -- silently orphaning the new job forever (it finishes,
         * but nothing is left polling it). Only pause if nothing new got
         * queued during that repaint. */
        if (!data->pending_job) lv_timer_pause(timer);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - data->pending_job_started_at.tv_sec) * 1000L +
                      (now.tv_nsec - data->pending_job_started_at.tv_nsec) / 1000000L;
    if (elapsed_ms >= COMPACT_LIST_FETCH_TIMEOUT_MS) {
        /* See COMPACT_LIST_FETCH_TIMEOUT_MS's own comment -- a genuinely
         * stuck read, not just a slow one. Give up waiting; the next scroll
         * tick starts a fresh job once this is cleared. Rows stay blank for
         * whatever range this would have filled until then. */
        compact_list_abandon_job(job);
        data->pending_job = NULL;
        lv_timer_pause(timer);
    }
}

/* Every live compact list, as an intrusive singly linked list threaded
 * through each list's own data block: linked at creation, unlinked in the
 * LV_EVENT_DELETE handler before the block is freed. Registration therefore
 * cannot fail or allocate, and the stale-list guard below can find a live
 * list without reading the (possibly freed) object it was handed. */
static compact_list_virtual_data_t * s_live_compact_lists;

static void register_active_compact_list(compact_list_virtual_data_t * data) {
    if (!data) return;
    data->next_live = s_live_compact_lists;
    s_live_compact_lists = data;
}

static void unregister_active_compact_list(compact_list_virtual_data_t * data) {
    compact_list_virtual_data_t ** link = &s_live_compact_lists;
    while (*link) {
        if (*link == data) {
            *link = (compact_list_virtual_data_t *) data->next_live;
            data->next_live = NULL;
            return;
        }
        link = (compact_list_virtual_data_t **) &(*link)->next_live;
    }
}

#define COMPACT_LIST_MAGIC 0xC0A1157Au

/* A compact list reached after its LV_EVENT_DELETE (freed data, cleared
 * user_data or a reused allocation) is reported with the caller's return
 * address instead of crashing, so the stale call site can be found with
 * addr2line. Same log the crash handler uses. */
static void compact_list_report_stale(const char * api, void * caller) {
    int fd = open("/data/mnt/sd_0/.logs/reload_diag.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char line[160];
    int n = snprintf(line, sizeof(line), "[pid=%ld] *** STALE compact_list api=%s caller=%p ***\n",
                     (long) getpid(), api, caller);
    if (n > 0) { ssize_t w = write(fd, line, (size_t) n); (void) w; }
    close(fd);
}

/* Liveness comes from the intrusive registry: only data blocks that are
 * still linked (so still allocated) are ever read, and the widget pointer is
 * only compared, never dereferenced. A pointer compare per live list is cheap
 * enough for scroll events (unlike lv_obj_is_valid(), which walks every
 * object on every screen). If a stale pointer happens to equal a newer live
 * list's address, the call acts on that live list: valid memory, no crash. */
static compact_list_virtual_data_t * compact_list_live_data(lv_obj_t * list, const char * api, void * caller) {
    for (compact_list_virtual_data_t * d = s_live_compact_lists; list && d;
         d = (compact_list_virtual_data_t *) d->next_live) {
        if (d->self == list && d->magic == COMPACT_LIST_MAGIC) return d;
    }
    compact_list_report_stale(api, caller);
    return NULL;
}
#define COMPACT_LIST_DATA_OR_RETURN(api, ...) \
    compact_list_virtual_data_t * data = compact_list_live_data(list, api, __builtin_return_address(0)); \
    if (!data) return __VA_ARGS__

void compact_list_refresh_all(void) {
    for (compact_list_virtual_data_t * d = s_live_compact_lists; d;
         d = (compact_list_virtual_data_t *) d->next_live)
        compact_list_refresh_visible(d->self);
}

static void compact_list_scroll_event_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    COMPACT_LIST_DATA_OR_RETURN("scroll_event");
    compact_list_update_window(list, data);
}

static void compact_list_delete_event_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    compact_list_virtual_data_t * data = (compact_list_virtual_data_t *) lv_obj_get_user_data(list);
    unregister_active_compact_list(data);
    if (data->poll_timer) lv_timer_delete(data->poll_timer);
    if (data->pending_job) {
        /* Teardown must not wait on a provider: the job owns its worker
         * reference, so abandoning it can detach an unfinished worker and
         * let that worker release the allocation after its late completion.
         * Completed jobs are joined by compact_list_abandon_job(). */
        compact_list_fetch_job_t * job = data->pending_job;
        data->pending_job = NULL;
        compact_list_abandon_job(job);
    }
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) free(data->row_ctx[slot]);
    free(data->items);
    data->magic = 0;
    lv_obj_set_user_data(list, NULL);
    free(data);
}

void compact_list_scroll_to_index(lv_obj_t * list, int index) {
    COMPACT_LIST_DATA_OR_RETURN("scroll_to_index");
    lv_obj_scroll_to_y(list, COMPACT_LIST_TOP_PAD + index * data->row_stride, LV_ANIM_OFF);
}

void compact_list_set_now_playing(lv_obj_t * list, int item_index) {
    COMPACT_LIST_DATA_OR_RETURN("set_now_playing");
    if (!data->now_playing_bar) return; /* this list wasn't built with enable_now_playing */

    data->now_playing_index = item_index;
    if (item_index < 0 || item_index >= data->item_count) {
        lv_obj_add_flag(data->now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(data->now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    /* y matches a row's own position exactly (see the row pool's own
     * lv_obj_set_y() in compact_list_update_window()) -- x is fixed at 0
     * (this list's own left edge, i.e. the screen's far left, see this
     * function's own doc comment in screen_builders.h), set once at
     * creation and never touched again since it doesn't depend on scroll
     * position. LVGL's own scroll clipping hides it exactly like any other
     * child once it scrolls out of the visible viewport -- no need to track
     * the pool's current window here at all. */
    lv_obj_set_y(data->now_playing_bar, COMPACT_LIST_TOP_PAD + item_index * data->row_stride);
}

/* Swaps a build_compact_list_screen() list's contents in place -- e.g. a
 * live search filter -- without the lv_obj_delete()+rebuild this project
 * otherwise always used for that (see poll_library_rescan()'s own
 * rebuild-from-scratch precedent in gui.c). Copies `items` the same way
 * build_compact_list_screen() itself does (caller doesn't need to keep the
 * array or its strings alive afterward, though the strings themselves
 * still need to outlive this call the same way the original construction-
 * time ones do -- see build_compact_list_screen()'s own doc comment). */
void compact_list_set_items(lv_obj_t * list, const compact_list_item_t * items, int item_count) {
    COMPACT_LIST_DATA_OR_RETURN("set_items");

    compact_list_cancel_pending_job(data);
    data->fetch_page = NULL; /* switch back to eager mode -- see this field's own doc comment */
    data->provider_ctx = NULL;
    data->cache_start = -1;
    data->cache_count = 0;
    data->cache_requested_count = 0;

    free(data->items);
    data->items = NULL;
    if (item_count > 0) {
        data->items = malloc(sizeof(compact_list_item_t) * (size_t) item_count);
        memcpy(data->items, items, sizeof(compact_list_item_t) * (size_t) item_count);
    }
    data->item_count = item_count;
    data->window_start = -1; /* force compact_list_update_window() below to actually repaint */

    int32_t total_height = COMPACT_LIST_TOP_PAD + item_count * data->row_stride;
    lv_obj_set_pos(data->spacer, 0, total_height > 0 ? total_height - 1 : 0);

    /* A shorter result set than the previous scroll position would
     * otherwise leave the view showing blank space past the new (shorter)
     * scrollable range. */
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    compact_list_update_window(list, data);
}

/* Switches a list to (or refreshes) paged mode -- see this function's own
 * declaration (screen_builders.h) for the full contract. Mirrors compact_
 * list_set_items() above (same window_start reset / spacer resize / scroll-
 * to-top / repaint shape), but points the list at a fetch_page() provider
 * instead of copying a caller-owned items[] array -- rows are cached and
 * fetched on demand from compact_list_update_window() as the list scrolls,
 * never all materialized at once. Pass fetch_page == NULL to switch back to
 * eager mode (the list then shows whatever compact_list_set_items() last
 * gave it, defaulting to empty if that was never called). */
void compact_list_set_paged_provider(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, void * ctx,
                                      int total_count) {
    COMPACT_LIST_DATA_OR_RETURN("set_paged_provider");

    compact_list_cancel_pending_job(data);
    data->fetch_page = fetch_page;
    data->provider_ctx = ctx;
    data->cache_start = -1; /* forces compact_list_ensure_cache() to actually fetch on next window update */
    data->cache_count = 0;
    data->cache_requested_count = 0;
    /* NULL fetch_page: leave item_count as whatever compact_list_set_items()
     * last set (0 if never called) -- don't clobber it here. */
    if (fetch_page) data->item_count = total_count;
    data->window_start = -1; /* force compact_list_update_window() below to actually repaint */

    int32_t total_height = COMPACT_LIST_TOP_PAD + data->item_count * data->row_stride;
    lv_obj_set_pos(data->spacer, 0, total_height > 0 ? total_height - 1 : 0);

    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    compact_list_update_window(list, data);
}

/* The virtualized list widget itself, with no screen/back-button/title
 * wrapper -- factored out of build_compact_list_screen() (which now just
 * calls this against a fresh screen) so a caller that already has its own
 * screen can attach a compact list directly onto it, e.g. gui.c's Files
 * search results overlay, layered on top of build_files_screen()'s own
 * folder-browsing UI rather than needing a whole separate screen. */
lv_obj_t * build_compact_list_widget(lv_obj_t * parent, const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      int32_t row_width, bool enable_now_playing, lv_style_t * now_playing_style) {
    lv_obj_t * list = lv_obj_create(parent);
    lv_obj_set_size(list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    /* Zero container padding so absolute row positions align with screen coordinates. */
    lv_obj_set_style_pad_all(list, 0, 0);
    /* Vertical-only -- see the matching comment in build_icon_grid_screen.
     * No flex flow here (unlike the old implementation) -- every row is
     * positioned by hand as part of the virtualization scheme below. */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    screen_builders_configure_content_scrollbar(list);
    lv_obj_set_style_pad_bottom(list, BOARD_SCALE_PX(8), 0);

    compact_list_virtual_data_t * data = malloc(sizeof(*data));
    data->magic = COMPACT_LIST_MAGIC;
    data->self = list;
    data->next_live = NULL;
    data->item_count = item_count;
    data->on_click = on_click;
    data->on_long_press = on_long_press;
    data->long_press_fired = false;
    data->row_height = ui_list_row_height();
    data->requested_row_height = GUI_ROW_HEIGHT;
    data->row_stride = data->row_height + GUI_ROW_GAP;
    data->window_start = -1;
    data->refresh_index = -1;
    data->items = NULL;
    if (item_count > 0) {
        data->items = malloc(sizeof(compact_list_item_t) * (size_t) item_count);
        memcpy(data->items, items, sizeof(compact_list_item_t) * (size_t) item_count);
    }
    data->fetch_page = NULL;
    data->provider_ctx = NULL;
    data->cache_start = -1;
    data->cache_count = 0;
    data->cache_requested_count = 0;
    data->pending_job = NULL;
    data->poll_timer = NULL; /* created below, once `list` itself exists */
    data->row_decorator = NULL;
    data->row_decorator_ctx = NULL;
    data->on_trailing_click = NULL;
    lv_obj_add_flag(list, LV_OBJ_FLAG_USER_4);

    /* Virtual rows are positioned absolutely, so flex cross alignment does
     * not center them.  Center every width explicitly. */
    int32_t row_x = (lv_display_get_horizontal_resolution(lv_display_get_default()) - row_width) / 2;
    if (row_x < 0) row_x = 0;

    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        data->action_circles[slot] = NULL;
        lv_obj_t * row = lv_label_create(list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        lv_obj_set_style_height(row, data->row_height, LV_PART_MAIN);
        lv_obj_set_style_pad_top(row,
            (data->row_height - lv_font_get_line_height(&LIST_ROW_FONT)) / 2, LV_PART_MAIN);
        if (row_width != LIST_ROW_WIDTH) lv_obj_set_style_width(row, row_width, 0); /* local override -- see this param's own doc comment (screen_builders.h) */
        /* This label is also the fixed-size row/card. Explicit scroll long
         * mode prevents LVGL's default wrapping while preserving the full
         * LIST_ROW_HEIGHT background and touch target. */
        row_label_enable_marquee(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, row_x, COMPACT_LIST_TOP_PAD + slot * data->row_stride);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN); /* shown by the initial window update below once it has real content */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        data->rows[slot] = row;
        data->row_logical_index[slot] = -1;

        lv_obj_t * leading = lv_image_create(row);
        lv_obj_align(leading, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_remove_flag(leading, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(leading, LV_OBJ_FLAG_HIDDEN);
        data->leading_images[slot] = leading;

        lv_obj_t * trailing = lv_image_create(row);
        lv_obj_align(trailing, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_add_flag(trailing, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(trailing, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_ext_click_area(trailing, 18);
        lv_obj_add_flag(trailing, LV_OBJ_FLAG_HIDDEN);
        data->trailing_images[slot] = trailing;
        data->subtitle_labels[slot] = row_label_create_subtitle(row);

        compact_list_row_ctx_t * ctx = malloc(sizeof(*ctx));
        ctx->data = data;
        ctx->pool_slot = slot;
        ctx->logical_index = -1;
        data->row_ctx[slot] = ctx;
        lv_obj_add_event_cb(row, compact_list_row_click_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(trailing, compact_list_trailing_click_cb, LV_EVENT_CLICKED, ctx);
        if (on_long_press) lv_obj_add_event_cb(row, compact_list_row_long_press_cb, LV_EVENT_LONG_PRESSED, ctx);
    }

    /* 1x1 invisible spacer at the bottom of the FULL virtual list -- not
     * hidden (a hidden object doesn't count toward scrollable content
     * size, see lv_obj_get_scroll_bottom()), just visually imperceptible.
     * This alone is what gives `list` the correct scroll range for all
     * item_count rows even though only COMPACT_LIST_POOL_SIZE real row
     * objects ever exist. */
    lv_obj_t * spacer = lv_obj_create(list);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, 0, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
    int32_t total_height = COMPACT_LIST_TOP_PAD + item_count * data->row_stride;
    lv_obj_set_pos(spacer, 0, total_height > 0 ? total_height - 1 : 0);
    data->spacer = spacer;

    /* Created after the row pool (and therefore drawn on top of it, LVGL
     * z-orders siblings by creation order) so it isn't hidden behind a
     * row's own opaque background. Fixed at x=0 -- the list's own left
     * edge, i.e. the physical screen's far left -- regardless of row_x/
     * row_width above; see compact_list_set_now_playing()'s own doc
     * comment (screen_builders.h) for why this is deliberately NOT tied to
     * the row's own (possibly inset/centered) position. */
    data->now_playing_bar = NULL;
    data->now_playing_index = -1;
    if (enable_now_playing) {
        lv_obj_t * bar = lv_obj_create(list);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, COMPACT_LIST_NOW_PLAYING_BAR_WIDTH, data->row_height);
        if (now_playing_style) lv_obj_add_style(bar, now_playing_style, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, COMPACT_LIST_NOW_PLAYING_BAR_WIDTH / 2, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(bar, 0, COMPACT_LIST_TOP_PAD);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN); /* shown by compact_list_set_now_playing() once something's actually playing */
        data->now_playing_bar = bar;
    }

    lv_obj_set_user_data(list, data);
    lv_obj_add_event_cb(list, compact_list_scroll_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, compact_list_delete_event_cb, LV_EVENT_DELETE, NULL);

    /* Paged mode only ever needs this once a background fetch is actually
     * in flight (compact_list_ensure_cache() resumes it) -- created paused
     * so it costs nothing for the lifetime of an eager-mode list, or a
     * paged list between fetches. */
    data->poll_timer = lv_timer_create(compact_list_poll_fetch_cb, COMPACT_LIST_FETCH_POLL_MS, list);
    lv_timer_pause(data->poll_timer);

    data->message = build_list_message(list, TR("No items"), TR("There are no entries in this view."));
    lv_obj_set_x(data->message, (screen_builders_active_display_width() - LIST_ROW_WIDTH) / 2);
    compact_list_update_window(list, data); /* populate the initially-visible rows */
    register_active_compact_list(data);

    return list;
}

void compact_list_set_trailing_click(lv_obj_t * list, compact_list_click_cb_t cb) {
    COMPACT_LIST_DATA_OR_RETURN("set_trailing_click");
    data->on_trailing_click = cb;
}

void compact_list_set_row_decorator(lv_obj_t * list, compact_list_row_decorator_cb_t cb, void * ctx) {
    COMPACT_LIST_DATA_OR_RETURN("set_row_decorator");
    data->row_decorator = cb;
    data->row_decorator_ctx = ctx;
    compact_list_refresh_visible(list);
}

bool compact_list_detach_image_src(lv_obj_t * list, const void * src) {
    compact_list_virtual_data_t * data = list ? (compact_list_virtual_data_t *) lv_obj_get_user_data(list) : NULL;
    if (!data || !src) return false;
    bool detached = false;
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        lv_obj_t * image = data->leading_images[slot];
        if (image && lv_image_get_src(image) == src) {
            lv_image_set_src(image, NULL);
            lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
            detached = true;
        }
    }
    return detached;
}

bool compact_list_shows_artwork_key(lv_obj_t * list, uint64_t key) {
    compact_list_virtual_data_t * data = list ? (compact_list_virtual_data_t *) lv_obj_get_user_data(list) : NULL;
    if (!data || !key) return false;
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++)
        if (data->row_logical_index[slot] >= 0 && data->row_artwork_key[slot] == key) return true;
    return false;
}

void compact_list_refresh_visible(lv_obj_t * list) {
    COMPACT_LIST_DATA_OR_RETURN("refresh_visible");
    data->window_start = -1;
    compact_list_update_window(list, data);
}

void compact_list_refresh_decorations(lv_obj_t * list) {
    COMPACT_LIST_DATA_OR_RETURN("refresh_decorations");
    if (!data->row_decorator) return;

    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        int index = data->row_logical_index[slot];
        if (index < 0 || index >= data->item_count ||
            lv_obj_has_flag(data->rows[slot], LV_OBJ_FLAG_HIDDEN))
            continue;

        int64_t identity = 0;
        uint64_t artwork_key = 0;
        const char * artwork_name = NULL;
        bool is_action = false;
        bool row_data_available = true;
        if (data->fetch_page) {
            int cache_idx = index - data->cache_start;
            if (data->cache_start < 0 || cache_idx < 0 || cache_idx >= data->cache_count) {
                row_data_available = false;
            } else {
                compact_list_page_row_t * info = &data->cache_rows[cache_idx];
                identity = info->identity;
                artwork_key = info->artwork_key;
                artwork_name = info->artwork_name[0] ? info->artwork_name : NULL;
                is_action = info->is_action;
            }
        } else {
            identity = data->items[index].identity;
            artwork_key = data->items[index].artwork_key;
            artwork_name = data->items[index].artwork_name;
            is_action = data->items[index].is_action;
        }

        if (!row_data_available) {
            /* A page may have been invalidated independently of a repaint.
             * Never leave a recycled image pointing at cache-owned memory. */
            lv_image_set_src(data->leading_images[slot], NULL);
            lv_obj_add_flag(data->leading_images[slot], LV_OBJ_FLAG_HIDDEN);
            data->row_artwork_key[slot] = 0;
            continue;
        }
        data->row_artwork_key[slot] = artwork_key;
        compact_list_run_row_decorator(list, data, slot, index, identity,
                                       artwork_key, artwork_name, is_action);
        compact_list_align_row_decoration(data, slot, is_action);
    }
}

void compact_list_refresh_on_show(lv_obj_t * list) {
    COMPACT_LIST_DATA_OR_RETURN("refresh_on_show");
    if (data->fetch_page && !data->pending_job && data->window_start >= 0) {
        int first = data->window_start;
        int end = first + LV_MIN(COMPACT_LIST_POOL_SIZE, data->item_count - first);
        if (data->cache_start < 0 || first < data->cache_start ||
            end > data->cache_start + data->cache_requested_count) {
            compact_list_refresh_visible(list);
            return;
        }
    }
    compact_list_refresh_decorations(list);
}

void compact_list_refresh_item(lv_obj_t * list, int logical_index) {
    COMPACT_LIST_DATA_OR_RETURN("refresh_item");
    if (!data || logical_index < 0 || logical_index >= data->item_count) return;
    int slot = logical_index % COMPACT_LIST_POOL_SIZE;
    if (data->row_logical_index[slot] != logical_index) return; /* no longer visible */
    data->refresh_index = logical_index;
    compact_list_update_window(list, data);
}

void compact_list_set_row_height(lv_obj_t * list, int32_t row_height) {
    COMPACT_LIST_DATA_OR_RETURN("set_row_height");
    if (!data || row_height < 48) return;
    data->requested_row_height = row_height;
    int32_t minimum = row_height >= MUSIC_LIST_ROW_HEIGHT ? ui_music_row_height() :
                      lv_font_get_line_height(&LIST_ROW_FONT) + 32;
    if (row_height < minimum) row_height = minimum;
    if (row_height == data->row_height) return;

    data->row_height = row_height;
    data->row_stride = row_height + GUI_ROW_GAP;
    int32_t text_pad_top = (row_height - lv_font_get_line_height(&LIST_ROW_FONT)) / 2;
    if (text_pad_top < 0) text_pad_top = 0;
    for (int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++) {
        lv_obj_set_style_height(data->rows[slot], row_height, LV_PART_MAIN);
        lv_obj_set_style_pad_top(data->rows[slot], text_pad_top, LV_PART_MAIN);
        lv_obj_set_y(data->rows[slot], COMPACT_LIST_TOP_PAD + slot * data->row_stride);
    }
    if (data->now_playing_bar) lv_obj_set_height(data->now_playing_bar, row_height);
    int32_t total_height = COMPACT_LIST_TOP_PAD + data->item_count * data->row_stride;
    lv_obj_set_pos(data->spacer, 0, total_height > 0 ? total_height - 1 : 0);
    if (data->now_playing_bar && data->now_playing_index >= 0)
        lv_obj_set_y(data->now_playing_bar,
                     COMPACT_LIST_TOP_PAD + data->now_playing_index * data->row_stride);
    data->window_start = -1;
    compact_list_update_window(list, data);
}

void compact_list_refresh_font_layout(lv_obj_t * list) {
    compact_list_virtual_data_t * data = lv_obj_get_user_data(list);
    if (!data) return;
    int32_t old_stride = data->row_stride;
    int32_t old_y = lv_obj_get_scroll_y(list);
    compact_list_set_row_height(list, data->requested_row_height);
    if (old_stride != data->row_stride)
        lv_obj_scroll_to_y(list, (int32_t)((int64_t)old_y * data->row_stride / old_stride), LV_ANIM_OFF);
    compact_list_refresh_visible(list);
}

lv_obj_t * build_compact_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      lv_obj_t ** out_list, lv_obj_t ** out_title_label, int32_t row_width,
                                      bool enable_now_playing, lv_style_t * now_playing_style) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * title_label = build_screen_header(scr, title, back_btn_cb, NULL, NULL);
    if (out_title_label) *out_title_label = title_label;

    lv_obj_t * list = build_compact_list_widget(scr, items, item_count, on_click, on_long_press, row_width,
                                                 enable_now_playing, now_playing_style);

    if (out_list) *out_list = list;
    return scr;
}
