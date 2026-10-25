/* Host regression for the real gui_player.c track metadata transition path.
 * The existing binder fixture supplies LVGL/font/application stubs; rename
 * its main so this spec can initialize only the minimal widget tree it needs. */
#define PLAYER_ARTWORK_JOBS_TEST 1
#define main player_layout_bind_unused_main
#include "player_layout_bind_test.c"
#undef main

#include <assert.h>
#include <errno.h>
#include <unistd.h>

static int test_queue_current(void) { return player_queue_model_current(); }
static const char *test_queue_path(int index) { return player_queue_model_path_at(index); }
static void test_queue_select(int index) {
    assert(index >= 0 && index < player_queue_model_count());
    player_queue_model_select_physical(index);
}
static void test_queue_install_state(const char *const *paths, int count, int current,
                                    int pending_count) {
    char **owned = count ? calloc((size_t)count, sizeof(*owned)) : NULL;
    assert(count == 0 || owned);
    for (int i = 0; i < count; ++i) {
        owned[i] = strdup(paths[i]);
        assert(owned[i]);
    }
    player_queue_model_clear_begin();
    player_queue_model_clear_finish();
    player_queue_model_install_take((player_queue_install_t) {
        .paths = owned, .count = count, .current = current,
        .pending_count = pending_count,
        .next_insert_index = pending_count > 0 ? current + 1 + pending_count : -1,
        .queue_sd_dirfd = -1
    });
}
static void test_queue_install(const char *const *paths, int count, int current) {
    test_queue_install_state(paths, count, current, 0);
}

static track_metadata_request_t submitted_request;
static bool submitted_request_valid;
static track_metadata_request_t submitted_next_request;
static bool submitted_next_request_valid;
static bool current_result_ready;
static track_metadata_result_t current_result;
static bool next_result_ready;
static track_metadata_result_t next_result;
static unsigned submitted_count;
static unsigned cover_track_begin_count;
static unsigned next_audio_arm_count;
static unsigned next_audio_clear_count;
static unsigned cover_prefetch_submit_count;
static unsigned cover_prefetch_cancel_count;
static char cover_prefetch_submitted_path[PATH_MAX];
static char generated_cache_test_path[PATH_MAX];
static unsigned cover_decode_call_count;
static size_t test_available_memory = SIZE_MAX;
static void * test_cover_noop_worker(void * unused) { (void) unused; return NULL; }
static unsigned lyrics_load_count;
static uint64_t test_audio_generation = 1;
static bool audio_start_saw_seed;
static bool audio_start_saw_no_metadata_request;
static char audio_start_seen_title[128];
static bool test_audio_advanced_pending;
static char last_info_toast[160];
static unsigned info_toast_count;
static unsigned audio_stop_count;
static unsigned stopped_notification_count;
static bool empty_delete_notification_observed;
bool bt_is_powered_cached;
static bool display_cache_hit;
static song_row_t display_cached_row;
bool metadata_db_try_get_cached_song_by_path(const char * path, song_row_t * row) {
    if (!display_cache_hit || strcmp(path, display_cached_row.path) != 0) return false;
    *row = display_cached_row;
    return true;
}

bool player_cover_prefetch_submit(const char * path, const char * artist,
                                  const char * album, const char * album_artist,
                                  int width, int height) {
    assert(path && artist && album && album_artist);
    assert(width == COVER_ART_WIDTH && height == COVER_ART_HEIGHT);
    snprintf(cover_prefetch_submitted_path, sizeof(cover_prefetch_submitted_path), "%s", path);
    ++cover_prefetch_submit_count;
    return true;
}
void player_cover_prefetch_cancel(void) { ++cover_prefetch_cancel_count; }

bool track_metadata_submit(int lane, const track_metadata_request_t * request) {
    if (lane == TRACK_METADATA_CURRENT) {
        submitted_request = *request;
        submitted_request_valid = true;
        ++submitted_count;
    } else {
        assert(lane == TRACK_METADATA_NEXT);
        submitted_next_request = *request;
        submitted_next_request_valid = true;
    }
    return true;
}

bool track_metadata_take(int lane, track_metadata_result_t * result) {
    if (!result) return false;
    if (lane == TRACK_METADATA_CURRENT) {
        if (!current_result_ready) return false;
        *result = current_result;
        current_result_ready = false;
        memset(&current_result, 0, sizeof(current_result));
        return true;
    }
    if (lane != TRACK_METADATA_NEXT || !next_result_ready) return false;
    *result = next_result;
    next_result_ready = false;
    memset(&next_result, 0, sizeof(next_result));
    return true;
}

bool track_metadata_count_play(const char * path) { (void)path; return true; }
bool track_metadata_worker_start(void) { return true; }
bool track_metadata_history_failed(void) { return false; }

bool remote_track_path_is_remote(const char * path) {
    return path && strncmp(path, "remote://", 9) == 0;
}
bool remote_track_meta_copy_for_path(const char * path, remote_track_meta_t * out) {
    (void)path;
    if (out) memset(out, 0, sizeof(*out));
    return false;
}

void gui_lyrics_load_track(int index, const char * path) { (void)index; (void)path; ++lyrics_load_count; }
void gui_lyrics_on_cover_changed(int index) { (void)index; }
void gui_shell_refresh_quick_drawer_cover(void) {}
void player_lyrics_backdrop_invalidate(void) {}
void gui_theme_set_cover_accent(bool valid, uint32_t rgb) { (void)valid; (void)rgb; }
uint64_t bt_cover_art_begin_track(const char * path) {
    (void)path;
    return ++cover_track_begin_count;
}
void gui_track_info_set_current(const gui_track_info_context_t * info) { (void)info; }
void player_seekbar_set_value(lv_obj_t * slider, int value) {
    if (slider) lv_slider_set_value(slider, value, LV_ANIM_OFF);
}
int32_t player_seekbar_get_value(lv_obj_t * slider) {
    return slider ? lv_slider_get_value(slider) : 0;
}
void player_seekbar_update(lv_obj_t * slider, const waveform_data_t * data) {
    (void)slider; (void)data;
}
bool gui_theme_prefers_light_text(lv_color_t bg) { (void)bg; return false; }
float gui_theme_contrast_ratio(lv_color_t a, lv_color_t b) { (void)a; (void)b; return 1.0f; }
bool gui_theme_color_is_dark(lv_color_t color) { (void)color; return false; }
bool player_transport_images_set_glyph_on_light(bool value) { (void)value; return true; }
void gui_shell_set_status_bar_screen_context(lv_obj_t * screen) { (void)screen; }
bool player_cover_accent_from_rgb565(const uint16_t * pixels, uint32_t * rgb) {
    (void)pixels; (void)rgb; return false;
}
uint8_t * player_compute_reflection_bytes_cancelable(const uint8_t * cover, int blur_radius,
        int blur_passes, int darken_num, int darken_den, uint32_t tint_rgb,
        int tint_weight, frosted_glass_cancel_fn cancel_cb, void * user_data) {
    (void)cover; (void)blur_radius; (void)blur_passes; (void)darken_num;
    (void)darken_den; (void)tint_rgb; (void)tint_weight; (void)cancel_cb; (void)user_data;
    return NULL;
}
void gui_player_prepare_play_btn_images(void) {}
const void * gui_player_play_btn_image_src(bool is_playing) { (void)is_playing; return NULL; }
bool remote_track_stream_is_fresh(const char * path) { (void)path; return false; }
bool remote_track_stream_prefetch(const char * path, uint64_t generation) {
    (void)path; (void)generation; return false;
}
bool audio_get_current_format_info(audio_current_format_info_t * info) {
    if (info) memset(info, 0, sizeof(*info));
    return false;
}
uint64_t audio_get_playback_generation(void) { return test_audio_generation; }
void audio_play_file_at_with_replaygain_mode(const char * path, double start_seconds, int mode) {
    (void)path; (void)start_seconds; (void)mode;
    snprintf(audio_start_seen_title, sizeof(audio_start_seen_title), "%s", lv_label_get_text(song_title_label));
    audio_start_saw_seed = true;
    audio_start_saw_no_metadata_request = !submitted_request_valid;
    ++test_audio_generation;
}
void audio_play_file_at(const char * path, double start_seconds, bool has_gain, double gain_db,
                        bool has_peak, double peak) {
    (void)path; (void)start_seconds; (void)has_gain; (void)gain_db; (void)has_peak; (void)peak;
    snprintf(audio_start_seen_title, sizeof(audio_start_seen_title), "%s", lv_label_get_text(song_title_label));
    audio_start_saw_seed = true;
    audio_start_saw_no_metadata_request = !submitted_request_valid;
    ++test_audio_generation;
}
bool airplay_control_disconnect_active_stream(void) { return false; }
void show_error_toast(const char * message) { (void)message; }
void show_info_toast(const char * message) {
    snprintf(last_info_toast, sizeof(last_info_toast), "%s", message ? message : "");
    ++info_toast_count;
}
void gui_popup_hide(gui_popup_t *popup) {
    if (popup && popup->popup) lv_obj_add_flag(popup->popup, LV_OBJ_FLAG_HIDDEN);
}
void audio_stop(void) { ++audio_stop_count; }
void plugin_manager_notify_stopped(void) {
    ++stopped_notification_count;
    if (gui_player_get_playlist_count() == 0 && gui_player_get_playlist_index() == 0)
        empty_delete_notification_observed = true;
}
void nav_push(lv_obj_t * screen) { (void)screen; }
void nav_pop(void) {}
void settings_save_async(const player_settings_t * settings) { (void)settings; }
bool audio_set_next_track_for_generation(const char * path, bool has_gain, double gain,
                                         bool has_peak, double peak, uint64_t generation) {
    (void)path; (void)has_gain; (void)gain; (void)has_peak; (void)peak; (void)generation;
    ++next_audio_arm_count;
    return true;
}
void audio_set_next_track(const char * path, bool has_gain, double gain,
                          bool has_peak, double peak) {
    (void)has_gain; (void)gain; (void)has_peak; (void)peak;
    if (path) ++next_audio_arm_count;
    else ++next_audio_clear_count;
}
bool audio_get_playback_progress(const char * path, audio_playback_progress_t * progress) {
    (void)path;
    if (progress) memset(progress, 0, sizeof(*progress));
    return false;
}
bool audio_consume_track_advanced(void) {
    bool advanced = test_audio_advanced_pending;
    test_audio_advanced_pending = false;
    return advanced;
}
bool player_layouts_timeline_refresh_timing(lv_anim_timeline_t * timeline) { (void)timeline; return false; }
uint8_t * frosted_glass_blur_rgb565(const uint16_t * source, int source_width, int source_height,
                                    int out_width, int out_height, int blur_radius, int blur_passes,
                                    int darken_num, int darken_den, bool mirror_y) {
    (void)source; (void)source_width; (void)source_height; (void)out_width; (void)out_height;
    (void)blur_radius; (void)blur_passes; (void)darken_num; (void)darken_den; (void)mirror_y;
    return NULL;
}
uint8_t * frosted_glass_blur_rgb565_cancelable(const uint16_t * source, int source_width, int source_height,
                                               int out_width, int out_height, int blur_radius, int blur_passes,
                                               int darken_num, int darken_den, bool mirror_y,
                                               frosted_glass_cancel_fn cancel_cb, void * user_data) {
    if (cancel_cb && cancel_cb(user_data)) return NULL;
    return frosted_glass_blur_rgb565(source, source_width, source_height, out_width, out_height,
                                     blur_radius, blur_passes, darken_num, darken_den, mirror_y);
}
uint8_t * frosted_glass_blur_rgb565_tinted_cancelable(const uint16_t * source, int source_width, int source_height,
                                                      int out_width, int out_height, int blur_radius, int blur_passes,
                                                      int darken_num, int darken_den, uint32_t tint_rgb, int tint_weight,
                                                      bool mirror_y, frosted_glass_cancel_fn cancel_cb, void * user_data) {
    (void)tint_rgb; (void)tint_weight;
    return frosted_glass_blur_rgb565_cancelable(source, source_width, source_height, out_width, out_height,
                                                blur_radius, blur_passes, darken_num, darken_den, mirror_y,
                                                cancel_cb, user_data);
}
size_t artwork_reserve_bytes_for_priority(artwork_priority_t priority) { (void)priority; return 0; }
bool albumart_generated_cache_fresh(const albumart_info_t * info, int width, int height,
                                    char * found, size_t found_size) {
    (void)info; (void)width; (void)height;
    if (!generated_cache_test_path[0] || !found || !found_size) return false;
    snprintf(found, found_size, "%s", generated_cache_test_path);
    return true;
}
bool albumart_generated_cache_find(const albumart_info_t * info, int width, int height,
                                   char * found, size_t found_size) {
    (void)info; (void)width; (void)height; (void)found; (void)found_size; return false;
}
bool bt_cover_art_reuse(uint64_t token, uint64_t image_key) { (void)token; (void)image_key; return false; }
size_t system_get_mem_available_bytes(void) { return test_available_memory; }
void cover_raster_cache_clear(void) {}
uint16_t * cover_raster_cache_get(const cover_raster_key_t * key) { (void)key; return NULL; }
void cover_raster_cache_put(const cover_raster_key_t * key, const uint16_t * pixels) { (void)key; (void)pixels; }
size_t cover_raster_cache_bytes(void) { return 0; }
albumart_load_result_t albumart_load_file_ex(const char * path, uint8_t ** out_data,
                                              uint32_t * out_size, uint32_t max_bytes,
                                              artwork_priority_t priority) {
    (void)path; (void)out_data; (void)out_size; (void)max_bytes; (void)priority; return ALBUMART_LOAD_INVALID;
}
cover_decode_result_t cover_decode_to_rgb565_ex(const uint8_t * data, uint32_t size,
                                                int target_w, int target_h, artwork_priority_t priority,
                                                artwork_cancel_fn cancel_cb, void * user_data,
                                                uint16_t ** out_pixels) {
    (void)data; (void)size; (void)target_w; (void)target_h; (void)priority;
    (void)cancel_cb; (void)user_data; (void)out_pixels; ++cover_decode_call_count;
    return COVER_DECODE_FAIL_UNSUPPORTED;
}
static pthread_mutex_t artwork_worker_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t artwork_worker_cond=PTHREAD_COND_INITIALIZER;
static bool artwork_worker_enabled, artwork_worker_a_started, artwork_worker_release_a;
static unsigned artwork_worker_http_calls, artwork_worker_decode_calls;
static void * artwork_worker_body_a, *artwork_worker_body_c, *artwork_worker_payload_b;
static unsigned artwork_worker_body_a_frees, artwork_worker_body_c_frees, artwork_worker_payload_b_frees;
static void * artwork_worker_pixel_ptrs[8];
static unsigned artwork_worker_pixel_frees[8], artwork_worker_pixel_count;
static uint16_t * artwork_worker_last_pixels;
void __real_free(void *ptr);
void __wrap_free(void *ptr) {
    if (ptr) {
        if(ptr==artwork_worker_body_a) { ++artwork_worker_body_a_frees; artwork_worker_body_a=NULL; }
        if(ptr==artwork_worker_body_c) { ++artwork_worker_body_c_frees; artwork_worker_body_c=NULL; }
        if(ptr==artwork_worker_payload_b) { ++artwork_worker_payload_b_frees; artwork_worker_payload_b=NULL; }
        for(unsigned i=0;i<artwork_worker_pixel_count;i++) if(ptr==artwork_worker_pixel_ptrs[i]) {
            ++artwork_worker_pixel_frees[i]; artwork_worker_pixel_ptrs[i]=NULL;
        }
    }
    __real_free(ptr);
}
bool cover_decode_to_rgb565(const uint8_t * data, uint32_t size, int target_w, int target_h,
                            uint16_t ** out_pixels) {
    (void)data; (void)size;
    if(!artwork_worker_enabled) { (void)target_w; (void)target_h; (void)out_pixels; return false; }
    ++artwork_worker_decode_calls;
    uint16_t *pixels=calloc((size_t)target_w*target_h,sizeof(uint16_t)); assert(pixels);
    pixels[0]=(uint16_t)artwork_worker_decode_calls;
    assert(artwork_worker_pixel_count<8);artwork_worker_pixel_ptrs[artwork_worker_pixel_count++]=pixels;
    artwork_worker_last_pixels=pixels;*out_pixels=pixels;return true;
}
bool http_get_to_buffer(const char * url, bool verify_tls, int * out_status,
                        uint8_t ** out_body, size_t * out_body_size) {
    (void)verify_tls;
    if(!artwork_worker_enabled) { (void)url; (void)out_status; (void)out_body; (void)out_body_size; return false; }
    pthread_mutex_lock(&artwork_worker_mutex);++artwork_worker_http_calls;
    bool hold=strstr(url,"hold-a")!=NULL;
    if(hold) {
        artwork_worker_a_started=true;pthread_cond_broadcast(&artwork_worker_cond);
        while(!artwork_worker_release_a)pthread_cond_wait(&artwork_worker_cond,&artwork_worker_mutex);
    }
    pthread_mutex_unlock(&artwork_worker_mutex);
    uint8_t *body=malloc(8);assert(body);memcpy(body,"artwork",8);
    if(hold)artwork_worker_body_a=body;else if(strstr(url,"track-c"))artwork_worker_body_c=body;
    *out_status=200;*out_body=body;*out_body_size=8;return true;
}
static void wait_artwork_worker_a(void) {
    pthread_mutex_lock(&artwork_worker_mutex);
    while(!artwork_worker_a_started)pthread_cond_wait(&artwork_worker_cond,&artwork_worker_mutex);
    pthread_mutex_unlock(&artwork_worker_mutex);
}
static void release_artwork_worker_a(void) {
    pthread_mutex_lock(&artwork_worker_mutex);artwork_worker_release_a=true;
    pthread_cond_broadcast(&artwork_worker_cond);pthread_mutex_unlock(&artwork_worker_mutex);
}
static void wait_artwork_worker_done(void) {
    for(unsigned i=0;i<5000;i++) {
        if(atomic_load_explicit(&cover_decode_done_flag,memory_order_acquire))return;
        usleep(1000);
    }
    assert(!"artwork worker timed out");
}
bool bt_cover_art_publish_rgb565_async(uint64_t token, const uint16_t * pixels,
                                 int width, int height, uint64_t image_key) {
    (void)token; (void)pixels; (void)width; (void)height; (void)image_key; return false;
}
void bt_cover_art_clear(uint64_t token) { (void)token; }
void ui_wake_notify(void) {}
void db_log(const char * area, const char * fmt, ...) { (void)area; (void)fmt; }
bool db_log_enabled(void) { return false; }
bool gui_library_generate_player_cover(const char * path, const char * artist,
                                       const char * album, const char * album_artist,
                                       artwork_cancel_fn cancel_cb, void * user_data,
                                       uint16_t ** out_pixels, bool * out_no_art_confirmed) {
    (void)path; (void)artist; (void)album; (void)album_artist;
    (void)cancel_cb; (void)user_data; (void)out_pixels; (void)out_no_art_confirmed;
    return false;
}
void favorite_writer_submit(const char * path, bool is_favorite) { (void)path; (void)is_favorite; }
subsonic_stream_song_meta_t * subsonic_stream_meta;
int subsonic_stream_meta_count;
void metadata_normalize_text_tags(track_metadata_t * meta) { (void)meta; }
void refresh_now_playing_indicators(void) {}
void gui_lock_screen_refresh(bool artwork_changed) { (void)artwork_changed; }
void plugin_manager_notify_track_started(const char * title, const char * artist, const char * album,
                                         double duration_seconds, const char * provider, const char * track_id) {
    (void)title; (void)artist; (void)album; (void)duration_seconds; (void)provider; (void)track_id;
}
size_t utf8_truncate_safe(char * dst, const char * src, size_t max_bytes) {
    if (!dst || !max_bytes) return 0;
    snprintf(dst, max_bytes, "%s", src ? src : "");
    return strlen(dst);
}
void utf8_sanitize(char * text) { (void)text; }
bool metadata_db_snapshot_path_at(const metadata_db_snapshot_t * snapshot, int rank,
                                  char * out, size_t out_size) {
    (void)snapshot; (void)rank; if (out && out_size) out[0] = '\0'; return false;
}
void metadata_db_snapshot_close(metadata_db_snapshot_t *snapshot) { (void)snapshot; }
bool file_browser_index_playable_path_at(const file_browser_index_t * index, unsigned ordinal,
                                         char * out, size_t out_size) {
    (void)index; (void)ordinal; if (out && out_size) out[0] = '\0'; return false;
}
void file_browser_index_close(file_browser_index_t *index) { (void)index; }
void free_group_song_entries(group_song_entry_t *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; ++i) {
        free(entries[i].path);
        free(entries[i].title);
    }
    free(entries);
}
static void setup_widgets(void) {
    lv_init();
    assert(ui_fonts_init());
    lv_display_t * display = lv_display_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    static uint8_t buffer[BOARD_SCREEN_WIDTH * 32 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_test);

    player_screen = lv_obj_create(NULL);
    song_title_label = lv_label_create(player_screen);
    artist_label = lv_label_create(player_screen);
    album_label = lv_label_create(player_screen);
    format_badge_label = lv_label_create(player_screen);
    pos_label = lv_label_create(player_screen);
    dur_label = lv_label_create(player_screen);
    song_count_label = lv_label_create(player_screen);
    progress_slider = lv_slider_create(player_screen);
    cover_img = lv_image_create(player_screen);
    lv_screen_load(player_screen);
    lv_obj_update_layout(player_screen);
    const char * paths[] = { "/music/Artist/First.flac", "/music/Artist/Second.flac" };
    test_queue_install(paths, 2, 0);
}

static void reset_art_queue(void) {
    free(cover_decode_pending.picture_data);
    memset(&cover_decode_pending, 0, sizeof(cover_decode_pending));
    cover_decode_pending_valid = false;
    artwork_dispatched_metadata_token = 0;
    atomic_store_explicit(&cover_reload_active, true, memory_order_relaxed);
    submitted_request_valid = false;
    submitted_next_request_valid = false;
    submitted_count = 0;
    current_result_ready = false;
    memset(&current_result, 0, sizeof(current_result));
    next_result_ready = false;
    memset(&next_result, 0, sizeof(next_result));
}

static void release_next_result(void) {
    assert(submitted_next_request_valid);
    next_result.request = submitted_next_request;
    snprintf(next_result.request.metadata.title, sizeof(next_result.request.metadata.title), "Prefetched next");
    next_result.request.metadata.has_title = true;
    next_result_ready = true;
    gui_player_poll_track_metadata();
    assert(!next_result_ready);
}

static void test_next_prefetch_is_independent_of_gapless(void) {
    reset_art_queue();
    test_queue_select(0);
    current_settings.gapless_enabled = false;
    next_audio_arm_count = 0;
    unsigned clears_before = next_audio_clear_count;

    arm_next_track_for_audio(test_queue_current());
    assert(submitted_next_request_valid);
    assert(strcmp(submitted_next_request.path, test_queue_path(1)) == 0);
    assert(next_audio_arm_count == 0);
    assert(next_audio_clear_count > clears_before);
    release_next_result();
    assert(next_metadata_cached);
    assert(strcmp(next_metadata_cached_path, test_queue_path(1)) == 0);
    assert(strcmp(next_metadata_cache.title, "Prefetched next") == 0);
    assert(next_audio_arm_count == 0);

    current_settings.gapless_enabled = true;
    arm_next_track_for_audio(test_queue_current());
    unsigned arms_before_result = next_audio_arm_count;
    release_next_result();
    assert(next_audio_arm_count == arms_before_result + 1);

    /* Turning gapless off while a request is in flight must keep its useful
     * metadata result while preventing a late audio handoff from being armed. */
    arm_next_track_for_audio(test_queue_current());
    assert(submitted_next_request_valid);
    arms_before_result = next_audio_arm_count;
    current_settings.gapless_enabled = false;
    release_next_result();
    assert(next_metadata_cached && strcmp(next_metadata_cached_path, test_queue_path(1)) == 0);
    assert(next_audio_arm_count == arms_before_result);

    /* Re-arming invalidates both cached identity and any outstanding NEXT token. */
    arm_next_track_for_audio(test_queue_current());
    assert(!next_metadata_cached);
    assert(next_metadata_cached_path[0] == '\0');
    assert(submitted_next_request_valid);

    track_metadata_request_t stale_request = submitted_next_request;
    arm_next_track_for_audio(test_queue_current());
    next_result.request = stale_request;
    next_result_ready = true;
    gui_player_poll_track_metadata();
    assert(!next_result_ready);
    assert(!next_metadata_cached);

    const char *one_path[] = { "/music/Artist/First.flac" };
    test_queue_install(one_path, 1, 0);
    next_metadata_cached = true;
    snprintf(next_metadata_cached_path, sizeof(next_metadata_cached_path), "%s",
             "/music/Artist/Second.flac");
    uint64_t old_token = metadata_next_token;
    clears_before = next_audio_clear_count;
    submitted_next_request_valid = false;
    arm_next_track_for_audio(test_queue_current());
    assert(metadata_next_token != old_token);
    assert(!next_metadata_cached && next_metadata_cached_path[0] == '\0');
    assert(next_audio_clear_count > clears_before);
    assert(!submitted_next_request_valid);
    const char *restored_paths[] = {
        "/music/Artist/First.flac", "/music/Artist/Second.flac"
    };
    test_queue_install(restored_paths, 2, 0);
}

static void prepare_settled_next_cover(void) {
    reset_art_queue();
    test_queue_select(0);
    metadata_ui_token = 91;
    snprintf(metadata_ui_path, sizeof(metadata_ui_path), "%s", test_queue_path(0));
    artwork_dispatched_metadata_token = metadata_ui_token;
    cover_decode_active = false;
    cover_decode_pending_valid = false;
    atomic_store_explicit(&cover_decode_done_flag, false, memory_order_relaxed);
    atomic_store_explicit(&cover_reload_active, false, memory_order_relaxed);
    next_cover_prefetch_token = 0;
    cover_prefetch_submit_count = 0;
    cover_prefetch_cancel_count = 0;
    cover_prefetch_submitted_path[0] = '\0';
}

static void test_next_cover_prefetch_waits_for_foreground_and_cancels(void) {
    prepare_settled_next_cover();
    current_settings.gapless_enabled = false;
    arm_next_track_for_audio(test_queue_current());
    release_next_result();
    assert(next_metadata_cached);

    poll_cover_decode();
    assert(cover_prefetch_submit_count == 1);
    assert(strcmp(cover_prefetch_submitted_path, test_queue_path(1)) == 0);
    poll_cover_decode();
    assert(cover_prefetch_submit_count == 1);

    unsigned cancels_before = cover_prefetch_cancel_count;
    arm_next_track_for_audio(test_queue_current());
    assert(cover_prefetch_cancel_count > cancels_before);
    assert(!next_metadata_cached);
    poll_cover_decode();
    assert(cover_prefetch_submit_count == 1);

    /* Each foreground blocker prevents speculation until the UI is settled. */
    next_metadata_cached = true;
    snprintf(next_metadata_cached_path, sizeof(next_metadata_cached_path), "%s", test_queue_path(1));
    snprintf(metadata_next_current_path, sizeof(metadata_next_current_path), "%s", test_queue_path(0));
    cover_decode_active = true;
    poll_cover_decode();
    assert(cover_prefetch_submit_count == 1);
    cover_decode_active = false;
    cover_decode_pending_valid = true;
    poll_cover_decode();
    assert(cover_prefetch_submit_count == 1);
    cover_decode_pending_valid = false;
    atomic_store_explicit(&cover_reload_active, true, memory_order_relaxed);
    poll_cover_decode();
    assert(cover_prefetch_submit_count == 1);
    atomic_store_explicit(&cover_reload_active, false, memory_order_relaxed);
}

static void test_generated_cover_same_file_identity_skips_decode(void) {
    char path_template[PATH_MAX];
    snprintf(path_template, sizeof(path_template), "build_ui_test/player-cover-identity-%ld-XXXXXX",
             (long) getpid());
    int fd = mkstemp(path_template);
    assert(fd >= 0);
    const char bytes[] = "cover-id";
    assert(write(fd, bytes, sizeof(bytes)) == (ssize_t) sizeof(bytes));
    assert(close(fd) == 0);
    snprintf(generated_cache_test_path, sizeof(generated_cache_test_path), "%s", path_template);

    struct stat st;
    assert(stat(generated_cache_test_path, &st) == 0);
    cover_file_id_t shown = { .valid = true, .dev = st.st_dev, .ino = st.st_ino,
        .mtime_sec = st.st_mtim.tv_sec, .mtime_nsec = st.st_mtim.tv_nsec,
        .ctime_sec = st.st_ctim.tv_sec, .ctime_nsec = st.st_ctim.tv_nsec, .size = st.st_size };
    cover_file_id_t out_id = {0};
    bool same = false;
    uint16_t *pixels = NULL;
    albumart_info_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", test_queue_path(0));
    unsigned decodes_before = cover_decode_call_count;
    assert(load_generated_player_cover(&info, test_queue_path(1), 17, 23, true, &shown,
                                       &out_id, &same, &pixels));
    assert(same && out_id.valid && cover_file_id_same(&shown, &out_id));
    assert(!pixels && cover_decode_call_count == decodes_before);

    char changed_path[PATH_MAX];
    snprintf(changed_path, sizeof(changed_path), "build_ui_test/player-cover-identity-%ld-XXXXXX",
             (long) getpid());
    fd = mkstemp(changed_path);
    assert(fd >= 0);
    assert(write(fd, bytes, sizeof(bytes)) == (ssize_t) sizeof(bytes));
    assert(close(fd) == 0);
    snprintf(generated_cache_test_path, sizeof(generated_cache_test_path), "%s", changed_path);
    out_id = (cover_file_id_t) {0};
    same = false;
    assert(!load_generated_player_cover(&info, test_queue_path(1), 18, 24, true, &shown,
                                        &out_id, &same, &pixels));
    assert(!same);

    assert(unlink(path_template) == 0);
    assert(unlink(changed_path) == 0);
    generated_cache_test_path[0] = '\0';
}

static void poll_held_current_result(void) {
    assert(submitted_request_valid);
    assert(!current_result_ready);
    gui_player_poll_track_metadata();
}

static void release_and_poll_current_result(bool favorite) {
    assert(submitted_request_valid);
    current_result.request = submitted_request;
    current_result.favorite = favorite;
    current_result_ready = true;
    gui_player_poll_track_metadata();
    assert(!current_result_ready);
}

static void test_cached_next_identity_and_art(void) {
    reset_art_queue();
    next_metadata_cached = true;
    snprintf(next_metadata_cached_path, sizeof(next_metadata_cached_path), "%s", test_queue_path(0));
    memset(&next_metadata_cache, 0, sizeof(next_metadata_cache));
    snprintf(next_metadata_cache.title, sizeof(next_metadata_cache.title), "Cached title");
    snprintf(next_metadata_cache.artist, sizeof(next_metadata_cache.artist), "Cached artist");
    snprintf(next_metadata_cache.album, sizeof(next_metadata_cache.album), "Cached album");
    next_metadata_cache.has_title = true;
    next_metadata_cache.has_artist = true;
    next_metadata_cache.has_album = true;

    request_track_metadata(0, true, false, true);
    assert(submitted_request_valid && submitted_count == 1);
    assert(!submitted_request.read_file && !submitted_request.read_audio);
    assert(strcmp(lv_label_get_text(song_title_label), "Cached title") == 0);
    assert(strcmp(lv_label_get_text(artist_label), "Cached artist") == 0);
    assert(strcmp(lv_label_get_text(album_label), "Cached album") == 0);
    assert(!next_metadata_cached);
    assert(cover_decode_pending_valid);
    assert(strcmp(cover_decode_pending.local_track_path, test_queue_path(0)) == 0);
    assert(strcmp(cover_decode_pending.artist, "Cached artist") == 0);
    assert(strcmp(cover_decode_pending.album, "Cached album") == 0);
    lv_tick_inc(1000);
    poll_held_current_result();
    assert(cover_decode_pending_valid);
    assert(strcmp(cover_decode_pending.local_track_path, test_queue_path(0)) == 0);

    /* Applying the real metadata result path with the same request token
     * must preserve the already queued artwork request, not enqueue a second. */
    unsigned pending_generation = cover_decode_pending.generation;
    unsigned cover_generation = atomic_load_explicit(&cover_decode_generation, memory_order_relaxed);
    unsigned begin_count = cover_track_begin_count;
    release_and_poll_current_result(false);
    assert(cover_decode_pending_valid);
    assert(strcmp(cover_decode_pending.local_track_path, test_queue_path(0)) == 0);
    assert(cover_decode_pending.generation == pending_generation);
    assert(atomic_load_explicit(&cover_decode_generation, memory_order_relaxed) == cover_generation);
    assert(cover_track_begin_count == begin_count);
    assert(artwork_dispatched_metadata_token == submitted_request.token);
}

static void test_manual_selection_seeds_before_audio_and_promotes_fresh_tags(void) {
    reset_art_queue();
    test_queue_select(0);
    test_audio_generation = 100;
    audio_start_saw_seed = false;
    audio_start_saw_no_metadata_request = false;
    submitted_request_valid = false;
    audio_start_seen_title[0] = '\0';
    track_metadata_t visible = {0};
    snprintf(visible.title, sizeof(visible.title), "Visible row title");
    snprintf(visible.artist, sizeof(visible.artist), "Visible row artist");
    snprintf(visible.album, sizeof(visible.album), "Visible row album");
    visible.has_title = visible.has_artist = visible.has_album = true;
    gui_player_seed_selection_song(test_queue_path(0), &visible);

    play_track_at_from(0, 0.0);
    assert(audio_start_saw_seed);
    assert(audio_start_saw_no_metadata_request);
    assert(strcmp(audio_start_seen_title, "Visible row title") == 0);
    assert(strcmp(lv_label_get_text(song_title_label), "Visible row title") == 0);
    assert(submitted_request_valid && submitted_request.read_audio && !submitted_request.read_file);
    assert(submitted_request.audio_generation == test_audio_generation);
    assert(strcmp(submitted_request.metadata.title, "Visible row title") == 0);
    assert(artwork_dispatched_metadata_token == submitted_request.token);
    assert(cover_decode_pending_valid);
    assert(strcmp(cover_decode_pending.artist, "Visible row artist") == 0);

    /* A completion from the previous selection remains unable to replace
     * the second track after a rapid tap. */
    track_metadata_request_t stale = submitted_request;
    test_queue_select(1);
    snprintf(visible.title, sizeof(visible.title), "Second row title");
    gui_player_seed_selection_song(test_queue_path(1), &visible);
    play_track_at_from(1, 0.0);
    assert(strcmp(lv_label_get_text(song_title_label), "Second row title") == 0);
    current_result.request = stale;
    snprintf(current_result.request.metadata.title, sizeof(current_result.request.metadata.title), "Stale title");
    current_result.request.metadata.has_title = true;
    current_result_ready = true;
    gui_player_poll_track_metadata();
    assert(strcmp(lv_label_get_text(song_title_label), "Second row title") == 0);
}

static void test_cache_mismatch_does_not_leak_identity_or_art(void) {
    reset_art_queue();
    next_metadata_cached = true;
    snprintf(next_metadata_cached_path, sizeof(next_metadata_cached_path), "%s", test_queue_path(0));
    snprintf(next_metadata_cache.title, sizeof(next_metadata_cache.title), "Stale cached title");
    next_metadata_cache.has_title = true;
    test_queue_select(1);

    request_track_metadata(1, true, false, true);
    assert(submitted_request_valid && submitted_request.read_audio);
    assert(strcmp(lv_label_get_text(song_title_label), "Second") == 0);
    assert(!next_metadata_cached);
    assert(!cover_decode_pending_valid);
}

static void test_late_metadata_art_and_rapid_replacement(void) {
    reset_art_queue();
    test_queue_select(0);
    next_metadata_cached = false;
    request_track_metadata(0, true, false, true);
    assert(submitted_request_valid && submitted_request.read_audio);
    assert(!cover_decode_pending_valid); /* local art waits for metadata */
    player_seekbar_set_value(progress_slider, 70);

    /* Metadata is intentionally held by the submit stub. Elapsed UI time
     * must not expire or discard a later track's queued artwork. */
    lv_tick_inc(1000);
    poll_held_current_result();
    assert(!cover_decode_pending_valid);

    current_result.request = submitted_request;
    snprintf(current_result.request.metadata.artist, sizeof(current_result.request.metadata.artist), "Loaded artist");
    snprintf(current_result.request.metadata.album, sizeof(current_result.request.metadata.album), "Loaded album");
    current_result.request.metadata.has_artist = current_result.request.metadata.has_album = true;
    current_result_ready = true;
    gui_player_poll_track_metadata();
    assert(!current_result_ready);
    assert(cover_decode_pending_valid);
    assert(strcmp(cover_decode_pending.local_track_path, test_queue_path(0)) == 0);
    assert(strcmp(cover_decode_pending.artist, "Loaded artist") == 0);
    assert(lv_slider_get_value(progress_slider) == 70);

    /* Rapidly requesting another current track while reload holds decodes
     * replaces the old queued path and its scalar identity. */
    next_metadata_cached = true;
    snprintf(next_metadata_cached_path, sizeof(next_metadata_cached_path), "%s", test_queue_path(1));
    memset(&next_metadata_cache, 0, sizeof(next_metadata_cache));
    snprintf(next_metadata_cache.title, sizeof(next_metadata_cache.title), "Second cached");
    snprintf(next_metadata_cache.artist, sizeof(next_metadata_cache.artist), "Second artist");
    next_metadata_cache.has_title = next_metadata_cache.has_artist = true;
    test_queue_select(1);
    unsigned old_pending_generation = cover_decode_pending.generation;
    unsigned old_cover_generation = atomic_load_explicit(&cover_decode_generation, memory_order_relaxed);
    request_track_metadata(1, true, false, true);
    assert(cover_decode_pending_valid);
    assert(strcmp(cover_decode_pending.local_track_path, test_queue_path(1)) == 0);
    assert(strcmp(lv_label_get_text(song_title_label), "Second cached") == 0);
    unsigned new_cover_generation = atomic_load_explicit(&cover_decode_generation, memory_order_relaxed);
    assert(new_cover_generation != old_cover_generation);
    assert(cover_decode_pending.generation != 0);
    assert(cover_decode_pending.generation == new_cover_generation);
    assert(cover_decode_pending.generation != old_pending_generation);
}

static void test_uncached_file_read_defers_art(void) {
    reset_art_queue();
    test_queue_select(0);
    next_metadata_cached = false;
    request_track_metadata(0, true, false, false);
    assert(submitted_request_valid && submitted_request.read_file && !submitted_request.read_audio);
    assert(!cover_decode_pending_valid);

    current_result.request = submitted_request;
    snprintf(current_result.request.metadata.artist, sizeof(current_result.request.metadata.artist), "File artist");
    snprintf(current_result.request.metadata.album, sizeof(current_result.request.metadata.album), "File album");
    current_result.request.metadata.has_artist = current_result.request.metadata.has_album = true;
    current_result_ready = true;
    gui_player_poll_track_metadata();
    assert(!current_result_ready);
    assert(cover_decode_pending_valid);
    assert(strcmp(cover_decode_pending.local_track_path, test_queue_path(0)) == 0);
    assert(strcmp(cover_decode_pending.artist, "File artist") == 0);
}

/* A tapped indexed song needs neither audio-startup completion nor a DB
 * read to show its tags and request its artwork. Fresh tags still correct a
 * stale display hint, and enrichment must not reload lyrics/art again. */
static void test_manual_catalog_seed_and_enrichment(void) {
    reset_art_queue();
    test_queue_select(0);
    next_metadata_cached = false;
    memset(&display_cached_row, 0, sizeof(display_cached_row));
    snprintf(display_cached_row.path, sizeof(display_cached_row.path), "%s", test_queue_path(0));
    snprintf(display_cached_row.tags.title, sizeof(display_cached_row.tags.title), "Indexed title");
    snprintf(display_cached_row.tags.artist, sizeof(display_cached_row.tags.artist), "Indexed artist");
    snprintf(display_cached_row.tags.album, sizeof(display_cached_row.tags.album), "Indexed album");
    display_cache_hit = true;
    request_track_metadata(0, true, false, true);
    assert(submitted_request.read_audio && !submitted_request.read_file);
    assert(strcmp(lv_label_get_text(song_title_label), "Indexed title") == 0);
    assert(strcmp(lv_label_get_text(artist_label), "Indexed artist") == 0);
    assert(cover_decode_pending_valid);
    assert(strcmp(cover_decode_pending.album, "Indexed album") == 0);
    unsigned seeded_generation = cover_decode_pending.generation;
    lv_tick_inc(4000);
    assert(strcmp(lv_label_get_text(song_title_label), "Indexed title") == 0);
    assert(!favorite_track_known);

    current_result.request = submitted_request;
    snprintf(current_result.request.metadata.title, sizeof(current_result.request.metadata.title), "Fresh title");
    snprintf(current_result.request.metadata.album, sizeof(current_result.request.metadata.album), "Fresh album");
    current_result.enrichment_pending = true;
    track_metadata_result_t enriched = current_result;
    current_result_ready = true;
    gui_player_poll_track_metadata();
    assert(strcmp(lv_label_get_text(song_title_label), "Fresh title") == 0);
    assert(strcmp(cover_decode_pending.album, "Fresh album") == 0);
    assert(cover_decode_pending.generation != seeded_generation);
    assert(!favorite_track_known);
    unsigned after_tags_lyrics = lyrics_load_count;
    unsigned after_tags_generation = cover_decode_pending.generation;
    current_result = enriched;
    current_result.enrichment_pending = false;
    current_result.favorite = true;
    current_result_ready = true;
    gui_player_poll_track_metadata();
    assert(favorite_track_known);
    assert(lyrics_load_count == after_tags_lyrics);
    assert(cover_decode_pending.generation == after_tags_generation);
    display_cache_hit = false;
}

static void test_manual_worker_catalog_hint(void) {
    reset_art_queue();
    test_queue_select(0);
    next_metadata_cached = false;
    display_cache_hit = false;
    request_track_metadata(0, true, false, true);
    assert(submitted_request.read_audio);
    assert(!cover_decode_pending_valid);
    unsigned initial_lyrics = lyrics_load_count;
    current_result.request = submitted_request;
    snprintf(current_result.request.metadata.title, sizeof(current_result.request.metadata.title), "Worker catalog title");
    snprintf(current_result.request.metadata.artist, sizeof(current_result.request.metadata.artist), "Worker catalog artist");
    snprintf(current_result.request.metadata.album, sizeof(current_result.request.metadata.album), "Worker catalog album");
    current_result.request.metadata.has_title = current_result.request.metadata.has_artist = current_result.request.metadata.has_album = true;
    current_result.display_hint = true;
    current_result.enrichment_pending = true;
    track_metadata_result_t fresh = current_result;
    current_result_ready = true;
    gui_player_poll_track_metadata();
    assert(strcmp(lv_label_get_text(song_title_label), "Worker catalog title") == 0);
    assert(cover_decode_pending_valid);
    assert(lyrics_load_count == initial_lyrics);
    assert(metadata_scalar_applied_token != submitted_request.token);
    assert(!favorite_track_known);
    unsigned hinted_generation = cover_decode_pending.generation;

    fresh.display_hint = false;
    snprintf(fresh.request.metadata.title, sizeof(fresh.request.metadata.title), "Fresh worker title");
    current_result = fresh;
    current_result_ready = true;
    gui_player_poll_track_metadata();
    assert(strcmp(lv_label_get_text(song_title_label), "Fresh worker title") == 0);
    assert(metadata_scalar_applied_token == submitted_request.token);
    assert(lyrics_load_count == initial_lyrics + 1);
    assert(cover_decode_pending.generation == hinted_generation);

    /* A catalog hint queued against an older selection cannot retarget the
     * currently displayed track or its cover. */
    fresh.display_hint = true;
    current_result = fresh;
    current_result_ready = true;
    test_queue_select(1);
    request_track_metadata(1, true, false, true);
    unsigned replacement_generation = atomic_load(&cover_decode_generation);
    gui_player_poll_track_metadata();
    assert(!cover_decode_pending_valid);
    assert(atomic_load(&cover_decode_generation) == replacement_generation);
    assert(strcmp(lv_label_get_text(song_title_label), "Worker catalog title") != 0);
}

static void test_cover_partial_handoff_cache_and_cleanup(void) {
    reset_art_queue();
    test_queue_select(0);
    atomic_store_explicit(&cover_reload_active, false, memory_order_relaxed);
    unsigned int generation = atomic_fetch_add_explicit(&cover_decode_generation, 1u,
                                                         memory_order_relaxed) + 1u;
    cover_decode_active = true; /* Simulate effects still running after the handoff. */
    cover_decode_done_flag = false;
    player_layout_config = (player_layout_config_t) {
        .configured = true, .flat = true, .has_bg_color = true, .bg_color = 0x123456
    };
    cover_decode_request_t req = { .for_index = test_queue_current(), .generation = generation };
    cover_file_id_t file = { .valid = true, .dev = 1, .ino = 2, .mtime_sec = 3,
        .mtime_nsec = 4, .ctime_sec = 5, .ctime_nsec = 6, .size = 7 };
    uint16_t * worker_pixels = calloc((size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT, sizeof(uint16_t));
    assert(worker_pixels);
    worker_pixels[0] = 0x55aa;
    cover_decode_partial_publish(&req, worker_pixels, &file, true, 0xabcdef);
    assert(cover_decode_partial.valid);
    poll_cover_decode();
    assert(current_cover_bytes && current_cover_bytes != (uint8_t *) worker_pixels);
    assert(((uint16_t *) current_cover_bytes)[0] == 0x55aa);
    assert(worker_pixels[0] == 0x55aa); /* The worker still owns its immutable source. */
    assert(current_cover_for_index == test_queue_current());
    assert(current_reflection_bytes == NULL); /* Flat mode never installs an old effect. */
    assert(cover_decode_partial_applied_generation == generation);
    free(worker_pixels);

    uint8_t * shown = current_cover_bytes;
    cover_decode_partial = (cover_decode_partial_t) {
        .valid = true, .for_index = test_queue_current(), .generation = generation + 1,
        .pixels = calloc((size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT, sizeof(uint16_t)), .file = file
    };
    assert(cover_decode_partial.pixels);
    poll_cover_decode();
    assert(current_cover_bytes == shown); /* A stale generation is discarded. */
    cover_decode_partial = (cover_decode_partial_t) {
        .valid = true, .for_index = 1, .generation = generation,
        .pixels = calloc((size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT, sizeof(uint16_t)), .file = file
    };
    assert(cover_decode_partial.pixels);
    poll_cover_decode();
    assert(current_cover_bytes == shown); /* A stale queue index is discarded. */
    cover_decode_active = false;

    player_frost_params_t params = { .blur_radius = 5, .blur_passes = 2,
        .darken_num = 3, .darken_den = 4 };
    size_t bg_bytes = player_reflection_bytes();
    uint8_t * reflection = malloc(bg_bytes);
    assert(reflection);
    memset(reflection, 0x3c, bg_bytes);
    player_reflection_cache_clear();
    player_reflection_cache_put(&file, &params, reflection,
        player_reflection_cache_epoch(), 0, NULL);
    uint8_t * reuse = player_reflection_cache_get(&file, &params);
    assert(reuse && reuse[0] == 0x3c);
    free(reuse);
    player_frost_params_t other_params = params;
    other_params.blur_radius++;
    assert(!player_reflection_cache_get(&file, &other_params));
    cover_file_id_t changed_file = file;
    changed_file.mtime_nsec++;
    assert(!player_reflection_cache_get(&changed_file, &params));
    unsigned int obsolete_epoch = player_reflection_cache_epoch();
    player_reflection_cache_clear();
    player_reflection_cache_put(&file, &params, reflection, obsolete_epoch, 0, NULL);
    assert(!player_reflection_cache_get(&file, &params)); /* Late worker cannot repopulate after clear. */
    unsigned int obsolete_generation = atomic_load_explicit(&cover_decode_generation, memory_order_relaxed);
    atomic_fetch_add_explicit(&cover_decode_generation, 1u, memory_order_relaxed);
    player_reflection_cache_put(&file, &params, reflection,
        player_reflection_cache_epoch(), obsolete_generation, player_artwork_jobs_cache_write_stale);
    assert(!player_reflection_cache_get(&file, &params)); /* Canceled work cannot cache effects. */

    test_available_memory = 512u * 1024u;
    player_reflection_cache_put(&changed_file, &params, reflection,
        player_reflection_cache_epoch(), 0, NULL);
    assert(!player_reflection_cache_get(&file, &params));
    test_available_memory = SIZE_MAX;
    free(reflection);

    reflection = calloc(1, bg_bytes);
    assert(reflection);
    player_reflection_cache_put(&file, &params, reflection,
        player_reflection_cache_epoch(), 0, NULL);
    free(reflection);
    pthread_mutex_lock(&cover_decode_partial_mutex);
    cover_decode_partial = (cover_decode_partial_t) {
        .valid = true, .for_index = 0, .generation = generation,
        .pixels = calloc((size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT, sizeof(uint16_t)), .file = file
    };
    pthread_mutex_unlock(&cover_decode_partial_mutex);
    gui_player_begin_cover_reload();
    assert(!cover_decode_partial.valid && !cover_decode_partial.pixels);
    assert(!player_reflection_cache_get(&file, &params));
    atomic_store_explicit(&cover_reload_active, false, memory_order_relaxed);

    /* A completed no-art result must leave the already-published cover
     * detached and release its pixels instead of retaining stale artwork. */
    cover_decode_active = true;
    atomic_store_explicit(&cover_decode_done_flag, true, memory_order_release);
    cover_decode_result_generation = atomic_load_explicit(&cover_decode_generation, memory_order_relaxed);
    cover_decode_result_for_index = test_queue_current();
    cover_decode_result_ok = false;
    cover_decode_result_pixels = NULL;
    cover_decode_result_reflection = NULL;
    next_metadata_cached = false;
    assert(pthread_create(&cover_decode_thread, NULL, test_cover_noop_worker, NULL) == 0);
    poll_cover_decode();
    assert(!cover_decode_active && current_cover_bytes == NULL && current_cover_for_index == -1);

    reflection = calloc(1, bg_bytes);
    assert(reflection);
    player_reflection_cache_put(&file, &params, reflection,
        player_reflection_cache_epoch(), 0, NULL);
    free(reflection);
    player_reflection_cache_clear(); /* Same cleanup used by UI teardown. */
    assert(!player_reflection_cache_get(&file, &params));
}



int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
static bool artwork_worker_fail_create;
int __wrap_pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*start)(void *),void *arg) {
    if(artwork_worker_fail_create){artwork_worker_fail_create=false;return EAGAIN;}
    return __real_pthread_create(thread,attr,start,arg);
}
static cover_decode_request_t artwork_stream_request(int index,unsigned generation,const char *url) {
    cover_decode_request_t req={.for_index=index,.generation=generation,.flat=true};
    snprintf(req.stream_url,sizeof(req.stream_url),"%s",url);return req;
}
static void test_real_artwork_worker_lifecycle(void) {
    artwork_worker_enabled=true;
    atomic_store(&cover_decode_generation,10);atomic_store(&cover_reload_active,false);
    player_artwork_jobs_clear_pending();player_artwork_jobs_partial_clear();
    cover_decode_active=false;atomic_store(&cover_decode_done_flag,false);
    artwork_worker_http_calls=artwork_worker_decode_calls=artwork_worker_pixel_count=0;
    artwork_worker_a_started=artwork_worker_release_a=false;

    launch_cover_decode_req(artwork_stream_request(0,10,"https://unit/hold-a"));
    wait_artwork_worker_a();
    cover_decode_request_t b=artwork_stream_request(0,10,"https://unit/track-b");
    b.picture_data=malloc(16);assert(b.picture_data);artwork_worker_payload_b=b.picture_data;
    launch_cover_decode_req(b);
    launch_cover_decode_req(artwork_stream_request(0,10,"https://unit/track-c"));
    assert(player_artwork_jobs_pending());
    assert(strcmp(cover_decode_pending.stream_url,"https://unit/track-c")==0);
    assert(artwork_worker_payload_b_frees==1);

    current_cover_bytes=calloc(1,(size_t)COVER_ART_WIDTH*COVER_ART_HEIGHT*2);assert(current_cover_bytes);
    current_cover_file=(cover_file_id_t){.valid=true,.dev=30,.ino=40,.mtime_sec=50,.size=60};
    current_cover_dsc.data=current_cover_bytes;
    current_cover_dsc.data_size=(uint32_t)COVER_ART_WIDTH*COVER_ART_HEIGHT*2;
    release_artwork_worker_a();wait_artwork_worker_done();poll_cover_decode();
    /* Poll applies A's partial/final result before it transfers pending C.
       C therefore captures A's newly shown file identity (invalid for a stream). */
    assert(!current_cover_file.valid&&current_cover_bytes);
    assert(player_artwork_jobs_active());
    assert(!player_artwork_jobs_test_last_shown.valid);
    wait_artwork_worker_done();poll_cover_decode();
    assert(artwork_worker_http_calls==2&&artwork_worker_decode_calls==2);
    assert(artwork_worker_body_a_frees==1&&artwork_worker_body_c_frees==1);
    assert(current_cover_bytes!=(uint8_t*)artwork_worker_last_pixels);
    assert(cover_decode_partial_applied_generation==10);
    for(unsigned i=0;i<artwork_worker_pixel_count;i++)assert(artwork_worker_pixel_frees[i]==1);

    /* A canceled generation cannot replace the visible cover or publish a partial. */
    uint8_t *shown_before_cancel=current_cover_bytes;
    artwork_worker_a_started=artwork_worker_release_a=false;
    launch_cover_decode_req(artwork_stream_request(0,10,"https://unit/hold-a"));
    wait_artwork_worker_a();player_artwork_jobs_advance_generation();release_artwork_worker_a();
    wait_artwork_worker_done();poll_cover_decode();
    assert(current_cover_bytes==shown_before_cancel&&!cover_decode_partial.valid);
    assert(artwork_worker_http_calls==3&&artwork_worker_decode_calls==3);
    assert(artwork_worker_body_a_frees==2);
    for(unsigned i=0;i<artwork_worker_pixel_count;i++)assert(artwork_worker_pixel_frees[i]==1);

    /* A same-file mismatch in poll parks a generation-zero local retry; the
       poll tail sends it through the launch wrapper, which allocates its BT
       token/generation once before the real worker starts. */
    unsigned old_generation=player_artwork_jobs_generation();
    unsigned old_bt_begins=cover_track_begin_count;
    player_artwork_jobs_park_retry(artwork_stream_request(0,0,"https://unit/park-only"));
    assert(player_artwork_jobs_generation()==old_generation&&cover_track_begin_count==old_bt_begins);
    assert(cover_decode_pending_valid&&cover_decode_pending.generation==0);
    player_artwork_jobs_clear_pending();
    last_local_cover_request=artwork_stream_request(0,old_generation,"https://unit/retry");
    last_local_cover_request_valid=true;
    cover_decode_active=true;
    cover_decode_result_generation=old_generation;
    cover_decode_result_for_index=test_queue_current();
    memset(&cover_decode_result_format,0,sizeof(cover_decode_result_format));
    cover_decode_result_ok=true;
    cover_decode_result_same=true;
    cover_decode_result_file=(cover_file_id_t){.valid=true,.dev=99,.ino=101,.mtime_sec=102,.size=103};
    cover_decode_result_pixels=NULL;
    cover_decode_result_reflection=NULL;
    atomic_store_explicit(&cover_decode_done_flag,true,memory_order_release);
    assert(pthread_create(&cover_decode_thread,NULL,test_cover_noop_worker,NULL)==0);
    poll_cover_decode();
    unsigned retry_generation=player_artwork_jobs_generation();
    assert(retry_generation==old_generation+1&&cover_track_begin_count==old_bt_begins+1);
    assert(player_artwork_jobs_active());
    wait_artwork_worker_done();poll_cover_decode();

    /* Epoch and stale-policy checks preserve reload and cancellation rules. */
    cover_file_id_t file={.valid=true,.dev=1,.ino=2,.mtime_sec=3,.size=4};
    player_frost_params_t frost={.blur_radius=5,.blur_passes=2,.darken_num=1,.darken_den=2};
    size_t bytes=player_reflection_bytes();uint8_t *reflection=calloc(1,bytes);assert(reflection);
    unsigned epoch=player_reflection_cache_epoch();player_artwork_jobs_begin_reload();
    player_reflection_cache_put(&file,&frost,reflection,epoch,player_artwork_jobs_generation(),
                                player_artwork_jobs_cache_write_stale);
    assert(!player_reflection_cache_get(&file,&frost));player_artwork_jobs_end_reload();
    unsigned stale=player_artwork_jobs_generation();player_artwork_jobs_advance_generation();
    player_reflection_cache_put(&file,&frost,reflection,player_reflection_cache_epoch(),stale,
                                player_artwork_jobs_cache_write_stale);
    assert(!player_reflection_cache_get(&file,&frost));free(reflection);

    gui_player_begin_cover_reload();
    unsigned http_before_reload=artwork_worker_http_calls;
    launch_cover_decode_req(artwork_stream_request(0,0,"https://unit/reload-held"));
    assert(player_artwork_jobs_pending()&&!player_artwork_jobs_active());
    assert(gui_player_cover_decode_idle());
    assert(artwork_worker_http_calls==http_before_reload);
    gui_player_cancel_cover_reload();
    assert(!player_artwork_jobs_reload_active()&&!player_artwork_jobs_pending());

    artwork_worker_fail_create=true;
    cover_decode_request_t failed=artwork_stream_request(0,player_artwork_jobs_generation(),"https://unit/fail");
    failed.picture_data=malloc(13);assert(failed.picture_data);artwork_worker_payload_b=failed.picture_data;
    unsigned old_payload_frees=artwork_worker_payload_b_frees;
    launch_cover_decode_req(failed);
    assert(artwork_worker_payload_b_frees==old_payload_frees+1);
    assert(!player_artwork_jobs_active()&&player_artwork_jobs_running_idle()&&!player_artwork_jobs_done());
    free(current_cover_bytes);current_cover_bytes=NULL;current_cover_dsc.data=NULL;current_cover_dsc.data_size=0;
    cover_decode_request_t pending;
    if(player_artwork_jobs_take_pending(&pending))free(pending.picture_data);
    artwork_worker_enabled=false;
}

static void assert_gui_queue(const char *const *expected, int count, int current,
                             int pending_count) {
    assert(gui_player_get_playlist_count() == count);
    assert(gui_player_get_playlist_index() == current);
    assert(player_queue_model_pending_count() == pending_count);
    for (int i = 0; i < count; ++i)
        assert(strcmp(gui_player_get_track_path_at(i), expected[i]) == 0);
}

static void test_gui_queue_toasts_order_and_advance(void) {
    assert(i18n_set_language("es"));
    play_mode_t previous_mode = current_settings.play_mode;
    current_settings.play_mode = PLAY_MODE_SEQUENTIAL;

    const char *base[] = { "/queue/current.flac", "/queue/next.flac" };
    test_queue_install(base, 2, 0);
    const char *after_play_next[] = {
        "/queue/current.flac", "/queue/priority.flac", "/queue/next.flac"
    };
    info_toast_count = 0;
    gui_player_queue_play_next("/queue/priority.flac");
    assert_gui_queue(after_play_next, 3, 0, 1);
    assert(info_toast_count == 1);
    assert(strcmp(last_info_toast, "Se agregó 1 canción a la cola") == 0);

    uint64_t unchanged_revision = player_queue_model_revision();
    unsigned unchanged_toasts = info_toast_count;
    gui_player_queue_play_next("");
    assert_gui_queue(after_play_next, 3, 0, 1);
    assert(player_queue_model_revision() == unchanged_revision);
    assert(info_toast_count == unchanged_toasts);

    const char *bulk_one[] = { "/queue/bulk-one.flac" };
    const char *after_one[] = {
        "/queue/current.flac", "/queue/priority.flac", "/queue/bulk-one.flac",
        "/queue/next.flac"
    };
    assert(gui_player_queue_try_add_many(bulk_one, 1));
    assert_gui_queue(after_one, 4, 0, 2);
    assert(info_toast_count == unchanged_toasts + 1);
    assert(strcmp(last_info_toast, "Se agregó 1 canción a la cola") == 0);

    const char *bulk_many[] = { "/queue/bulk-two.flac", "/queue/bulk-three.flac" };
    const char *after_many[] = {
        "/queue/current.flac", "/queue/priority.flac", "/queue/bulk-one.flac",
        "/queue/bulk-two.flac", "/queue/bulk-three.flac", "/queue/next.flac"
    };
    assert(gui_player_queue_try_add_many(bulk_many, 2));
    assert_gui_queue(after_many, 6, 0, 4);
    assert(strcmp(last_info_toast, "Se agregaron 2 canciones a la cola") == 0);

    const char *pending[] = {
        "/queue/auto-current.flac", "/queue/auto-next.flac",
        "/queue/pending-tail.flac", "/queue/later.flac"
    };
    test_queue_install_state(pending, 4, 0, 2);
    const char *after_auto_and_play_next[] = {
        "/queue/auto-current.flac", "/queue/auto-next.flac",
        "/queue/after-advance.flac", "/queue/pending-tail.flac", "/queue/later.flac"
    };
    test_audio_advanced_pending = true;
    gui_player_queue_play_next("/queue/after-advance.flac");
    assert(!test_audio_advanced_pending);
    assert_gui_queue(after_auto_and_play_next, 5, 1, 2);
    assert(strcmp(gui_player_get_current_track_path(), "/queue/auto-next.flac") == 0);
    assert(strcmp(last_info_toast, "Se agregó 1 canción a la cola") == 0);

    const char *last_track[] = { "/queue/delete-me.flac" };
    test_queue_install(last_track, 1, 0);
    delete_song_popup.popup = lv_obj_create(player_screen);
    assert(delete_song_popup.popup);
    lv_obj_add_event_cb(delete_song_popup.popup, delete_song_confirm_cb,
                        LV_EVENT_CLICKED, NULL);
    unsigned stops_before = audio_stop_count;
    unsigned notifications_before = stopped_notification_count;
    empty_delete_notification_observed = false;
    assert(lv_obj_send_event(delete_song_popup.popup, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(audio_stop_count == stops_before + 1);
    assert(stopped_notification_count == notifications_before + 1);
    assert(empty_delete_notification_observed);
    assert(gui_player_get_playlist_count() == 0);
    assert(gui_player_get_playlist_index() == -1);
    lv_obj_delete(delete_song_popup.popup);
    delete_song_popup.popup = NULL;

    current_settings.play_mode = previous_mode;
    const char *restored[] = { "/music/Artist/First.flac", "/music/Artist/Second.flac" };
    test_queue_install(restored, 2, 0);
    (void)i18n_set_language("en");
}

int main(void) {
    setup_widgets();
    test_next_prefetch_is_independent_of_gapless();
    test_next_cover_prefetch_waits_for_foreground_and_cancels();
    test_manual_selection_seeds_before_audio_and_promotes_fresh_tags();
    test_generated_cover_same_file_identity_skips_decode();
    test_cached_next_identity_and_art();
    test_cache_mismatch_does_not_leak_identity_or_art();
    test_late_metadata_art_and_rapid_replacement();
    test_uncached_file_read_defers_art();
    test_manual_catalog_seed_and_enrichment();
    test_manual_worker_catalog_hint();
    test_cover_partial_handoff_cache_and_cleanup();
    test_real_artwork_worker_lifecycle();
    test_gui_queue_toasts_order_and_advance();
    player_queue_model_clear_begin();
    player_queue_model_clear_finish();
    puts("Player track transition runtime: cached identity, early artwork, replacement and real worker lifecycle passed");
    return 0;
}
