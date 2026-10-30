#ifndef GUI_H
#define GUI_H

#include "../../library/metadata/scan/library_walk.h"
#include "lvgl/lvgl.h"
#include "../../core/system/runtime/board_config.h"
#include "../../core/persistence/settings.h"
#include "../../core/system/runtime/play_mode.h"
#include "../../library/browser/paths/library_paths.h"
#include "../settings/input/gui_button_bindings.h"
#include "../appearance/gui_theme.h"
#include "../plugins/manage/gui_plugin_order_editor.h"
#include "../shell/gui_notifications.h"
#include "../library/browse/gui_library.h"
#include "../library/library_search.h"
#include "../player/screen/gui_queue.h"
#include "../player/screen/gui_player.h"
#include "../settings/lock/gui_lock_screen.h"

#include "../plugins/gui_plugins.h"
#include "../shell/gui_shell.h"
#include "../shell/gui_navigation.h"
#include "../../library/metadata/db/metadata_db.h"
#include "../plugins/gui_plugin_library.h"


#include "../../library/artwork/albumart/albumart.h"
#include "../../library/artwork/albumart/albumart_info.h"
#include "../../library/remote/remote_track.h" /* remote_track_meta_t, used by gui_plugin_play_remote_tracks() below */
#include "../home/home_layout.h" /* home_layout_config_t, used by gui_plugin_set_home_layout() below */
#include "../home/launcher_layout.h"
#include "../player/layout/player_layout.h"
#include <stdint.h>

#ifdef UI_PERF_TRACE
uint64_t ui_perf_now_us(void);
#endif
#include <stddef.h>

/* Initialize the user interface elements and callbacks */
void gui_init(uint32_t screen_width, uint32_t screen_height);
/* For gui_reload.c's in-process UI reload -- build_stream_media_screen()
 * itself is static to gui.c, so these wrap it. */
void gui_stream_media_teardown(void);
void gui_stream_media_rebuild(void);
void gui_stream_media_refresh(void);
void gui_deinit(void);
/* True while the screen is lit or its selected wake surface is ready to paint. */
bool gui_display_refresh_allowed(void);
/* An expired sleep timer overrides the lyrics keep-awake option. Other
 * screens retain their normal inactivity policy. Called on the UI thread. */
void gui_sleep_timer_expired(void);


/* Optional player splash after display setup, before gui_init(). Currently
 * disabled (zero duration); the loader owns the boot artwork. */
#ifndef HOST_BUILD
void gui_show_boot_splash(void);

/* Re-anchor screen inactivity after main.c switches LVGL from the boot-time
 * clock callback to its low-overhead incremented runtime clock. */
void gui_reset_interactive_timeout_baseline(void);
#endif

/* Tells the main loop (main.c) to drop the real time elapsed while the UI
 * thread was blocked on purpose, so LVGL's clock does not jump. */
void main_loop_discard_elapsed_time(void);
/* Clear touch frames queued while a synchronous external app owned input. */
bool main_loop_discard_pointer_input(void);

/* UI-thread cleanup after an emulator process returns; see gui.c. */
void gui_resume_after_external_session(void);
/* UI-thread release of rebuildable caches before an emulator runs; see gui.c. */
void gui_release_memory_for_external_session(void);

/* ---- Bridge for src/plugins/manager/plugin_manager.c ----
 * plugin_manager.c owns Lua state/script lifecycle and has no LVGL code of
 * its own; these let a plugin's C API calls (plugin.show_list/play_file/
 * play_list/show_toast, see plugin_manager.c's register_plugin_api()) reach
 * gui.c's existing screens/playback plumbing instead of duplicating it. */

/* Repopulates and pushes a shared plugin-list screen (one of a small
 * reusable pool -- see gui.c's own comment on why a pool rather than a
 * single shared screen) showing `labels[0..count)` as plain tappable rows.
 * Tapping row i calls plugin_manager_list_item_selected(slot, i). icon_paths[i]/
 * text_sizes[i] (either may be NULL wholesale, or contain per-row NULLs) are
 * optional per-row icon (raw absolute filesystem path, see
 * screen_builders.h's pill_row_apply_icon())/text-size ("small"/"medium"/
 * "large", already validated by plugin_manager.c) -- a row with neither set
 * anywhere in this call keeps today's exact plain-label rendering. height
 * (0 = shared font-aware 96px default, scaled for the board) applies
 * to every row in this call, not per-row. Icon rows inherit native category
 * gradients and use 44px reference icons. wrap_labels opts individual rows
 * into multiline text and grows them beyond the minimum height to fit. Returns
 * the selected screen-pool slot so callbacks can be stored per slot. */
int gui_plugin_show_list(const char * title, const char * const * labels, const char * const * icon_paths,
                          const char * const * text_sizes, const bool * wrap_labels, int32_t height, int32_t width,
                          int selected_index, int count, int columns, int replace_slot);
/* True while pool slot's list screen is the top of the navigation stack. */
bool gui_plugin_list_is_top(int slot);
/* Resets the scroll position of an already-rendered plugin list while it is
 * still the navigation top. Returns false for an invalid or covered slot. */
bool gui_plugin_list_reset_scroll(int slot);

/* Repopulates and pushes a shared plugin-settings-list screen -- a SEPARATE
 * pool from gui_plugin_show_list()'s own (PLUGIN_SETTINGS_LIST_SCREEN_POOL_
 * SIZE slots, plugin_manager.h), for a plugin's own nested settings submenu
 * built from real toggle/slider rows, not plain tappable text. Parallel
 * arrays, row_types[i] one of PLUGIN_SETTINGS_ROW_TAP/_TOGGLE/_SLIDER
 * (plugin_manager.h) -- toggle_initial[]/slider_min[]/slider_max[]/
 * slider_value[] are read only for the matching row type, ignored (may be
 * garbage) otherwise. icon_paths[i]/heights[i]/widths[i]/text_sizes[i] are the same
 * per-row options as gui_plugin_show_list() above, except height IS per-row
 * here (ignored for a "slider" row -- see screen_builders.h's PILL_ROW_
 * HEIGHT_MIN comment for why a resized row needs a different background,
 * which a slider's own fixed-layout card doesn't support). Returns the pool
 * slot index this call landed in, so plugin_manager.c can key its own
 * per-slot Lua callback-ref storage off the same slot gui.c actually used.
 * When update_existing is true, an already-live screen with this title is
 * repopulated in place and is not pushed again; absent a match, a new screen
 * is opened normally. */
struct plugin_control_dashboard_options;
int gui_plugin_show_settings_list(const char * title, const int * row_types, const char * const * labels,
                                   const bool * toggle_initial, const int * slider_min, const int * slider_max,
                                   const int * slider_value, const char * const * icon_paths, const int32_t * heights,
                                   const int32_t * widths, const char * const * text_sizes,
                                   const bool * wrap_labels, int count, bool update_existing,
                                   const gui_lock_screen_options_t * preview,
                                   const struct plugin_control_dashboard_options * dashboard);

/* Starts playback of a brand-new playlist built from `paths[0..count)`,
 * starting at paths[start_index] -- same "starting something new clears
 * Up Next" semantics as every other play-launch path (on_file_selected()).
 * Copies paths/strings itself; caller retains ownership of its own array.
 * open_player false keeps the current screen instead of opening Now Playing. */
void gui_plugin_play_paths(const char * const * paths, int count, int start_index, bool open_player);

/* Same as gui_plugin_play_paths() above, for a remote-provider queue (see
 * remote_track.h) -- each track's synthetic "remote://" key becomes the
 * playlist[] entry, and the full descriptor (real stream URL, codec,
 * ReplayGain, artwork) is published for gui.c/audio.c to look up by that
 * key. Copies tracks itself; caller retains ownership of its own array. */
void gui_plugin_play_remote_tracks(const remote_track_meta_t * tracks, int count, int start_index);

/* Shows the same transient toast used elsewhere in the app (e.g. "Added to
 * queue"). */
void gui_plugin_show_toast(const char * msg, uint32_t duration_ms);

/* Sets one of the three shared background-color styles (screen_builders.h's
 * style_theme_screen_bg/style_theme_card_bg, or list_row_style itself for
 * "list_row") live, app-wide -- slot must be "screen", "card", or
 * "list_row" (plugin_manager.c's l_plugin_set_background_color() already
 * validates this before calling here, so this trusts its caller). rgb is
 * a packed 0xRRGGBB value. */
void gui_plugin_set_background_color(const char * slot, uint32_t rgb);

/* Same shape, for text color -- slot must be "primary" or "muted"
 * (plugin_manager.c's l_plugin_set_text_color() already validates this).
 * See screen_builders.h's style_theme_text_primary/style_theme_text_muted
 * comment for what each covers and what's deliberately excluded. */
void gui_plugin_set_text_color(const char * slot, uint32_t rgb);

/* Stores plugin.set_home_layout()'s validated config for build_home_screen()
 * (gui_settings.c) to read at startup or a targeted theme refresh.
 * plugin_manager.c's
 * l_plugin_set_home_layout() already validated every enum-like field
 * (key/mode/align/text_size) before calling here, so this trusts its caller,
 * same convention gui_plugin_set_background_color() above already uses. */
void gui_plugin_set_home_layout(const home_layout_config_t * config);

/* Restores home_layout_config to its unconfigured (configured == false)
 * zero state -- called by plugin_manager_deinit()'s full-reload path
 * (gui_reload.c) before plugin_manager_init() re-runs plugin scripts.
 * Ensures that if a plugin modifying home_layout is removed or disabled,
 * the layout reverts to native rather than retaining stale configuration.
 * plugin.refresh_theme() does not call this, rebuilding Home from whatever
 * is currently configured. */
void gui_plugin_reset_home_layout(void);
void gui_plugin_set_launcher_layout(const launcher_layout_config_t * config);
void gui_plugin_reset_launcher_layout(void);
void gui_plugin_set_player_layout(const player_layout_config_t * config);
void gui_plugin_reset_player_layout(void);

/* ---- Playback control bridges for plugin.toggle_pause()/stop()/next_track()/
 * prev_track()/seek()/set_volume()/is_playing()/is_paused()/get_position()/
 * get_duration() ----
 *
 * Unlike peq.c's plugin.eq_*() bindings (which call peq_set_*()/peq_save()
 * directly from plugin_manager.c, no LVGL/gui.c state involved), these can't
 * just call audio_toggle_pause()/audio_stop()/audio_set_volume() directly --
 * gui.c's own native call sites always pair each of those with extra local
 * state (the play/pause icon, the volume slider/popup, shuffle-aware
 * next/prev stepping, DAC-mode blocking) that would otherwise go stale. Each
 * bridge below just calls the exact same local gui.c helper the native UI
 * itself already uses for that action. */

/* Calls the same local toggle_play_pause() the play/pause button's own
 * click handler uses -- includes its external_dac_block_reason() guard,
 * set_play_button_state() icon update, and last_position checkpoint. */
void gui_plugin_toggle_pause(void);

/* audio_stop() + set_play_button_state(false), guarded by audio_is_playing()
 * -- same pairing every native audio_stop() call site already uses (e.g.
 * gui.c's Bluetooth-DAC-mode-enable handler). */
void gui_plugin_stop(void);

/* Shuffle-aware next/prev -- compute_manual_step_index() + play_track_at(),
 * the same pair gui.c's own remote-control (BT/phone) consume path uses. */
void gui_plugin_next_track(void);
void gui_plugin_prev_track(void);

/* Seeks the current track to an absolute position in seconds. */
void gui_plugin_seek(double seconds);

/* Clamps to [0,100], then mirrors gui.c's own remote-control volume path:
 * audio_set_volume() + the volume_slider widget + current_settings.volume/
 * settings_save() + show_volume_popup()/refresh_volume_topbar(), so a
 * plugin-driven volume change looks identical to a hardware/remote one. */
void gui_plugin_set_volume(int percent);
/* Same volume update without opening the transient volume popup. */
void gui_plugin_set_volume_silent(int percent);
/* Updates live audio/UI only; leaves remembered normal/car-mode volume and
 * persisted settings untouched, even when another setting is saved later. */
void gui_plugin_set_volume_transient(int percent, bool show_popup);

/* Pure state reads -- trivial wraps of audio_is_playing()/audio_is_paused()/
 * audio_get_position_seconds()/audio_get_duration_seconds(), routed through
 * gui.c for the same "plugin_manager.c has no audio-state code of its own"
 * boundary the rest of this block keeps, even though none of these need any
 * gui.c-local state. */
bool gui_plugin_is_playing(void);
bool gui_plugin_is_paused(void);
double gui_plugin_get_position_seconds(void);
double gui_plugin_get_duration_seconds(void);

/* "sequential"/"repeat_all"/"repeat_one"/"shuffle" -- string form of
 * current_settings.play_mode (see gui.c's own play_mode_t), matching the
 * order/loop/single/random icon the player screen cycles through. */
const char * gui_plugin_get_play_mode(void);

/* Absolute path of the track at playlist_index, or NULL if nothing is
 * loaded (no active playlist yet, or playback stopped and cleared it). */
const char * gui_plugin_get_current_track_path(void);

/* plugin.open_file_manager([folder]): the native More > File Manager. */
bool gui_plugin_open_file_manager(const char * folder);
/* plugin.pick_file([folder], callback): native indexed SD card browser. */
bool gui_plugin_pick_file(const char * folder);
void gui_plugin_cancel_file_picker(void);
/* After a plugin's file operation, so every file browser shows the change. */
void gui_plugin_refresh_file_browsers(void);

/* ---- Bridges for plugin.get_now_playing()/set_interval()/clear_interval()/
 * show_text_input() -- see plugin_manager.h's own comments on the Lua-facing
 * shape of each. ---- */

/* Fills title/artist/album (NUL-terminated, truncated to fit) and
 * *out_duration_seconds from gui.c's own cached now-playing state (the same
 * metadata already passed to a "track_started" event handler, populated at
 * the same two call sites that fire it) -- returns false (buffers/duration
 * untouched) if nothing is currently loaded. */
bool gui_plugin_get_now_playing(char * title_out, size_t title_size, char * artist_out, size_t artist_size,
                                 char * album_out, size_t album_size, double * out_duration_seconds);

/* Creates/deletes an lv_timer for plugin pool slot `slot` (0-based, into a
 * PLUGIN_MAX_INTERVALS-sized array) -- the shared timer callback reads its
 * own slot back out and calls plugin_manager_interval_fired(slot). Same
 * repeating-timer mechanism this file's own update_timer_cb() already uses
 * for its 500ms polling loop. */
void gui_plugin_set_interval(int slot, uint32_t period_ms);
void gui_plugin_clear_interval(int slot);

/* Thin wrapper around this file's own show_text_entry() singleton screen
 * (numeric=false always -- a plugin wanting a numeric-only keypad can still
 * just validate/convert the returned string itself). See
 * plugin_manager.h's own doc comment on plugin.show_text_input() for the
 * singleton/cancel-semantics caveats this inherits as-is. */
void gui_plugin_show_text_input(const char * title, const char * initial_text, bool is_password);


/* gui_font_role_t declared in gui_theme.h */


lv_obj_t * add_section_header(lv_obj_t * parent, const char * text);
void start_bt_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled);
void set_play_button_state(bool is_playing);
void arm_next_track_for_audio(int index);

const char * basename_of(const char * path);

typedef enum {
    PLAYER_SOURCE_NONE,
    PLAYER_SOURCE_ALL_SONGS,
    PLAYER_SOURCE_GROUP_SONGS,
    PLAYER_SOURCE_FILE_BROWSER,
    PLAYER_SOURCE_RECENTLY_ADDED,
} player_source_kind_t;

extern player_source_kind_t player_source_kind;
extern int player_source_all_songs_index;
extern int player_source_recently_added_index;
extern group_song_entry_t * player_source_group_entries;
extern int player_source_group_count;
extern int player_source_group_pos;
extern char * player_source_group_title;
extern char player_source_file_browser_dir[];
extern int player_source_file_browser_row;

void hide_more_menu_popup(void);

extern char now_playing_path[600];

void set_player_source_all_songs(int index);
void set_player_source_recently_added(int index);
void set_player_source_file_browser(const char * dir, int row);




lv_indev_t * find_pointer_indev(void);
void configure_scrolling_row_label(lv_obj_t * label, int32_t width);
void open_song_context_menu(const char * path);


const char * playlist_path_at(int index);
void on_file_selected_lazy_all_songs(int selected_index);
void on_file_selected_lazy_recently_added(int selected_index);
/* Takes ownership of snapshot/title_ranks on success. `title_ranks` maps
 * collection order into the pinned full-catalog snapshot. */
bool gui_player_play_lazy_collection(metadata_db_snapshot_t *snapshot,
                                     int *title_ranks, int count, int play_mode);
/* Takes ownership of `paths` on success; array/strings use the standard
 * metadata_db_load_* caller-owned convention. */
bool gui_player_play_collection_paths(char **paths, int count, int play_mode);

void nav_reset_to_home(void);
void open_queue_screen(void);
void library_scan_once(void);
void library_load_from_cache_only(void);



void queue_add_song(const char * path);
void play_track_at(int target);
void get_display_names(const char * path, char * out_title, size_t title_sz, char * out_folder, size_t folder_sz);
void row_label_enable_marquee(lv_obj_t * label);

void refresh_volume_topbar(int32_t percent);

void player_transition_mark_dirty(void);
void set_play_button_state(bool is_playing);


const char * play_mode_icon_asset(play_mode_t mode);


void quick_drawer_mark_snapshot_dirty(void);


void toggle_play_pause(void);
int compute_manual_step_index(int index, int direction);
void on_file_selected(char ** new_playlist, int count, int selected_index);
void on_file_selected_at(char ** new_playlist, int count, int selected_index, double start_seconds);
void clear_player_source(void);



#define NAV_STACK_MAX 16
void boot_checkpoint(const char * msg);
/* See its own doc comment (main.c) -- call once, at the very start of any
 * thread whose own crash needs to be loggable even if the crash IS the
 * thread's own stack being exhausted. */
void install_thread_crash_altstack(void);

/* ---- Shared GUI and Screen Builders Helpers ---- */
void generic_back_cb(lv_event_t * e);
lv_obj_t * add_pill_chevron_row(lv_obj_t * list, const char * text, lv_event_cb_t cb);
lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text);
void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon);

/* ---- Busy Indicator & Notifications ---- */
/* Notifications and Accent declared in gui_notifications.h and gui_theme.h */
void crossfade_switch_event_cb(lv_event_t * e);
void gapless_switch_event_cb(lv_event_t * e);
void lyrics_switch_event_cb(lv_event_t * e);
void car_mode_switch_event_cb(lv_event_t * e);
void inline_remote_switch_event_cb(lv_event_t *e);
void swipe_up_home_switch_event_cb(lv_event_t * e);
void hide_player_topbar_switch_event_cb(lv_event_t * e);
void charge_limiter_switch_event_cb(lv_event_t * e);
void safe_charging_switch_event_cb(lv_event_t * e);
void led_indicator_switch_event_cb(lv_event_t * e);
void battery_percent_switch_event_cb(lv_event_t * e);
void clock_24h_switch_event_cb(lv_event_t * e);
void resume_mode_settings_row_cb(lv_event_t * e);
void replaygain_mode_settings_row_cb(lv_event_t * e);
void font_size_settings_row_cb(lv_event_t * e);
void usb_mode_settings_row_cb(lv_event_t * e);
void update_music_database_row_cb(lv_event_t * e);
void refresh_all_metadata_row_cb(lv_event_t * e);
void firmware_update_row_cb(lv_event_t * e);
void bt_dac_settings_row_cb(lv_event_t * e);
void register_swipe_dead_zone(lv_obj_t * obj);
void refresh_clock_label(void);


#define BT_MAX_RESULTS 32
extern bool bt_is_powered_cached;
extern char bt_connected_mac_cached[18];
extern char bt_connected_codec_cached[32];
/* Negotiated sampling frequency of the same A2DP link, 0 when unknown. */
extern unsigned int bt_connected_rate_cached;
void quick_drawer_wifi_event_cb(lv_event_t * e);
void quick_drawer_bt_event_cb(lv_event_t * e);

void populate_wifi_screen(bool enabled);

#endif /* GUI_H */

/* Owner-thread service after the opt-in input/worker poll wakes. */
void gui_process_input_wake(void);
/* A worker published a result (ui_wake_notify): the main loop calls begin
 * right before lv_timer_handler() (make the "is the job done" poll timers due)
 * and end right after it (apply what depends on their outcome). */
void gui_jobs_dispatch_begin(void);
void gui_jobs_dispatch_end(void);
/* True once, after a dispatch_end skipped for a slide and the slide is over. */
bool gui_jobs_take_deferred(void);
