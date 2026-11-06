#include "player_cover.h"
#include "player_backdrop.h"
#include "player_artwork_cache.h"
#include "player_artwork_jobs.h"
#include "player_artwork_pixels.h"
#include "player_lyrics_backdrop.h"
#include "../screen/gui_player_internal.h"
#include "../screen/gui_player.h"
#include "../screen/gui_lyrics.h"
#include "../layout/player_layout_internal.h"
#include "../lyrics/player_inline_lyrics.h"
#include "../queue/player_queue_model.h"
#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../assets/assets.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../shell/gui_shell.h"
#include "../../settings/lock/gui_lock_screen.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../diagnostics/ui_frame_trace.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../../library/artwork/cache/cover_raster_cache.h"
#include "../../../library/artwork/coordinator/artwork_memory.h"
#include "../../../library/artwork/decode/cover_decode.h"
#include "../../../library/artwork/prefetch/player_cover_prefetch.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/playlists/playback_order.h"
#include "../../../network/bluetooth/media/bt_cover_art.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "lvgl/src/draw/sw/lv_draw_sw_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_timer_t * player_artwork_cache_timer;

static lv_image_dsc_t current_cover_dsc;

static uint8_t * current_cover_bytes = NULL;
/* Square edge current_cover_bytes was decoded at; valid while it is non-NULL. */
static int current_cover_edge;
static int current_cover_for_index = -1;
static cover_file_id_t current_cover_file;
static unsigned int cover_decode_partial_applied_generation;
static uint64_t current_cover_generation;

const uint8_t * player_cover_bytes(void) { return current_cover_bytes; }
int player_cover_edge(void) { return current_cover_edge; }
uint64_t player_cover_generation(void) { return current_cover_generation; }
const cover_file_id_t * player_cover_file(void) { return &current_cover_file; }

static cover_decode_request_t last_local_cover_request;
static bool last_local_cover_request_valid;

void cover_decode_request_snapshot_frost(cover_decode_request_t * req) {
    player_frost_params_t p = resolve_player_frost_params();
    req->flat = p.flat;
    req->has_bg_color = p.has_bg_color;
    req->bg_color = p.bg_color;
    req->blur_radius = p.blur_radius;
    req->blur_passes = p.blur_passes;
    req->darken_num = p.darken_num;
    req->darken_den = p.darken_den;
    req->tint_rgb = p.tint_rgb;
    req->tint_weight = p.tint_weight;
}
void gui_player_invalidate_cover_negatives(void) {
    player_artwork_jobs_invalidate_negatives();
}

void launch_cover_decode_req(cover_decode_request_t r) {
    player_cover_prefetch_cancel();
    if (r.generation == 0) {
#ifdef UI_PERF_TRACE
        r.request_perf_us = ui_perf_now_us();
#endif
        r.bt_art_token = bt_cover_art_begin_track(playlist_path_at(r.for_index));
        r.generation = player_artwork_jobs_advance_generation();
    }
    r.edge = player_cover_raster_px();
    /* Pixels decoded at another edge cannot stand in for this request. */
    cover_file_id_t shown = current_cover_bytes && current_cover_edge == r.edge
        ? current_cover_file : (cover_file_id_t) { .valid = false };
    player_artwork_jobs_partial_clear();
    cover_decode_partial_applied_generation = 0;
    if (r.local_track_path[0]) {
        last_local_cover_request = r;
        last_local_cover_request_valid = true;
    } else if (r.stream_url[0] || r.subsonic_cover_id[0]) {
        last_local_cover_request_valid = false;
    }
    player_artwork_jobs_submit(r, &shown);
}

void launch_cover_decode_from_track(int for_index, const char * track_path, const char * artist,
                                            const char * album, const char * album_artist) {
    cover_decode_request_t r = { .for_index = for_index, .generation = 0,
                                  .picture_data = NULL, .picture_size = 0,
                                  .stream_url = "", .stream_verify_tls = false };
    snprintf(r.local_track_path, sizeof(r.local_track_path), "%s", track_path ? track_path : "");
    snprintf(r.artist, sizeof(r.artist), "%s", artist ? artist : "");
    snprintf(r.album, sizeof(r.album), "%s", album ? album : "");
    snprintf(r.album_artist, sizeof(r.album_artist), "%s", album_artist ? album_artist : "");
    cover_decode_request_snapshot_frost(&r);
    launch_cover_decode_req(r);
}

/* Subsonic streaming's own art source -- see cover_decode_request_t's own
 * comment. url is subsonic_build_cover_art_url()'s output, fetched on the
 * same background thread that would otherwise be decoding already-local
 * bytes, so a slow/flaky connection can't block the UI here either, same
 * reasoning as launch_cover_decode_from_track() itself. */
void launch_cover_decode_from_url(int for_index, const char * url, bool verify_tls) {
    cover_decode_request_t r = { .for_index = for_index, .generation = 0,
                                  .picture_data = NULL, .picture_size = 0,
                                  .stream_verify_tls = verify_tls };
    snprintf(r.stream_url, sizeof(r.stream_url), "%s", url);
    cover_decode_request_snapshot_frost(&r);
    launch_cover_decode_req(r);
}

/* A Subsonic track's art, read from the cover cache by id. allow_net comes
 * from the UI thread, so offline a cached cover still shows. */
void launch_cover_decode_from_subsonic(int for_index, const subsonic_server_t * server, const char * cover_id,
                                       bool allow_net) {
    cover_decode_request_t r = { .for_index = for_index, .subsonic_server = *server,
                                  .subsonic_allow_net = allow_net };
    snprintf(r.subsonic_cover_id, sizeof(r.subsonic_cover_id), "%s", cover_id);
    cover_decode_request_snapshot_frost(&r);
    launch_cover_decode_req(r);
}

void fit_cover_img_to_card(void) {
    if (!cover_img || !cover_card) return;
    const void * source = lv_image_get_src(cover_img);
    const void * thumbnail_target = player_inline_lyrics_source_for_scaling(source);
    const void * thumbnail_source = player_cover_thumbnail ? lv_image_get_src(player_cover_thumbnail) : NULL;
    bool same_source = thumbnail_target == thumbnail_source;
    if (!same_source && thumbnail_target && thumbnail_source &&
        lv_image_src_get_type(thumbnail_target) == LV_IMAGE_SRC_FILE &&
        lv_image_src_get_type(thumbnail_source) == LV_IMAGE_SRC_FILE)
        same_source = strcmp(thumbnail_target, thumbnail_source) == 0;
    if (player_cover_thumbnail && !same_source) {
        lv_image_set_src(player_cover_thumbnail, thumbnail_target);
        lv_image_set_inner_align(player_cover_thumbnail, LV_IMAGE_ALIGN_CONTAIN);
    }
    /* lv_image_set_src already decoded the source dimensions. Reuse them
     * while the card animates instead of reopening a file on every frame. */
    int32_t source_w = lv_image_get_src_width(cover_img);
    int32_t source_h = lv_image_get_src_height(cover_img);
    if (source_w <= 0 || source_h <= 0) return;
    int32_t card_w = lv_obj_get_width(cover_card);
    int32_t card_h = lv_obj_get_height(cover_card);
    if (card_w <= 0 || card_h <= 0) {
        card_w = player_s(350);
        card_h = player_s(350);
    }
    int32_t scale_w = (card_w * LV_SCALE_NONE + source_w - 1) / source_w;
    int32_t scale_h = (card_h * LV_SCALE_NONE + source_h - 1) / source_h;
    int32_t scale = scale_w > scale_h ? scale_w : scale_h;
#if defined(BOARD_TEMPOTEC_V1)
    if (player_layout_builtin) {
        scale_w = card_w * LV_SCALE_NONE / source_w;
        scale_h = card_h * LV_SCALE_NONE / source_h;
        scale = scale_w < scale_h ? scale_w : scale_h;
        if (scale < 1) scale = 1;
    }
#endif
    lv_image_set_scale(cover_img, scale);
    lv_obj_align(cover_img, LV_ALIGN_CENTER, 0, 0);
}

void player_cover_size_changed_cb(lv_event_t * e) {
    (void) e;
    fit_cover_img_to_card();
}

static bool player_artwork_cache_idle(void) {
    /* Source pixels are immutable. Unrelated scrolling labels may continue;
     * foreground gestures and artwork/layout changes defer construction. */
    bool idle = gui_player_get_screen() && lv_display_get_screen_active(NULL) == gui_player_get_screen() &&
        backlight_screen_is_on() && !player_artwork_jobs_active() &&
        !gui_navigation_transition_in_progress() && !gui_player_lyrics_animation_in_progress() &&
        !gui_shell_boot_prompt_blocked() && !gui_player_boot_prompt_blocked() &&
        gui_popup_visible_count() == 0 && !gui_notifications_toast_visible() &&
        (!cover_img || !lv_anim_get(cover_img, NULL)) &&
        (!cover_card || !lv_anim_get(cover_card, NULL)) &&
        lv_display_get_inactive_time(NULL) >= 350;
    for (lv_indev_t * input = lv_indev_get_next(NULL); idle && input; input = lv_indev_get_next(input))
        if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) idle = false;
    return idle;
}

static void player_artwork_cache_service(lv_timer_t * timer) {
    bool idle = player_artwork_cache_idle();
    lv_draw_sw_image_cache_stats_t stats;
    lv_draw_sw_image_cache_get_intermediate_stats(&stats);
    /* Without allocations there is nothing to reclaim. Read memory only
     * when pressure can release storage or eligible idle work can start. */
    if (!stats.entries && !stats.pending && !cover_raster_cache_bytes() &&
        (!idle || !stats.qualified_candidates)) {
        if (timer) lv_timer_pause(timer);
        return;
    }
    size_t available = system_get_mem_available_bytes();
    if (available < player_raster_cache_memory_floor()) {
        player_cover_prefetch_cancel();
        cover_raster_cache_clear();
        player_reflection_cache_clear();
    }
    /* Nearly full swap is memory pressure too: MemAvailable alone stays high
     * while the process is being pushed out to swap. */
    lv_draw_sw_image_cache_service(system_swap_under_pressure() ? 0 : available, idle);
    lv_draw_sw_image_cache_get_intermediate_stats(&stats);
    if (timer && (!idle || !stats.pending)) lv_timer_pause(timer);
    if (!timer && idle && stats.pending) {
        if (!player_artwork_cache_timer)
            player_artwork_cache_timer = lv_timer_create(player_artwork_cache_service, 20, NULL);
        else lv_timer_resume(player_artwork_cache_timer);
    }
}

static void apply_decoded_cover_pixels(uint16_t * pixels, int edge, int for_index,
                                       const cover_file_id_t * file,
                                       bool accent_valid, uint32_t accent) {
    if (!pixels) return;
    player_inline_lyrics_invalidate_cover_source();
    lv_image_cache_drop(&current_cover_dsc);
    free(current_cover_bytes);
    current_cover_bytes = (uint8_t *) pixels;
    current_cover_edge = edge;
    current_cover_generation++;
    current_cover_for_index = for_index;
    current_cover_file = *file;
    memset(&current_cover_dsc, 0, sizeof(current_cover_dsc));
    current_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    current_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    current_cover_dsc.header.flags |= LV_IMAGE_FLAGS_USER3;
    current_cover_dsc.header.w = edge;
    current_cover_dsc.header.h = edge;
    current_cover_dsc.header.stride = edge * 2;
    current_cover_dsc.data = current_cover_bytes;
    current_cover_dsc.data_size = (uint32_t) edge * edge * 2;
    if (cover_img) lv_image_set_src(cover_img, &current_cover_dsc);
    fit_cover_img_to_card();
    player_lyrics_backdrop_invalidate();
    gui_shell_refresh_quick_drawer_cover();
    gui_lock_screen_refresh(true);

    /* The old reflection belongs to the old cover. Keep the background
     * empty until this request's effect pass completes; flat styling is
     * resolved from live layout state on the UI thread. */
    player_frost_params_t live = resolve_player_frost_params();
    if (live.flat) apply_player_flat_background(live.has_bg_color, live.bg_color);
    else {
        drop_current_reflection();
        player_panel_show_reflection();
    }
    gui_theme_set_cover_accent(accent_valid, accent);
    gui_lyrics_on_cover_changed(for_index);
    player_transition_mark_dirty();
}

static void poll_cover_decode_partial(void) {
    cover_decode_partial_t partial = player_artwork_jobs_take_partial();
    if (!partial.valid) return;
    bool current = !player_artwork_jobs_reload_active() &&
                   partial.generation == player_artwork_jobs_generation() &&
                   partial.for_index == player_queue_model_current();
    if (current) {
        apply_decoded_cover_pixels(partial.pixels, partial.edge, partial.for_index, &partial.file,
                                   partial.accent_valid, partial.accent);
        cover_decode_partial_applied_generation = partial.generation;
#ifdef UI_PERF_TRACE
        printf("PERF player_cover_partial_request_to_apply_ms elapsed=%u index=%d\n",
               (unsigned)((ui_perf_now_us() - partial.request_perf_us) / 1000), partial.for_index);
#endif
    } else {
        free(partial.pixels);
    }
}

/* Called from the player UI timer. Applies the finished decode to
 * cover_img/player_overlay_panel -- unless player_queue_model_current() has already moved
 * on to a different track by the time this lands (another launch_cover_
 * decode() call superseded it via cover_decode_pending, in which case that
 * one is now either running or about to be), in which case the result is
 * just discarded rather than briefly flashing a stale track's art. */
void poll_cover_decode(void) {
    /* Slides display owned snapshots. Applying artwork now cannot update
     * those frames, and a reload must retain its just-finished result until
     * finish_cover_reload() queues the forced reread. */
    if (gui_navigation_transition_in_progress() || gui_player_lyrics_animation_in_progress() ||
        player_artwork_jobs_reload_active()) return;
    poll_cover_decode_partial();
    if (!player_artwork_jobs_active()) {
        maybe_prefetch_next_cover();
        return;
    }
    if (!player_artwork_jobs_done()) return;
    player_artwork_result_t cover_result;
    if (!player_artwork_jobs_take_result(&cover_result)) return;

    bool result_generation_current = cover_result.generation == player_artwork_jobs_generation();

#ifdef UI_PERF_TRACE
    if (result_generation_current && cover_result.for_index == player_queue_model_current()) {
        printf("PERF player_cover_request_to_apply_ms elapsed=%u index=%d\n",
               (unsigned)((ui_perf_now_us() - cover_result.request_perf_us) / 1000), player_queue_model_current());
    }
#endif

    if (result_generation_current && cover_result.format.valid &&
        strcmp(cover_result.format.path, gui_player_get_current_track_path()) == 0) {
        player_track_source_set_cached_format(&cover_result.format);
        refresh_format_badge();
    }

    if (!result_generation_current || cover_result.for_index != player_queue_model_current()) {
        free(cover_result.pixels);
        free(cover_result.reflection);
    } else if (cover_result.ok && cover_result.same && current_cover_bytes &&
               current_cover_edge == cover_result.edge && cover_file_id_same(&cover_result.file, &current_cover_file)) {
        /* Same cached file as the cover on screen (next track of the same
         * album): the pixels, reflection, lock screen, drawer and accent
         * already match. Only the owning queue slot moves on. */
        free(cover_result.pixels);
        free(cover_result.reflection);
        current_cover_for_index = cover_result.for_index;
        gui_player_refresh_frosted_background();
        gui_lyrics_on_cover_changed(player_queue_model_current());
    } else if (cover_result.same) {
        /* The shown cover changed after this worker started, so the result
         * has no pixels to apply; decode this track's cover properly. */
        if (last_local_cover_request_valid && !player_artwork_jobs_pending()) {
            cover_decode_request_t retry = last_local_cover_request;
            retry.generation = 0;
            retry.picture_data = NULL;
            player_artwork_jobs_park_retry(retry);
        }
    } else if (!cover_result.ok) {
        free(cover_result.reflection);
        player_inline_lyrics_invalidate_cover_source();
        /* Retarget every live image object before releasing the backing
         * buffers. The lock screen may keep current_cover_dsc as its source,
         * so clear the descriptor before freeing its pixels. */
        if (cover_img) lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
        lv_image_cache_drop(&current_cover_dsc);
        uint8_t * old_cover_bytes = current_cover_bytes;
        current_cover_bytes = NULL;
        current_cover_generation++;
        current_cover_for_index = -1;
        current_cover_file.valid = false;
        /* current_cover_dsc.data still points at the old pixels until it is
         * cleared below -- gui_player_get_current_cover_dsc() hands this same static
         * struct's address out to other callers (the lock screen), who keep
         * referencing &current_cover_dsc for as long as they're showing. */
        current_cover_dsc.data = NULL;
        current_cover_dsc.data_size = 0;
        fit_cover_img_to_card();
        player_lyrics_backdrop_invalidate();
        /* No in-memory raw bitmap to reflect for the static placeholder
         * cover. Apply a configured flat color if set (it needs no cover
         * pixels), otherwise reset the panel back to its plain background
         * rather than leaving a stale reflection from whatever track played
         * before this one. */
        player_frost_params_t failure_params = resolve_player_frost_params();
        if (failure_params.flat) {
            apply_player_flat_background(failure_params.has_bg_color, failure_params.bg_color);
        } else {
            drop_current_reflection();
            /* Clear any flat-mode BG_COLOR left over from a previous live
             * switch away from flat -- same reasoning as gui_player_
             * refresh_frosted_background()'s own no-cover-yet branch. */
            if (player_overlay_panel)
                lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
            gui_player_update_backdrop_contrast();
        }
        free(old_cover_bytes);
        gui_lock_screen_refresh(true);
        gui_theme_set_cover_accent(false, 0);
        player_transition_mark_dirty(); /* cover_img just changed to the placeholder -- see the cache's own doc comment */
    } else {
        bool pixels_already_applied = cover_decode_partial_applied_generation == cover_result.generation;
        if (pixels_already_applied) {
            /* The UI owns the partial display copy; this original remained
             * worker-owned for reflection generation. */
            free(cover_result.pixels);
        } else {
            apply_decoded_cover_pixels(cover_result.pixels, cover_result.edge, cover_result.for_index,
                                       &cover_result.file, cover_result.accent_valid,
                                       cover_result.accent);
            cover_result.pixels = NULL;
        }

        /* The reflection this decode computed was built from frost params
         * snapshotted when the decode was LAUNCHED. If a plugin changed
         * player_layout_config while this decode was in flight (a rare
         * race, e.g. plugin.set_player_layout()/refresh_theme() firing
         * mid-decode), that stale snapshot must not silently overwrite the
         * live config the plugin just applied -- compare against the
         * current live params and only trust the worker's precomputed
         * result when they still match. */
        player_frost_params_t live_params = resolve_player_frost_params();
        bool snapshot_is_stale = (live_params.flat != cover_result.flat) ||
            (live_params.flat
                ? (live_params.has_bg_color != cover_result.has_bg_color ||
                   (live_params.has_bg_color && live_params.bg_color != cover_result.bg_color))
                : (live_params.blur_radius != cover_result.blur_radius ||
                   live_params.blur_passes != cover_result.blur_passes ||
                   live_params.darken_num != cover_result.darken_num ||
                   live_params.darken_den != cover_result.darken_den ||
                   live_params.tint_rgb != cover_result.tint_rgb ||
                   live_params.tint_weight != cover_result.tint_weight));

        if (snapshot_is_stale) {
            /* Discard the worker's now-outdated result and recompute
             * synchronously against the live config and the cover art
             * just installed above -- a one-off UI-thread recompute here
             * is the same acceptable tradeoff gui_player_refresh_frosted_
             * background()'s own live-refresh callers already make. */
            free(cover_result.reflection);
            gui_player_refresh_frosted_background();
        } else if (cover_result.flat) {
            free(cover_result.reflection);
            apply_player_flat_background(cover_result.has_bg_color, cover_result.bg_color);
        } else {
            player_backdrop_adopt_reflection(cover_result.reflection, &live_params);
        }

        /* The drawer shows this same frosted copy, so it can only be
         * refreshed once the new one has replaced the previous track's. */
        gui_shell_refresh_quick_drawer_cover();

        /* Refresh lyrics screen backdrop now that current_cover_bytes has
         * been updated with new cover art. */
        if (!pixels_already_applied)
            gui_lyrics_on_cover_changed(player_queue_model_current());
        player_transition_mark_dirty(); /* cover_img/player_overlay_panel's reflection just changed -- see the cache's own doc comment */
    }

    if (player_artwork_jobs_pending()) {
        cover_decode_request_t pending;
        if (player_artwork_jobs_take_pending(&pending))
            launch_cover_decode_req(pending); /* original submit wrapper, after result application */
    }
    maybe_prefetch_next_cover();
}

/* Keep optional cache pressure checks and idle candidate cleanup on the
 * slower system timer; decoding completion polling remains cheap. */
void gui_player_service_artwork_cache(void) {
    player_artwork_cache_service(NULL);
}

/* A fresh copy of the playing track's last local request, for decoding it
 * again. False when the last request was for another track or a stream. */
static bool take_last_local_cover_request(cover_decode_request_t * req) {
    if (!last_local_cover_request_valid || last_local_cover_request.for_index != player_queue_model_current() ||
        strcmp(last_local_cover_request.local_track_path, gui_player_get_current_track_path()) != 0)
        return false;
    *req = last_local_cover_request;
    req->picture_data = NULL;
    req->picture_size = 0;
    cover_decode_request_snapshot_frost(req);
    return true;
}

/* A rebuilt layout may show the cover at another size. Decode the shown one
 * again at the new raster edge (a no-op when it already matches). */
void player_cover_resync_raster(void) {
    cover_decode_request_t req;
    if (!current_cover_bytes || current_cover_edge == player_cover_raster_px() ||
        !take_last_local_cover_request(&req)) return;
    req.generation = 0;
    launch_cover_decode_req(req);
}

void gui_player_begin_cover_reload(void) {
    player_next_cover_prefetch_reset();
    player_artwork_jobs_begin_reload();
    player_reflection_cache_clear();
    player_artwork_jobs_partial_clear();
    /* New decodes are held (launch_cover_decode_req() parks them in
     * cover_decode_pending) until gui_player_finish_cover_reload(). One
     * already running, local or streamed, finishes normally: the reload
     * worker waits for gui_player_cover_decode_idle() before deleting any
     * file, so it cannot write an old cover back afterwards, and the UI
     * thread never blocks on an isolated extraction here. */
}

bool gui_player_cover_decode_idle(void) {
    return !!player_artwork_jobs_running_idle();
}

void gui_player_finish_cover_reload(void) {
    player_artwork_jobs_end_reload();
    /* Re-read the playing track's cover whatever album was reloaded: the
     * player's tag-derived key can differ from the library's, and one cached
     * read is cheap. The result replaces (and frees) the old cover; a pending
     * request for a newer track wins instead. */
    cover_decode_request_t req;
    /* Ownership of any pending picture_data transfers to this UI request. */
    if (!player_artwork_jobs_take_pending(&req) && !take_last_local_cover_request(&req)) return;
    if (req.local_track_path[0]) {
        /* Held or current local track alike: the cache it would hit may
         * predate the reload, so read the track's own art first and fall
         * back to the rebuilt album cover. Use the library's current tags: a
         * metadata refresh may have renamed the album since playback
         * started, and the reload rebuilt the cache under the new identity. */
        req.force_source = true;
        song_row_t row;
        if (metadata_db_get_song_by_path(req.local_track_path, &row)) {
            snprintf(req.artist, sizeof(req.artist), "%s", row.tags.artist);
            snprintf(req.album, sizeof(req.album), "%s", row.tags.album);
            snprintf(req.album_artist, sizeof(req.album_artist), "%s", row.tags.album_artist);
        }
    }
    req.generation = 0;
    launch_cover_decode_req(req);
}

void gui_player_cancel_cover_reload(void) {
    player_next_cover_prefetch_reset();
    player_artwork_jobs_end_reload();
    player_artwork_jobs_advance_generation();
    player_artwork_jobs_clear_pending();
}

bool player_inline_query_cover(player_inline_cover_info_t * info) {
    if (!info) return false;
    info->descriptor = &current_cover_dsc;
    info->has_pixels = current_cover_bytes != NULL;
    info->generation = current_cover_generation;
    return true;
}

uint8_t * gui_player_copy_cover_rgb565(int for_index, int * out_edge) {
    if (!out_edge || !current_cover_bytes || current_cover_for_index != for_index) return NULL;
    size_t bytes = (size_t) current_cover_edge * current_cover_edge * 2;
    uint8_t * copy = malloc(bytes);
    if (!copy) return NULL;
    memcpy(copy, current_cover_bytes, bytes);
    *out_edge = current_cover_edge;
    return copy;
}

const lv_image_dsc_t * gui_player_get_current_cover_dsc(uint64_t * generation) {
    if (generation) *generation = current_cover_generation;
    if (!current_cover_bytes) return NULL;
    return &current_cover_dsc;
}

void player_cover_cache_timer_pause(void) {
    if (player_artwork_cache_timer) lv_timer_pause(player_artwork_cache_timer);
}

void player_cover_cache_timer_delete(void) {
    if (player_artwork_cache_timer) {
        lv_timer_delete(player_artwork_cache_timer);
        player_artwork_cache_timer = NULL;
    }
}

void player_cover_show_retained(void) {
    if (current_cover_bytes && current_cover_dsc.data && cover_img) {
        lv_image_set_src(cover_img, &current_cover_dsc);
        fit_cover_img_to_card();
    }
}
