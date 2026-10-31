#pragma once

#include <stdbool.h>

/* In-process "soft" UI reload: tears down every screen, style, and
 * plugin-contributed UI element and rebuilds them fresh in the same
 * process, without restarting it -- so a plugin.set_icon()/set_background_
 * color()/set_text_color() call can take full effect without requiring the
 * player to be killed and relaunched. Deliberately never touches
 * audio_init()'s playback state or any network/Bluetooth/D-Bus connection
 * (Wi-Fi, Subsonic, DLNA, AirPlay, Remote Control) -- only the LVGL screen
 * tree, style objects, the image cache, and the plugin Lua state get reset.
 * See gui_reload.c's own top comment for the exact teardown/rebuild
 * sequence and why each step is ordered the way it is. */

/* Synchronous -- runs the full teardown/rebuild sequence inline. Must ONLY
 * be called from the main loop (gui.c's while(1), via lv_timer_handler()),
 * never from inside a plugin_call(): it lua_close()s every plugin's
 * lua_State, and closing the state a currently-executing Lua call lives on
 * is undefined behavior. plugin.reload_ui() does NOT call this directly --
 * see gui_reload_request() below. */
void gui_soft_reload(void);

/* Safe to call from anywhere, including from inside a plugin's own Lua
 * callback -- this is what plugin.reload_ui() calls. Schedules
 * gui_soft_reload() to run from a one-shot LVGL timer on the next main-loop
 * pass, strictly after the triggering call has fully returned and control
 * is back in gui.c's lv_timer_handler() loop. */
void gui_reload_request(void);
/* True while a soft reload is tearing down and rebuilding the UI. */
bool gui_reload_in_progress(void);
/* True while a soft reload is queued or running. */
bool gui_reload_pending(void);

/* Explicit Plugin Manager refresh: rebuilds plugin UI, then restores its
 * supported native navigation path and scroll position from semantic IDs. */
bool gui_reload_request_preserving_plugin_manager_page(void);

/* Replace only Player widgets after the current callback returns. Preserve
 * navigation, other screens, plugin state and playback; keep a visible
 * Player's old frame until its replacement is ready. Latest layout wins.
 * A coalesced full asset/plugin reload still takes precedence. */
void gui_player_layout_reload_request(void);

/* Applies already-written theme assets/config after the caller returns.
 * Rebuilds Home and render caches only; screens, plugins, navigation,
 * playback, and connection-owning services remain alive. */
void gui_theme_refresh_request(void);
