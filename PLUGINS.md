# Writing plugins

Plugins add features to the player with plain Lua: new screens, settings,
themes, streaming sources, automations. No compiler, rebuild or firmware
flash is needed.

Ready-made plugins live in the
[compas-plugins repository](https://github.com/Starnished66/compas-plugins).
The in-app **Plugin Store** (Settings > System > Plugin Manager > Plugin
Store) installs from it. Browse it for real examples, or contribute your own.
More examples are in `plugins_examples/` in this repository.

This guide describes the API in `src/plugins/plugin_manager.c`. If the two
ever disagree, the code is right.

**Contents**

- [Quick start](#quick-start)
- [How plugins run](#how-plugins-run)
- [Where your plugin appears](#where-your-plugin-appears)
- [Identity and compatibility](#identity-and-compatibility)
- [Screens](#screens)
- [Theming](#theming)
- [Playback](#playback)
- [Music library](#music-library)
- [Files and playlists](#files-and-playlists)
- [Sound](#sound)
- [Device](#device)
- [Network](#network)
- [Data and storage](#data-and-storage)
- [Events and timers](#events-and-timers)
- [Examples](#examples)
- [Testing and debugging](#testing-and-debugging)
- [Extending the API (C developers)](#extending-the-api-c-developers)

## Quick start

1. Create a `.lua` file.
2. Give the plugin an identity with `plugin.define()`.
3. Add a way to open it, for example a row in Settings.
4. Copy the file to `<SD card>/.plugins/`.
5. Open **Settings > System > Plugin Manager** and tap **Refresh Plugins**,
   or restart the player.

```lua
plugin.define({
    id = "org.example.hello",
    name = "Hello Player",
    version = "1.0.0",
    api_min = 1,
})

plugin.register_list_item("settings", "Hello Player", function()
    plugin.show_list("Hello Player", { "The plugin is working!" }, function(index)
        plugin.show_toast("Selected row " .. index)
    end)
end)
```

Save it as `<SD card>/.plugins/HelloPlayer.lua`. The row appears under
Settings > System > Additional Tools.

Tip: start from the example closest to what you want to build.
`Audiobooks.lua`, `NetRadio.lua`, `LastFmScrobbler.lua`, or the historical
`Themes.lua` reference cover common shapes.

## How plugins run

**Loading**

- Every `*.lua` file directly inside `<SD card>/.plugins/` is a plugin
  (`/data/mnt/sd_0/.plugins/` on the device, `./music/.plugins/` on the
  simulator). Subfolders are not scanned, but plugins can keep their own files
  there.
- Plugins load at startup, and again on **Refresh Plugins** or
  `plugin.reload_ui()`. Each one runs its file from top to bottom once; this is
  where it registers rows, tiles and events.
- Files load in alphabetical order (byte-wise, case-sensitive). Up to 32
  enabled plugins load; Plugin Manager marks the rest "not loaded: plugin limit
  reached".
- Some settings are global and the last plugin to set them wins, for example
  colors, icons and the Home layout. Because the order is alphabetical, the
  winner is always the same.
- Each plugin has its own Lua state, kept alive for as long as it is loaded,
  so callbacks can run at any time later.
- A plugin that fails to load is skipped and logged; the others are not
  affected. A callback that raises an error is logged and the player keeps
  running.

**Sandbox**

Plugins come from the SD card, so they don't get the full Lua standard
library:

- Removed: `load`, `loadstring`, `loadfile`, `dofile`, `require`, `package`,
  `debug`, `os.execute`, `os.getenv`, `os.exit`, `os.tmpname` and `io.popen`.
  A plugin must be a single file; it cannot load other Lua files or run shell
  commands.
- Kept: the rest of `io` (including `io.open`), and `os.time`, `os.date`,
  `os.clock`, `os.difftime`, `os.remove` and `os.rename`, so plugins can keep
  their own state files.
- File access is otherwise unrestricted, with one exception: nothing can touch
  `/usr/data/.compas/plugins/`, where `plugin.storage` and `plugin.secrets` keep their
  data. Every `plugin.*` function that takes a path refuses paths inside it
  with an error; so do Lua's `io` and `os` functions (`io.open`, `os.remove`
  and `os.rename` return `nil, error`, the others raise).

**Time limit**

Each run of plugin code (the top-level code, a callback, an event, a timer
tick) may use at most 2 seconds of Lua time. Time spent inside `plugin.*`
functions (copying icons, a slow HTTP request) doesn't count, but a runaway
loop is stopped. File reads through Lua's own file handles do count.

## Where your plugin appears

| Entry point | Function | Limit |
| --- | --- | --- |
| A row in a native list | `register_list_item` | 16 in Playback & Controls or Sound; 8 in other lists, all plugins together |
| A tile in Stream Media | `register_stream_media_tile` | 5 tiles |
| A tile on Home | `register_home_tile` | 6 tiles, shown only when a Home layout lists them |
| An on/off tile in the quick drawer | `register_quick_toggle` | 4 tiles |

The lists that accept rows:

| `list_id` | Where the row appears | Good for |
| --- | --- | --- |
| `"books"` | Books | Readers, audiobooks, reference tools |
| `"settings"` | Settings > System > Additional Tools | General plugin settings |
| `"display"` | Settings > Display | Themes and visual tools |
| `"playback"` | Settings > Playback & Controls | Resume and playback tools |
| `"music_audio"` | Settings > Sound | EQ, DSP, gain and volume tools |
| `"music_controls"` | Settings > Playback & Controls > Buttons & Remote | Button and remote behavior |
| `"music_timers"` | Settings > Power | Sleep and idle timers |
| `"music_library"` | Settings > Library | Scanning, tagging, scrobbling |
| `"power"` | Settings > Power | Battery and power tools |
| `"system"` | Settings > System | Device and maintenance tools |

Rows from different plugins share a list, up to 16 in `playback` or
`music_audio` and 8 in other lists, and the list
scrolls. Any other `list_id` is an error.

## Identity and compatibility

### `plugin.define(info)`

Declare who the plugin is, once, at the top of the file:

```lua
plugin.define({
    id = "org.example.my_plugin",  -- letters, digits, ".", "_", "-"; must be unique
    name = "My Plugin",
    version = "1.0.0",
    api_min = 2,                   -- refuse to load on an older player
})
```

- The `id` names the plugin's private storage, so keep it stable across
  versions. Two plugins with the same id are an error.
- `api_min` stops the plugin from loading on a player whose API is older.
- Plugins without `define()` still work. They get an id from their file
  name, made unique with a short hash of the path if needed.

### Checking what the player supports

- `plugin.api_version()` returns the API version, currently `16`.
- `plugin.has_capability(name)` returns whether one feature exists. Prefer it
  over `api_min` when you only need one feature. Tokens:

  | Area | Tokens |
  | --- | --- |
  | UI | `ui.list`, `ui.settings`, `ui.row_width`, `ui.text_input`, `ui.toast`, `ui.progress`, `ui.screenshot`, `ui.theme`, `ui.theme_refresh`, `ui.reload`, `ui.home_layout`, `ui.home_tiles`, `ui.launcher_layout`, `ui.home_background`, `ui.lock_screen`, `ui.quick_toggle`, `ui.text_view`, `ui.text_view_images`, `ui.list_grid`, `ui.list_showing`, `ui.list_update`, `ui.list_wrap`, `ui.settings_list_wrap`, `ui.player_layout_xml`, `ui.file_manager` |
  | Playback and audio | `playback.control`, `playback.state`, `playback.events`, `playback.progress`, `playback.remote`, `playback.transport_skip`, `playback.speed`, `playback.format`, `playback.output_info`, `playback.silent_volume`, `playback.transient_volume`, `playback.output_events`, `playback.settings`, `playback.ab_loop`, `playback.ab_switch`, `playback.http_seek`, `audio.peq`, `audio.peq.transient`, `audio.peq.state`, `audio.stereo_width`, `audio.hw_volume_curve` |
  | Files | `filesystem.sd`, `filesystem.mkdir`, `filesystem.playlists`, `filesystem.file_ops` |
  | Storage | `storage.namespaced`, `storage.secrets`, `storage.secrets_get` |
  | Network | `network.http.sync`, `network.http.async`, `network.http.download`, `network.http.download_progress` |
  | Data | `data.json`, `crypto.md5`, `data.zip`, `data.zip_image`, `data.image_thumbnail`, `data.html` |
  | Library | `library.artist_albums`, `library.paged`, `library.refresh`, `library.track_metadata` |

- LEDs depend on the model: check `plugin.led_available()` instead.
- `plugin.get_app_info()` returns `{ version, build, platform, plugin_api }`.

### API versions

| Version | Added |
| --- | --- |
| 2 | `storage`, `secrets`, `json_decode`/`json_encode`, `media_capabilities`, `download_file_async`, `mkdir`, and the full `http_request` (headers, all methods, timeouts, redirects, response headers, real cancel). The sandbox also removed `require`, `dofile` and friends in this release. |
| 3 | `play_remote`, `queue_remote_list`, and `provider`/`track_id` on `track_started` |
| 4 | `set_home_layout` |
| 5 | `reload_ui` |
| 6 | `refresh_theme` |
| 7 | `register_home_tile` and `order` in `set_home_layout` |
| 8 | `set_launcher_layout` |
| 10 | `background_image` in `set_home_layout` |
| 11 | `set_hw_volume_curve` |
| 12 | `register_quick_toggle`, `set_quick_toggle` |
| 13 | LED control, `get_volume`, `get_battery`, and the `volume_changed`, `battery_changed`, `suspending` and `system_resumed` events |
| 14 | `zip_read`, `zip_list`, `zip_image_async`, `html_to_blocks`, `show_text_view` with pictures, grid lists, `is_list_showing`, and one long (4095-byte) HTTP header per request |
| 15 | Full XML Player-layout support (`ui.player_layout_xml`), including plugin-bundle discovery, current named-widget features, companion PNG previews, and resolution-specific XML variants using `@WIDTHxHEIGHT` or `_WIDTHxHEIGHT` filenames |
| 16 | Playback speed/stereo width/progress plus format/output snapshots, silent volume, transient EQ, output events, playback settings, native A–B loops/comparison, and finite HTTP seeking. Modeless progress bars and download byte snapshots are also available, as is the File Manager (`open_file_manager`, `file_copy`, `file_move`, `file_delete`). API 16 remains the unreleased v1.1 API. |

All of these are additions; older plugins keep working.

## Screens

### `plugin.register_list_item(list_id, label, on_open [, options])`

Adds a row to a native list (see [Where your plugin
appears](#where-your-plugin-appears)). `on_open()` runs when the row is
tapped; open your first screen from there.

`options` may contain the [row options](#row-options) `icon`, `height`,
`width` and `text_size`, plus `group` (up to 32 bytes) to place the row in a
submenu:

- In `music_audio`: `"effects"` (under Sound Effects) or `"profiles"` (under
  Equalizer > Profiles > Download profiles).
- In `display`: `"appearance"` or `"player_layout"` (under those submenus).
- Other values leave the row directly in its list. Older players ignore
  `group`.

A plugin may register several rows. In API 16, Playback and Music Audio accept
16 rows each; other lists accept 8. The limit includes all plugins together.
Exceeding it is an error.

### `plugin.register_stream_media_tile(label, on_open [, icon])`

Adds a tile to Stream Media, after Subsonic. Good for streaming and radio
plugins. `icon` is a theme image path, default `"stream_media/radio.png"`. Up
to 5 plugin tiles; the grid doesn't scroll.

### `plugin.register_home_tile(id, label, on_open, icon)`

Registers a tile that a Home layout can show (see
[`set_home_layout`](#pluginset_home_layouttiles-options)). Registering alone
doesn't show it: it appears once a layout's `order` lists its `id`.

- `id`: 1 to 39 letters, digits, `.`, `_` or `-`, unique among all Home tiles,
  and not a native key (`music`, `stream_media`, `wireless`, `books`,
  `settings`, `dac`, `subsonic`).
- `icon`: required, a theme image path of at most 79 characters.
- Up to 6 plugin tiles.

### `plugin.register_quick_toggle(id, label, on_change, options)`

Adds an on/off tile to the quick drawer's expanded area (pull the drawer
down, then drag the handle below the first row). Use it for a simple switch;
use a list row for anything that needs a screen.

- `id`: 1 to 39 letters, digits, `.`, `_` or `-`, unique.
- `label`: short caption, about 8 characters fit.
- `on_change(new_value)`: called with the new boolean when tapped.
- `options.icon` (required, under 80 characters): the off-state image.
- `options.icon_selected`: the on-state image. Default: `icon` with
  `_s.png` in place of its extension. It is recolored to the accent color.
- `options.value`: initial state, default `false`.
- `options.on_text`, `options.off_text`: state captions, default `"On"` and
  `"Off"`.
- `options.on_hold()`: called when the tile is held, for example to open the
  plugin's settings.

Up to 4 tiles from all plugins; they fill the drawer's third row in
registration order.

```lua
if plugin.has_capability("ui.quick_toggle") then
    plugin.register_quick_toggle("mseb", "MSEB", function(on) set_enabled(on) end,
        { icon = "pull_down/mseb.png", value = enabled })
end
```

### `plugin.set_quick_toggle(id, value)`

Updates the tile's state without calling `on_change`. Use it when the plugin
changes the setting elsewhere, such as its own settings screen.

### `plugin.show_list(title, items, on_select [, options])`

Opens a list screen and returns a handle (`nil` on players older than API
14).

- `items`: one entry per row. Either a string, or a table
  `{ label = "...", icon = "...", text_size = "...", wrap = true }`.
  - Labels are up to 159 bytes, or 511 with `wrap = true`, which wraps the
    label onto several lines and grows the row instead of scrolling it
    (check `ui.list_wrap`).
  - Up to 500 rows; the rest are dropped.
- `on_select(index)`: called with the 1-based index of the tapped row. Not
  called if the user goes back.
- `options`:
  - `height`, `width`: size of every row (see [row options](#row-options)).
  - `replace = handle`: API 16, `ui.list_update`. Replaces this plugin's
    currently visible list in place without pushing navigation. Returns a new
    handle; the old handle expires. Returns `nil` without changing the screen
    for a closed, covered, foreign, or superseded handle. Keep the returned
    handle for later pages and guard asynchronous completions with it.
  - `selected`: 1-based row drawn with an accent outline. The outline follows
    later taps.
  - `layout = "grid"`: shows cards instead of rows. Each card shows its `icon`
    in a 2:3 portrait area (fitted, not cropped, like a book cover) with the
    label below in up to two lines; without an icon, the label fills the
    card. `height` and `width` don't apply. Check `ui.list_grid`.
  - `columns`: cards per row in a grid, 2 to 4, default 3.

Calling `show_list` again (for example from `on_select`) opens a new screen
on top, and Back returns to the previous one with its own callback. Up to 4
lists can be stacked; going deeper breaks Back navigation.

`plugin.is_list_showing(handle)` returns `true` while that list is the screen
in front. Check it before opening a screen from a late callback (an HTTP
response, a finished image job), so you don't open it over whatever the user
moved on to. Check `ui.list_showing`.

Rows with icons use the native submenu look (44 px icons, 96 px rows, a blue
gradient that a custom `list_row` color replaces). Text-only lists stay
compact.

### `plugin.show_settings_list(title, items [, options])`

Opens a screen that looks like a native Settings submenu, with real switches
and sliders. Each item is a table with `type` and `label`:

```lua
plugin.show_settings_list("My Plugin", {
    { type = "row",    label = "About",  on_select = function() ... end },
    { type = "toggle", label = "Enable", value = true, on_change = function(on) ... end },
    { type = "slider", label = "Boost",  min = 0, max = 12, value = 3,
      on_change = function(v) ... end },
})
```

- `row`: `on_select()` runs on tap. Open another `show_settings_list` from it
  for a nested submenu.
- `toggle`: `on_change(new_value)` runs on every tap.
- `slider`: integer range `min` to `max`. `on_change(new_value)` runs once,
  when the finger is lifted.
- Every type accepts the [row options](#row-options) `icon`, `width` and
  `text_size`; `height` works on rows and toggles but not sliders. Rows and
  toggles also accept `wrap = true` for labels up to 511 bytes (95 without
  it). Check `ui.settings_list_wrap`.
- Up to 24 items and 4 sliders per screen; extras are dropped. A missing
  `type`, an unknown `text_size`, or a missing callback is an error.
- Up to 2 settings screens can be stacked.
- Optional third argument `options` supports `update = true`, which refreshes
  the deepest live settings screen with the same title in place, without
  adding a navigation entry. If no live screen has that title, it opens a new
  screen normally. Use this when a child chooser changes a value shown by its
  covered parent.
- `options.preview` accepts the same `mode`, `image_path`, `image_fit`, and
  `clock_24h` fields as `plugin.show_lock_screen()`. It embeds a live lock
  screen preview above the settings rows.

### Row options

These optional fields work in `register_list_item` options, `show_list`
items and `show_settings_list` items. In `show_list`, `height` and `width` go
in the call's `options` so every row matches.

| Field | Meaning |
| --- | --- |
| `icon` | An image shown left of the label, scaled to a fixed size. An absolute path, or a path relative to `<SD card>/.plugins/`. A missing image is skipped, not an error. |
| `height` | Row height in px, clamped to 100 to 220. Default: 124 for list rows added with `register_list_item`, 84 for plain `show_list` rows. |
| `width` | Row width in px, clamped to 240 to 464, kept centered. |
| `text_size` | `"small"`, `"medium"`, `"large"` (with fallback fonts for Cyrillic, CJK, Korean and Thai) or `"mono"` (an ASCII-only pixel font). Any other value is an error. |

### `plugin.show_text_input(title, initial_text, is_password, on_submit)`

Opens the app's keypad text entry (the one used for Wi-Fi passwords).
`on_submit(text)` runs when the user presses Enter; it doesn't run if they go
back. Returns `true`, or `false, "text input busy"` if another request is
open. You can open the next input from inside `on_submit` (for example a
username, then a password).

### `plugin.show_text_view(title, text [, options])`

Opens a paged reader for long text. Returns `true`, or `false, "busy"` if one
is already open, or `false, "unavailable"` during a screen transition.

- `text`: up to 256 KB; longer text is cut.
- `options.page` (1-based) or `options.offset` (byte offset): where to start.
  `page` wins.
- `options.images`: up to 64 picture files (usually made by
  `zip_image_async`). The text shows picture `n` (0-based) where it contains
  `"\27" .. n .. "\27"`. Each picture gets its own page. Check
  `ui.text_view_images`.
- `options.on_turn(page, pages, byte_offset)`: runs shortly after the view
  opens (never inside the `show_text_view` call), after each page turn, and
  when the final page count is known. Use it to save the reading position.
  While the view is still finding the requested `page` or `offset`, it isn't
  called, so a saved position is never overwritten by a temporary one.
- `options.on_close(page, byte_offset)`: runs after the view closes, however
  it was closed. If it closes before reaching the requested `offset`, it
  reports that offset. Not called on a plugin reload.
- `options.font_px` is accepted but ignored; the text uses the Font Size
  setting.

Tap the right third or swipe left for the next page; tap the left third or
swipe right for the previous page. Swiping right on the first page, or the
Back button, closes the view. The footer shows `page / pages`. Pagination runs
in the background, so very long texts open immediately; past 8192 pages the
rest isn't shown.

### API 16 progress UI

Check `ui.progress` before using these functions. They run on the UI thread
and leave navigation and playback available while work continues.

| Function | Result |
| --- | --- |
| `plugin.show_progress(title, message [, fraction])` | A new opaque handle, or `nil, error` if the UI is unavailable. |
| `plugin.update_progress(handle, message [, fraction])` | `true` if that plugin still owns the visible card; otherwise `false`. |
| `plugin.close_progress(handle)` | Closes the owned card; returns whether it was visible. |

Omit `fraction` (or pass `nil`) for an animated indeterminate bar during
searches, connection setup, or transfers with unknown size. A finite fraction
from `0` to `1` displays a percentage. Title and message are bounded to 128
and 512 bytes respectively; NUL bytes are rejected. Do not invent percentages
for work without a measured total.

There is one modeless card. A new `show_progress` replaces it and invalidates
its previous handle, even if another plugin owned it. The user can dismiss
the card without cancelling the task. Updates never reopen a dismissed card;
only a fresh user action should call `show_progress` again. Handles belong to
the calling plugin, including its coroutines, and are invalidated on plugin
unload, failed loading, or UI reload. Close the card on every completion and
failure path; use a toast for the result. Keep automatic background work quiet.

```lua
local progress = plugin.show_progress("Cover Art Fetcher", "Searching for cover…")
-- Later, on the UI thread:
if progress then plugin.update_progress(progress, "Downloading cover…", 0.4) end
if progress then plugin.close_progress(progress) end
plugin.show_toast("Cover saved")
```

The native regressions use real Lua and LVGL:

```sh
python3 scripts/test_plugin_progress_api.py
make host
python3 scripts/test_plugin_progress_ui.py
```

For a reproducible device transfer, `scripts/tests/progress_http_fixture.c`
is a loopback-only fixture serving `/known` (Content-Length) and `/unknown`
(chunked), each with 262144 payload bytes over roughly 32 seconds. Compile
for the device, stage and run it only from persistent SD storage, then stop
its listener and child handlers and remove all fixture files after testing.

### `plugin.show_toast(message [, duration_ms])`

Shows a short message at the bottom of the screen. Duration 100 to 30000 ms,
default 5000.

### `plugin.screenshot()`

Starts a screenshot. Returns `true`, or `false, reason` with one of
`screen_off`, `no_card`, `usb_storage`, `busy`, `framebuffer_unavailable`,
`worker_start_failed` or `unavailable`. The screen flashes and a toast
confirms it. Files go to `<SD card>/Screenshots/`. Listen to the
`screenshot_saved` and `screenshot_failed` events for the result.

### `plugin.show_lock_screen(options)`

Shows a full-screen lock screen over everything, dismissed by swiping up.
Returns `true`, or `false, message`.

- `mode` (required): `"album_art"` (the current cover, or the default
  cover when there is none), `"image"` or `"clock"`.
- `image_path`: the image file, required for `"image"`.
- `image_fit`: `"contain"` keeps the whole image over a blurred fill; `"cover"`
  fills the screen with centered cropping. Applies to photos and album art.
  Omitted fit preserves native-size photos and fills the screen for album art.
- `clock_24h`: optional override; otherwise follows the device clock setting.

The `screen_woke` event runs while the panel is still dark, allowing this call
to prepare the lock screen before the first visible frame. Artwork decoding
and blur generation run in the background.

## Theming

Themes are natively built into Compás and managed via the built-in system
theme picker (**More → Themes**), loading `.theme` key-value files directly from
`SD/Themes/` without requiring a Lua theme plugin. Active themes resolve icons
via an in-memory native icon overlay layer without performing NAND bulk copies.
When other plugins call `plugin.set_icon`, their explicitly copied overrides
continue to be supported and take priority over theme layer lookups.

Theme packs distributed via the Plugin Store keep companion `.theme` files
alongside a legacy `Themes.lua` stub for backward compatibility with older
player versions and store catalog classification. On current versions of the
player, `Themes.lua` is recognized as a legacy theme loader and suppressed at
load time, as native theme management handles `.theme` files directly.

The script `plugins_examples/Themes.lua` remains available as a historical
reference format for custom theme application and programmatic UI styling.

| To apply... | Call |
| --- | --- |
| colors | nothing, they apply immediately |
| icons, Home and launcher layouts | `plugin.refresh_theme()` |
| anything, the heavy way | `plugin.reload_ui()` |

### `plugin.set_icon(theme_path, source_file)`

Replaces a built-in icon. `theme_path` is the icon's path in the theme (for
example `"launcher/book.png"`); `source_file` is the replacement image, which
is copied into the theme override folder. Raises an error if it can't be
copied.

- Call it from top-level code. The app caches icons, and screens built before
  the override existed keep using the old file until `refresh_theme()` or
  `reload_ui()`.
- Keep the replacement close to the original's size and shape: the layout was
  measured for the original.

### `plugin.set_background_color(slot, rgb)` / `plugin.set_text_color(slot, rgb)`

Change colors app-wide, immediately, from anywhere. `rgb` is `0xRRGGBB`.

- Background slots: `"screen"` (every screen), `"card"` (popups and cards),
  `"list_row"` (every list row).
- Text slots: `"primary"` (titles and labels) and `"muted"` (secondary text).
  Red warning text and accent-colored text are not affected.

Another slot name is an error.

### `plugin.set_home_layout(tiles, options)`

Restyles Home, chooses which tiles it shows, or turns it into a scrolling
list. Takes effect when Home is rebuilt (`refresh_theme()`, `reload_ui()` or a
restart). Each call replaces the whole previous layout.

`tiles` restyles individual tiles, one table per tile:

```lua
{
    key = "music",          -- native key or a register_home_tile id (required)
    bg_color = 0x1e3524, text_color = 0xd8c9a3,
    radius = 24,            -- px, not negative
    -- list mode only:
    height = 92, width = 440,         -- Home reference sizing (see below)
    align = "center",                 -- "left" (default), "center" or "right"
    accessory = true,                 -- show a chevron
    text_size = "medium",             -- like row options
    icon = true,                      -- show the tile's icon in the row
}
```

Native keys: `music`, `stream_media`, `wireless`, `books`, `settings`, `dac`,
and `subsonic` (opt-in, opens Subsonic directly).

Home measurements use the R1's 480×800 screen as a reference. Widths,
radii and gaps scale to the active board. In list mode, heights express
the relative row proportions: Home expands or shrinks them to fill the
space between the status bar and gesture area, respecting font and touch
minimums. If those minimums cannot fit, the list scrolls. This Home fitting
does not change dimensions passed to submenu layouts or other plugin lists.

`options`:

- `mode`: `"tile"` (the icon grid, default) or `"list"`.
- `tile_gap` (tile mode, 0 to 64 px) and `row_gap` (list mode, 0 to 84 px,
  0 means the default 6).
- `order`: which tiles to show, in order. Leave a tile out to hide it; add a
  plugin tile's id to show it. Without `order`, Home shows the six original
  tiles. Tile mode shows at most 6, since the grid doesn't scroll; use list
  mode for more.
- `background_image`: a `.png`, `.jpg` or `.jpeg` behind Home only, drawn at
  its own size and centered. Make it the screen's size (480x800 on the R1).
  It is copied when you call the function.

Errors: a repeated `order` entry, more than 12 tile entries, an unknown
`align` or `text_size`, a negative `radius`, an invalid background image, or
`options` that isn't a table. A tile key that doesn't exist yet is not an
error (its plugin may load later); if it still doesn't exist when Home is
built, it is skipped. A repeated key replaces the earlier entry.

```lua
plugin.set_home_layout({
    { key = "music", bg_color = 0x1e3524, text_color = 0xd8c9a3, radius = 24 },
}, { mode = "list", row_gap = 10, order = { "music", "dac", "books" } })
```

### `plugin.set_launcher_layout(options)`

Switches the Music, Stream Media and Wireless menus between their icon grid
and a list, independently. Each of `music`, `stream_media` and `wireless` takes
`mode = "tile"` or `"list"` plus the list styling fields of
`set_home_layout` (`row_gap`, `height`, `width`, `bg_color`, `text_color`,
`radius`, `align`, `accessory`, `text_size`, `icon`). Menus you leave out stay
a grid; an empty call resets all three. Apply with `refresh_theme()`.

```lua
plugin.set_launcher_layout({
    music = { mode = "list", height = 108, width = 480, row_gap = 10,
              bg_color = 0x000000, text_color = 0x33FF33, text_size = "mono" },
})
```

### `plugin.set_player_layout(options)`

Changes the look of the Player (Now Playing) screen. Every field is optional.

**Background**

| Field | Type | Meaning |
| --- | --- | --- |
| `flat` | boolean | `true` replaces the blurred cover background with a solid color. |
| `bg_color` | `0xRRGGBB` | The solid color for `flat`. Without it, the theme background is used. |
| `blur_radius` | integer | Blur strength, 0 to 64. Default 5. |
| `blur_passes` | integer | Blur passes, 0 to 16. Default 3. |
| `darken_num`, `darken_den` | integers | Darkens the blurred cover by `darken_num / darken_den`. Set both. Default 1/2. |

Each call replaces all of these, so pass every option you want in the same
call. They are global: they apply to every layout, including Default, and not
only to one you ship. A layout that wants its own background should paint it
(an opaque root) instead of relying on `flat`.

**Whole layout**

| Field | Type | Meaning |
| --- | --- | --- |
| `xml` | string | An XML layout file, relative to `<SD card>/.plugins/`. |
| `name` | string | Name shown in Settings. Default: the file name without `.xml`. |
| `id` | string | Internal id, 1 to 63 letters, digits, `_`, `-` or `.`; not `default`. Default: `plugin.<file name without .xml>`, other characters replaced by `_`. Longer ids are cut short. |

How to write the XML file, and which widget names the app needs, is explained
in [docs/PLAYER_LAYOUTS.md](docs/PLAYER_LAYOUTS.md).

- The file must be inside the `.plugins` folder. Absolute paths, `..`, links
  leading outside the folder, or a missing file raise a Lua error.
- If the file can't be used (invalid XML or a required widget missing), the
  default layout is shown instead.
- Called from top-level code, it only adds the layout to **Settings > Display
  > Player Layout > Layout**. Users pick it there like any other layout, and
  the choice is remembered while the plugin stays installed and enabled. If
  the plugin is disabled or removed, the default layout is shown instead.
- Called from a callback (a button, for example), the layout is selected right
  away for this session, and the interface reloads to apply it if that changes
  the active layout. It stays until the player restarts or the user picks
  another layout in Settings.
- Check `ui.player_layout_xml` first if the plugin should also run on older
  firmware.

```lua
-- Solid dark background instead of the blurred cover
plugin.set_player_layout({ flat = true, bg_color = 0x101010 })

-- Offer a layout shipped with the plugin, with the same background
if plugin.has_capability("ui.player_layout_xml") then
    plugin.set_player_layout({
        xml = "player_layouts/clean.xml", name = "Clean",
        flat = true, bg_color = 0x101010,
    })
end
```

### `plugin.refresh_theme()`

Applies icon changes and the Home and launcher layouts, without disturbing
anything else: other screens, navigation, playback and connections stay as
they are. It runs after your callback returns. Use it after a batch of theme
calls.

### `plugin.reload_ui()`

Rebuilds every screen and restarts all plugins, without restarting the app
or interrupting playback and connections. It runs after your callback
returns, and the user lands on Home. Call it only in response to a user
action, never unconditionally from top-level code (that would reload on
every load; a second request during a reload is ignored).

```lua
plugin.register_list_item("display", "Theme", function()
    plugin.show_list("Theme", { "Dark", "White" }, function(index)
        apply_theme(index == 2 and "white" or "dark")  -- your own theme calls
        plugin.refresh_theme()
    end)
end)
```

## Playback

### Playing files

- `plugin.play_file(path)`: plays one file (absolute path) or stream URL as a
  new queue, replacing Up Next like tapping a song does.
- `plugin.play_list(paths [, start_index])`: plays a list of paths or URLs as
  a new queue, starting at the 1-based `start_index` (default 1, out-of-range
  values are clamped). Up to 500 entries.

**Stream URLs** (`http://` or `https://`) support live streams and finite files:

- Format is chosen in this order. A `#.ext` fragment is first (`#.flac`,
  `#.aac`, `#.aacp`, `#.m4a`, `#.m4b`, `#.mp4`; the fragment is never sent
  to the server). Then the URL path's extension, ignoring the query string
  and fragment, for those same extensions. Then the response `Content-Type`:
  `audio/flac` or `audio/x-flac` for FLAC, `audio/aac` or `audio/aacp` for
  ADTS AAC/AAC+, and `audio/mp4`, `video/mp4` or `application/mp4` for AAC
  in MP4. Anything else stays MP3, including a `.mp3` URL and playlist URLs
  such as `.m3u`, so an unrecognized or ambiguous type does not change
  today's default. Other formats fail to open.
- Live streams remain forward-only. Finite FLAC, MP3 and AAC-in-MP4 sources
  can seek when the server supports validated byte ranges; check
  `get_playback_format().seekable`. Raw ADTS AAC remains forward-only.
  Duration is shown when the decoder or remote-track metadata provides it.
- No reconnect: if the connection drops, playback stops.
- An endless stream (radio) never auto-advances. Finite tracks can.
- Stream titles (ID3 or ICY metadata) are not shown.

### `plugin.play_remote(track)` / `plugin.queue_remote_list(tracks [, start_index])`

For streaming-service plugins (Qobuz, Tidal and the like): plays one track,
or a new queue of up to 500, with full metadata.

```lua
plugin.play_remote({
    provider = "qobuz",           -- required
    track_id = "12345",           -- required, stable for this track
    stream_url = "https://...",   -- required
    verify_tls = true,            -- default true
    title = "Song", artist = "Artist", album = "Album",
    duration_ms = 214000,
    artwork_url = "https://...",
    codec = "flac",               -- "mp3" (default), "flac" or "aac"
    sample_rate = 44100, bit_depth = 16, channels = 2, bitrate_kbps = 0,  -- display only
    replaygain_db = -3.5,
})
```

- Favorites and play counts use `remote://<provider>/<track_id>`, not the URL,
  so they add up even when the URL changes on every play. They don't appear
  in the Favorites and Most Played screens yet.
- Remote tracks are not resumed after a restart.
- Playback follows the stream rules above: finite sources can seek when
  `get_playback_format().seekable` is true; live sources remain forward-only.
  There is no reconnect.

### Controls

| Function | Does |
| --- | --- |
| `plugin.toggle_pause()` | Play/pause, like the button (blocked in Bluetooth DAC and AirPlay modes, like the button). |
| `plugin.stop()` | Stops playback. |
| `plugin.next_track()`, `plugin.prev_track()` | Next and previous, respecting shuffle. |
| `plugin.seek(seconds)` | Requests a position in the current track. Finite HTTP sources can seek when `get_playback_format().seekable` is true; live streams remain forward-only. |
| `plugin.set_volume(percent, opts)` | Sets volume clamped to 0–100. Shows the popup by default; `{ silent = true }` suppresses it (`playback.silent_volume`). `{ persist = false }` updates live audio/UI without changing remembered volume or saving settings (`playback.transient_volume`); persistence defaults to true. |
| `plugin.set_playback_speed(dir, speed)` | API 16, `playback.speed`: requests pitch-preserving speed from 0.5 to 2.0 for local tracks beneath an absolute directory. Returns acceptance, not completion of the asynchronous transition. `nil, 1.0` resets the scope. Scoped to path components; last call wins, cleared on plugin shutdown. |
| `plugin.get_playback_speed()` | Returns the actual committed speed; 1.0 while idle or for unsupported formats. |
| `plugin.set_transport_skip(dir, seconds)` | Makes Next and Previous skip `seconds` (1 to 300) within files under `dir` instead of changing track; 0 turns it off. Applies to every Next/Previous control, not to automatic track changes. Not saved; the last call wins. Check `playback.transport_skip`. |

Playback speed uses speech time stretching for local mono/stereo PCM sources from
8–96 kHz, with 16-bit output while active. DSD and network streams keep normal
speed. Crossfade is disabled across tracks with active stretching. Seek, chapter,
bookmark and progress times stay in source seconds; during stretching the
delivered source position is nominal, with fractional frame accounting and an
exact source endpoint once the final output is delivered. Speed preferences
should be saved by the plugin, and scoped to its content directory.


### Current state

| Function | Returns |
| --- | --- |
| `plugin.is_playing()`, `plugin.is_paused()` | booleans |
| `plugin.get_position()`, `plugin.get_duration()` | seconds |
| `plugin.get_playback_progress(expected_path)` | A coherent `{ position, duration, playing, paused, terminal }` snapshot, or `nil` unless the absolute path exactly matches the current playback generation or its retained terminal state. Check `playback.progress` before using it. `terminal` is `"active"`, `"natural_eof"`, or `"manual_stop"`. `active` can include decoder recovery after a premature EOF; only `natural_eof` confirms natural completion. Use this for checkpoints that must distinguish completion from a manual stop; the legacy getters above are independent snapshots. |
| `plugin.get_volume()` | 0 to 100 |
| `plugin.get_now_playing()` | `title, artist, album, duration_seconds`, or `nil` if nothing has played yet |
| `plugin.get_current_track_path()` | absolute path, or `nil` when nothing is loaded |
| `plugin.get_play_mode()` | `"sequential"`, `"repeat_all"`, `"repeat_one"` or `"shuffle"` |

### API 16 audio additions

These additions keep API version **16**. Check each capability with
`plugin.has_capability(token)` when supporting older builds.

| Functions | Capability and behavior |
| --- | --- |
| `get_playback_format()` | `playback.format`: `nil` while idle, otherwise a snapshot with `path`, `codec`, source `sample_rate`/`bit_depth`, `output_sample_rate`/`output_bit_depth`, `channels`, `bitrate_kbps`, `duration_seconds`, `is_stream`, `seekable`, `is_dsd`, `dop`, `replaygain_applied`/`replaygain_applied_db`, `generation`, and live `software_volume_gain`, `playback_speed`, `crossfade_enabled`. Unknown source depth/bitrate is 0; MP3 has no encoded PCM bit depth. Software gain is a linear PCM multiplier, distinct from hardware/UI volume. Crossfade reports the setting, not whether two tracks are currently mixing. |
| `get_output_info()` | `playback.output_info`: `route` (`wired`, `bluetooth`, or `usb_dac`), `active`, PCM `sample_rate`/`bit_depth`, `hardware_sample_rate`/`hardware_bit_depth` when known, cached `bluetooth_codec` when available, `resampling`, `resampling_known`, and `dop`. Bluetooth and USB conversion/final hardware formats are unknown; matching PCM fields alone cannot prove bit-perfect output. |
| `eq_apply_profile(path, { persist = false })` | `audio.peq.transient`: applies an SD profile to runtime EQ without saving it. Default persistence remains enabled. Later explicit EQ save/edit operations may save the active profile. |
| `get_eq_state()` | `audio.peq.state`: detached runtime snapshot with `bypass`, `preamp_db`, `stereo_width`, and ten ordered `bands` (`index`, `freq_hz`, `gain_db`, `q`, `type`, `enabled`). This observes active settings, including transient profiles, rather than reading a possibly stale saved profile. |
| `eq_apply_state(state, { persist = false })` | `audio.peq.state`: validates the complete snapshot before applying it. Persistence defaults to true. Plugins can restore a saved runtime snapshot without writing flash; compare the current state with the last state they applied before restoring, to preserve subsequent user changes. Numerical limits: preamp/gain −300 to 300 dB, frequency 1–24000 Hz, Q 0.01–100, width 0–2; all must be finite. |
| `get_track_metadata(path)` | `library.track_metadata`: exact local SD-path lookup in the scanned database, returning `id`, `path`, `title`, `artist`, `album`, `album_artist`, and `genre`. Returns `nil` for unindexed/remote/invalid paths; absent tags are empty strings. Does not parse files or iterate the library on each track. |
| `get_crossfade()`, `set_crossfade(enabled)` | `playback.settings`: boolean; retains native coupling with gapless. |
| `get_gapless()`, `set_gapless(enabled)` | `playback.settings`: boolean; disabling gapless also disables crossfade. |
| `get_replaygain_mode()`, `set_replaygain_mode(mode)` | `playback.settings`: `off`, `track`, or `album`. Applies at the next track transition; it does not recompute current decoder gain. |
| `get_play_mode()`, `set_play_mode(mode)` | `playback.settings`: `sequential`, `repeat_all`, `repeat_one`, or `shuffle`. Setters follow native settings persistence. |
| `set_ab_loop(start, finish)`, `get_ab_loop()`, `clear_ab_loop()` | `playback.ab_loop`: seconds converted to decoder frames; end is exclusive. Setter returns acceptance. Getter returns `{ start, finish }` or `nil`. Supports finite local FLAC/WAV/AIFF/CAF at up to 16-bit PCM; unsupported sources, pause, speed changes, crossfade, seek, stop, and track changes clear/reject loops. Wrap runs in the audio thread without reopening/flushing output. |
| `prepare_ab_switch(path)` | `playback.ab_switch`: asynchronously prepares an alternate SD file against the currently playing primary. Returns acceptance, not readiness. Both must be finite local 16-bit FLAC/WAV/AIFF/CAF or MP3 with identical rate, channels and playable frame count. MP3 delay/padding follows the native decoder's trimming; unmatched lengths are rejected. Preparation/adoption can involve I/O and one alignment seek; long MP3 preparation builds its seek index off-thread. The practice looper retains its lossless-only restriction. |
| `get_ab_switch()`, `select_ab_source(source)`, `clear_ab_switch()` | `playback.ab_switch`: snapshot `{ preparing, ready, source }`; source is `a` or `b`. Select returns false until ready. Both decoders advance together; selection switches raw PCM at a chunk boundary before one shared DSP pass, with the primary's ReplayGain, EQ and volume. Selection does not seek, reopen, or flush output. Pause, seek, stop, track/speed/crossfade changes or decode/output errors clear it. Loops and comparison are mutually exclusive. |

`plugin.on("output_changed", function(current, previous) ... end)`
(`playback.output_events`) reports deduplicated output snapshots on the UI thread.
Each contains `route`, `requested_route`, `active`, `bluetooth_connected`, `bluetooth_codec`, and
`headphone_state` (native jack state). The first observed state establishes a baseline.
Bluetooth information comes from the player's cached/debounced radio state.

`playback.http_seek` indicates finite HTTP Range support. Servers must honor
validated byte ranges. FLAC, MP3 and AAC in MP4/M4A use byte seeking; raw ADTS AAC
and live/chunked radio remain forward-only. MP3 without a seek table can decode
forward from an earlier byte position during a seek, so acceptance is not a
promise of instantaneous seeking. Provider URL expiry, server policy and
container format can still make an individual source non-seekable.

### `plugin.media_capabilities()`

Describes what the player can play, for streaming plugins choosing a quality:

```lua
-- codecs = {"mp3", "aac", "flac"}, containers = {"mp3", "adts", "flac"}
-- max_bit_depth = 16, max_channels = 2
-- max_sample_rate = 0 (unknown), direct_http_streaming = true
-- range_seeking = false, hls = false, dash = false
-- encryption_modes = {}, drm_systems = {}
```

## Music library

The library functions read the on-device database. Matching is
case-insensitive.

- `plugin.get_artist_albums(artist)`: the artist's album names, sorted, or
  `nil`.
- `plugin.get_album_tracks(artist, album)`: the album's track paths in album
  order, or `nil`.
- `plugin.get_next_album_tracks(artist, album)`: the tracks of the artist's
  next album, or `nil` if there is none (it doesn't wrap). Useful to keep
  playing through a discography.

### Paged access

For browsing the whole library without loading it all. `limit` defaults to,
and is capped at, 200; use `offset` to page. Check `library.paged`.

- `plugin.library_song_count()`: number of songs.
- `plugin.library_get_songs([offset], [limit], [filters])`: returns
  `songs, total`. `filters` may contain `query` (part of the title or artist),
  `artist`, `album_artist` and `album` (exact matches). `total` counts all
  matches, for "page N of M".
- `plugin.library_search(query, [limit])`: songs whose title or artist
  contains `query`.
- `plugin.library_get_song(id)`: one song, or `nil` if it no longer exists.
- `plugin.library_get_artists([offset], [limit])`: artist groups.
- `plugin.library_get_albums([offset], [limit], [artist])`: album groups,
  optionally only those whose artist or album artist is `artist`.

A **song** is `{ id, path, title, artist, album, album_artist }`. Its `id`
survives rescans: keep it to look the song up later with
`library_get_song(id)`. A **group** is
`{ name, count, first_song_id, album_artist }`; `album_artist` tells apart
same-named albums by different artists.

```lua
local songs, total = plugin.library_get_songs(0, 50, { artist = "Boards of Canada" })
plugin.show_toast(("Found %d songs"):format(total))
```

### `plugin.refresh_library()`

Rescans the music library, like Settings > Library > Update Music Database,
with the same progress screen. Needed after a plugin adds music files, since
the library doesn't watch for changes. Returns `true, "started"`, or
`false, "already_running"` or `false, "rate_limited"` (one scan per minute per
plugin). Check `library.refresh`.

## Files and playlists

- `plugin.sd_root()`: the SD card's path (`/data/mnt/sd_0` on the device,
  `./music` on the simulator). Build every path from it so the plugin works in
  both.
- `plugin.list_dir(path)`: the entries of a folder (absolute path), as
  `{ name, dir, size, modified }` tables (`size` and `modified`, in Unix
  seconds, when available). Hidden entries are skipped, the order isn't
  sorted, and a missing folder gives an empty table.
- `plugin.mkdir(path)`: creates a folder and any missing parents. Returns
  `true`, or `nil, error`. An existing folder is fine.

### File Manager (API 16)

Check `plugin.has_capability("filesystem.file_ops")` before the file
operations, and `"ui.file_manager"` before `open_file_manager`.

- `plugin.open_file_manager([folder])`: opens More > File Manager, at `folder`
  when given (an existing folder on the SD card; a path elsewhere raises a
  Lua error). Returns `true`, or `nil, error` when the folder does not exist
  or the File Manager is already open under another screen.
- `plugin.file_copy(paths, dest_dir [, callback])`,
  `plugin.file_move(paths, dest_dir [, callback])`,
  `plugin.file_delete(paths [, callback])`: copy, move or delete files and
  whole folders in the background, with the File Manager's own engine.
  `paths` is one absolute path or a list of them (up to 4096). Returns `true`
  once the operation starts, or `nil, error`.
  - Every path, and `dest_dir`, must be on the SD card (under
    `plugin.sd_root()`), written plainly: no trailing `/` and no `.`, `..`
    or empty parts. Any other path raises a Lua error.
  - Nothing is overwritten: a name already taken in `dest_dir` gets a
    " (2)" style suffix. A folder cannot be copied or moved into itself, and
    moving an item into the folder it is already in does nothing.
  - The player's own folders, `sd_root() .. "/.compas"` and
    `sd_root() .. "/.plugins"`, cannot be deleted or moved this way: the
    call returns `nil, "the player's own folders cannot be deleted or moved"`.
  - Only one operation runs at a time, shared with the File Manager:
    starting another gives `nil, "another file operation is running"`.
    While a computer has the card mounted over USB storage it gives
    `nil, "USB storage is connected"`.
  - `callback(result)` runs when the operation ends, with
    `{ done, total, failed, stopped, running, current }`: `failed` counts
    items not fully done, and `stopped` is true when
    `cancel_file_operation()` left items undone. It gets `nil` in the rare
    case a newer operation replaced the result first. If the plugin is
    unloaded the operation still finishes, without the callback.
- `plugin.file_operation_status()`: the running or last operation, whoever
  started it, as the same table as `callback`'s.
- `plugin.cancel_file_operation()`: stops this plugin's operation after its
  current file (a partly copied file is removed). Returns whether there was
  one; an operation the user started is left alone.

```lua
local music = plugin.sd_root() .. "/Music"
plugin.file_copy({ music .. "/a.flac", music .. "/Album" }, plugin.sd_root() .. "/Backup",
    function(result)
        if result and result.failed == 0 then plugin.show_toast("Backup done") end
    end)
```

File changes do not update the music library by themselves; call
`plugin.refresh_library()` afterwards when music moved.

### Playlists

These work on the `.m3u` playlists the Playlists screen shows, and changes
appear there immediately.

| Function | Returns |
| --- | --- |
| `plugin.playlist_list()` | paths of all playlists, or `nil` if there are none |
| `plugin.playlist_read(m3u_path)` | song paths in order, or `nil` |
| `plugin.playlist_create(name, song_path)` | the new playlist's path (with `song_path` as its first song), or `nil` |
| `plugin.playlist_add(m3u_path, song_path)` | `true`, or `nil, error` |
| `plugin.playlist_remove(m3u_path, song_path)` | `true`, or `nil, error`; removes every copy of the song |
| `plugin.playlist_delete(m3u_path)` | whether it was deleted, or `nil, error` for an invalid path |

## Sound

### Equalizer

These control the player's 10-band parametric EQ. Every change applies and is
saved immediately, and the EQ screen shows it.

| Function | Does |
| --- | --- |
| `plugin.eq_load_profile(path)` | Loads a `.peq` profile. Returns `false` if it can't be read. |
| `plugin.eq_save_profile(path)` | Saves the current EQ as a `.peq` profile, the same format as the EQ screen. Returns `false` if it can't be written. |
| `plugin.eq_reset()` | Restores the defaults. |
| `plugin.eq_set_bypass(on)` | `true` turns the whole EQ off. |
| `plugin.eq_set_preamp(db)` | Sets the preamp gain. |
| `plugin.eq_get_stereo_width()` | Gets the configured width (API 16, capability `audio.stereo_width`). |
| `plugin.eq_set_stereo_width(width)` | Sets width from 0 (mono fold) through 1 (neutral) to 2 (wider). Rejects nonfinite/out-of-range values. |
| `plugin.eq_set_band(index, freq_hz, gain_db, q)` | Sets one band. `index` is 1 to 10. |
| `plugin.eq_set_band_type(index, type)` | `"peaking"`, `"low_shelf"` or `"high_shelf"`. |
| `plugin.eq_set_band_enabled(index, on)` | Turns one band on or off. |

Stereo width is saved as `stereo_width` in `.peq` profiles, included in backups,
restored to 1 when loading older profiles, and reset by `eq_reset()`. EQ bypass
also bypasses width. Slider changes ramp over at most 20 ms; mono input is
unchanged. Widening applies headroom compensation, reducing the center level.
Native DSD over PCM (DoP) bypasses all PEQ processing, including width.
This is stereo width, not crossfeed or a room simulation. The EQ frequency graph
shows the band/preamp response and excludes this channel-dependent matrix.

A bad index or type is an error. Since every call writes
to storage, change only the bands that changed.

### `plugin.set_hw_volume_curve(curve)`

Replaces how the volume slider maps to the internal DAC, for example to
recreate the stock firmware's Low and High Gain modes. `curve` is a table of
exactly 101 integers (volume 0% to 100%), each a raw register value from 0
(loudest) to 255 (quietest). `nil` restores the built-in curve. It applies
immediately but isn't saved, so set it again on every load. It affects only
the headphone and line output, not Bluetooth or USB. See
`plugins_examples/GainMode.lua`.

## Device

### LEDs

Plugins can drive the red and blue charge LEDs while the player is awake.
First check `plugin.led_available()`.

| Function | Does |
| --- | --- |
| `plugin.led_set(color, on_or_level)` | `color` is `"red"` or `"blue"`. `true` (full), `false` (off) or a level 0 to 100. Works even when the LED Indicator setting is off. |
| `plugin.led_blink(color, on_ms, off_ms [, level])` | Blinks. Each phase is clamped to 50 to 60000 ms. |
| `plugin.led_breathe(color, rate [, level])` | Pulses. `rate` up to 600 means breaths per minute; above 600, a period in ms (up to 120000). `level` is the peak, default 100. |
| `plugin.led_get(color)` | `{ mode, level }`, `mode` being `off`, `on`, `blink`, `breathe` or `status`. |
| `plugin.led_status([color])` | Returns one LED (or both) to showing the charge status. |
| `plugin.led_release()` | Gives both LEDs back to the player. |

Levels are clamped to 0 to 100 and default to 100 for effects. The last
plugin to use the LEDs owns them until it releases them, fails, or
the plugins reload. Effects pause during suspend and resume afterwards.

```lua
if plugin.led_available() then
    plugin.led_blink("red", 300, 700, 80)
    -- later:
    plugin.led_release()
end
```

### Battery

`plugin.get_battery()` returns `{ level, charging, full, external_power }`,
`level` being 0 to 100 (0 without a battery). See also the `battery_changed`
event.

## Network

### `plugin.http_request(options, callback)`

The way to make HTTP requests. It returns a handle immediately, the request
runs in the background, and `callback(status, body, error, headers)` runs when
it finishes. On success `error` is `nil` and `headers` is a table of response
headers; on failure `status` and `body` are `nil` and `error` says why.

If the request can't start, it returns `nil, error` instead (for example
`"too many active HTTP requests"`), and the callback never runs.

```lua
local handle = plugin.http_request({
    url = "https://example.com/api",       -- the only required field
    method = "POST",                       -- GET (default), POST, PUT, PATCH, DELETE, HEAD
    headers = { Authorization = "Bearer " .. token },
    body = '{"hello":"world"}',            -- sent only with POST, PUT and PATCH
    content_type = "application/json",
    verify_tls = true,                     -- default true
    max_response_bytes = 262144,           -- default 512 KB, at most 2 MB
    connect_timeout_ms = 10000,            -- timeouts: none by default, at most 5 minutes
    read_timeout_ms = 15000,
    total_timeout_ms = 30000,
    redirect_limit = 5,                    -- default 0, at most 10
}, function(status, body, err, headers)
    if err then plugin.show_toast(err) return end
    plugin.show_toast("HTTP " .. status .. ", " .. #body .. " bytes")
end)
```

Limits and rules:

- Up to 4 requests at a time, from all plugins together.
- Request body up to 1 MB. Up to 32 headers, names up to 63 bytes, values up
  to 767 bytes, plus one value per request up to 4095 bytes (for long tokens;
  not for Host, User-Agent, Content-Type, Content-Length or Connection). Going
  over a limit, or a value out of range, raises an error instead of being cut.
- Without `redirect_limit`, a 3xx response is returned as a normal result.
  Redirects follow browser rules (303, and 301/302 after a POST, become a GET
  without body), and credentials (`Authorization`, `Cookie`, `Cookie2`,
  `Proxy-Authorization`) are never sent to a different origin (scheme, host
  or port). A redirect from
  HTTPS to HTTP is refused while `verify_tls` is on.
- If a response repeats a header, the last one wins.

Error values: `invalid_url`, `invalid_request` (a line break in the URL or a
header), `dns_failure`, `connect_failed`, `connect_timeout`, `tls_failure`,
`timeout`, `cancelled`, `too_many_redirects`, `response_too_large`,
`malformed_response`, `io_error`, `insecure_redirect`. They are stable, so you
can compare against them.

`plugin.cancel(handle)` stops a request (or download) and returns whether it
was still running. Its callback is then not called. The connection closes at
once, except while the host name is being looked up, which finishes first.

### `plugin.get_download_progress(handle)` — API 16

Check `network.http.download_progress`. Poll the handle returned by
`download_file_async` from an existing interval while the request is active.
Returns `{ downloaded = bytes, total = bytes }`, or `nil, error` for a stale,
cancelled, foreign, or non-download handle. `total = 0` means unknown size (for example a chunked transfer);
keep the bar indeterminate. For a positive total, clamp `downloaded / total`
to `0..1`. Bytes describe the network transfer, not completion of the final
rename, validation, or plugin storage work. The completion callback remains
the authority for success. Snapshots are coherent and no Lua or UI code runs
on the download worker. API 15 download signatures and callbacks are unchanged.

### `plugin.download_file_async(url, dest_path [, verify_tls], callback)`

Downloads a file straight to disk in the background, without loading it into
memory. `verify_tls` defaults to `true`. It returns a handle, or `nil, error`
if it can't start. `callback(saved_path, error)` runs when it ends; on failure
it gets `nil, "download failed"`. The file only appears at `dest_path` after
a complete HTTP 2xx response, so a failed download never leaves a broken file
or replaces an existing one. It shares the 4 request slots and can be
cancelled with `plugin.cancel()`.

```lua
local dir = plugin.sd_root() .. "/Talks"
if plugin.mkdir(dir) then
    plugin.download_file_async("https://example.com/talk.mp3", dir .. "/talk.mp3",
        function(path, err) plugin.show_toast(err or ("Saved " .. path)) end)
end
```

### `plugin.http_get(url [, verify_tls])` / `plugin.http_post(url, body [, content_type] [, verify_tls])`

Older, blocking requests: the whole player freezes until they finish. Use
`http_request` instead. They return `status, body` (HTTP error codes such as
404 included), or `nil, "network error"`. `http_post` sends `body` as is, with
`content_type` defaulting to `application/x-www-form-urlencoded`.

## Data and storage

### `plugin.storage` and `plugin.secrets`

Private key-value storage for your plugin (keyed by its `id`, so call
`plugin.define()` first), kept on the player's internal memory, not the SD
card.

```lua
plugin.storage.set("last_sync", tostring(os.time()))
local last = plugin.storage.get("last_sync", "never")   -- with a default
local keys = plugin.storage.list("cache_")              -- keys starting with "cache_"
plugin.storage.delete("last_sync")

plugin.secrets.set("access_token", token)
local token = plugin.secrets.get("access_token")         -- nil if missing
if plugin.secrets.exists("access_token") then ... end
plugin.secrets.delete("access_token")
```

- `get(key [, default])` returns `default` (or `nil`) for a missing key.
  `list([prefix])` lists all keys when the prefix is omitted or empty.
- Values are strings (binary is fine), up to 256 KB each. Setting the same
  value again doesn't write to storage again.
- `set` and `delete` return `true` on success. `false` can mean a limit was
  reached, or that the change was made but couldn't be confirmed as safe from
  a sudden power loss, so don't retry in a loop.
- Limits: 2 MB and 500 keys per plugin (storage and secrets together), 8 MB
  for all plugins.
- `secrets` is for tokens and passwords. It has `get`, `set`, `exists` and
  `delete` but no `list`, so secret names can't be enumerated. `get` returns
  `nil` for a missing secret (it was added later: check
  `storage.secrets_get`). Only your plugin can read its secrets, but they are
  **not encrypted**: anyone with root access to the device can. Don't describe
  them as encrypted to your users.

### `plugin.json_decode(text [, limits])` / `plugin.json_encode(value [, limits])`

Convert between JSON and Lua. They return `value` or `nil, error`.

| Limit | Default and maximum |
| --- | --- |
| `max_input_bytes` (decode) / `max_output_bytes` (encode) | 512 KB |
| `max_nesting` | 32, at most 64 |
| `max_entries` | 10000 |

Larger values are lowered to the maximum. Keep in mind:

- JSON `null` becomes `nil`, so it looks like a missing key.
- Numbers are exact only up to 2^53: treat large ids as strings.
- A table with items `1..n` becomes a JSON array; any other table, even an
  empty one, an object. Object keys must be strings.
- Invalid UTF-8 is an error.

### `plugin.md5(text)`

Returns the MD5 hash of `text` in lowercase hex, for APIs that sign requests
this way (like Last.fm).

### ZIP files and EPUBs

- `plugin.zip_list(path)`: the names of all entries, folders included.
- `plugin.zip_read(path, entry)`: one entry's contents (`entry` is the exact,
  case-sensitive name), as `bytes, info` with `info.method` (0 stored, 8
  deflate), `info.compressed` and `info.uncompressed`. Both the compressed and
  the uncompressed size must be at most 256 KB.

Both return `nil, reason` on failure: `not_found`, `encrypted`,
`zip64_unsupported`, `unsupported_method`, `corrupt`, `crc_mismatch`,
`entry_too_large`, `io_error` or `nomem`. Archives with more than 2000 entries
or a directory larger than 256 KB count as `corrupt`.

### Image thumbnails

- `plugin.zip_image_async(path, entry, dest, max_w, max_h, callback)` makes a
  thumbnail from a JPEG, PNG or BMP inside a ZIP (an EPUB cover, for example),
  up to 3 MB.
- `plugin.image_thumbnail_async(source, dest, max_w, max_h, callback)` does
  the same from an image file, up to 3 MB. Check `data.image_thumbnail`.

Both scale the image to fit `max_w` x `max_h` (16 to 800), never enlarging or
cropping it, and write it to `dest`, which must end in `.bin`. The result can
be used as a `show_list` icon or a `show_text_view` picture. They run in the
background and call `callback(dest)` or `callback(nil, reason)`.

- They return `true`, or `nil, "busy"` while another thumbnail job (from any
  plugin) runs: try again a bit later.
- Failure reasons include the ZIP reasons, `"unsupported image"`, `"could not
  decode image"`, `"could not write image"`, `"could not open image"`,
  `"not a regular file"`, `"image too large"` and `"could not read image"`.
  `"busy"` and `"nomem"` are temporary (the player was decoding artwork), so
  don't treat the image as broken.
- `dest` is replaced only on success. Jobs are cancelled, without callback,
  when the plugins reload.

### `plugin.html_to_blocks(bytes [, options])`

Turns an HTML or XHTML chapter into a list of blocks for `show_text_view`.
Returns a list of `{ kind, level, text }` with a `truncated` flag, or
`nil, reason` (`input_too_large` over 512 KB, `bad_args`, `nomem`).

- `kind` is `"p"` (paragraphs, `div`, `li`, `blockquote`), `"h"` (headings,
  with `level` 1 to 6), `"img"` (`text` is the image source, `alt` its alt
  text) or `"hr"`.
- Scripts, styles, `head` and comments are dropped. Common entities are
  decoded, whitespace is collapsed, and `<br>` becomes a line break.
- Output stops at 2000 blocks or 256 KB of text, with `truncated` set.
- `options` is reserved; if given, it must be a table.

## Events and timers

### `plugin.on(event, callback)`

Runs `callback` whenever something happens. Every subscribed plugin is
called. Up to 16 subscribers per event; an unknown event name is an error.

| Event | Arguments | When |
| --- | --- | --- |
| `track_started` | `title, artist, album, duration_seconds, provider, track_id` | A new track starts, for any reason. `provider` and `track_id` are `""` except for `play_remote` tracks. |
| `paused`, `resumed` | none | Playback pauses or resumes. |
| `stopped` | none | Playback stops (not when a queue simply ends; see `queue_exhausted`). |
| `queue_exhausted` | `direction` (`1` next, `-1` previous) | Next or Previous is pressed with no track left in that direction. |
| `screen_woke` | none | The screen turns back on. |
| `volume_changed` | `percent` | The volume changes, at most twice a second. |
| `battery_changed` | same table as `get_battery()` | A battery value changes. |
| `screenshot_saved` | `path` | A screenshot was saved. |
| `screenshot_failed` | `reason` | A screenshot couldn't be saved. |
| `suspending` | none | The player is about to sleep (it can't be stopped). |
| `system_resumed` | none | The player woke from sleep. |

### `plugin.set_interval(seconds, callback)` / `plugin.clear_interval(handle)`

Runs `callback` every `seconds` (at least 1) until cleared. Returns a handle;
clearing an unknown or already cleared handle does nothing. Up to 16 timers
from all plugins together. Callbacks run on the interface thread, so keep
them quick.

## Examples

Files in this table are in `plugins_examples/`, except ListenBrainz Scrobbler and Radio Browser, which are installed from the plugin store.

| Example | Shows |
| --- | --- |
| `Audiobooks.lua` | Books row, folder scanning, resume, bookmarks, chapters, sleep timer |
| `Podcasts.lua` | RSS and OPML, episode downloads, resume |
| `NetRadio.lua` | Stream Media tile and radio streams from `Radio.txt` |
| `HiByStockPlayer.lua` | A Player layout in the style of the stock HiBy OS player, with its own lyrics area, offered in Settings and applied from a row |
| `Themes.lua` | Historical reference format: icons, colors, Home layout, saved choice, `refresh_theme` |
| `SoundProfiles.lua` | Switching EQ profiles |
| `MSEB.lua` | Chained settings screens, sliders sharing EQ bands, backup and restore |
| `GainMode.lua` | Low/High Gain volume curves |
| `PlaybackExtras.lua` | Toggles, sliders, nested settings, row icons |
| `PlayThrough.lua` | Continuing into the next folder or album when a queue ends |
| `ExtendedSleepTimer.lua` | A longer sleep timer with `stop()` |
| `LastFmScrobbler.lua` | Events, timers, text input, MD5, async HTTP |
| `AsyncHttp.lua` | `http_request` and `cancel` |
| `PluginApiInfo.lua` | Version and capability checks |
| `NestedLists.lua` | Stacked lists and busy text input |
| `EpubReader.lua` | ZIP and HTML helpers, paged reading, saved position |
| `LyricsFetcher.lua` | LRCLIB synced `.lrc` sidecar for the current local track |
| `CoverArtFetcher.lua` | MusicBrainz + Cover Art Archive `cover.jpg` for the current album folder |
| `AlbumShuffle.lua` | Random albums with in-album track order, continued on `queue_exhausted` |
| `ListenBrainzScrobbler.lua` | ListenBrainz user-token scrobble and offline queue (plugin store) |
| `RadioBrowser.lua` | Radio Browser search and `Radio.txt` favorites (plugin store) |

## Example plugins: lyrics, covers, album shuffle

Copy any of these from `plugins_examples/` to `<SD card>/.plugins/`, then **Refresh Plugins**.

### Lyrics Fetcher

Settings > Library > **Lyrics Fetcher**.

- **Fetch lyrics for current track** looks up [LRCLIB](https://lrclib.net/docs) with the playing title, artist, album and duration (no API key) and, if `syncedLyrics` is present, writes a sidecar next to the file. Requires plugin API 15.
- The sidecar name matches native `lyrics_load_sidecar()`: the audio filename's extension is replaced with `.lrc` (a file with no extension gets `.lrc` appended). An existing `.lrc` is never replaced. The API JSON is never saved as lyrics.
- **Automatic fetch** is off until you turn it on. It runs on `track_started` only for absolute local files (`/…`). `http://` / `https://` streams, `remote://` tracks, and relative paths are skipped.
- No match, instrumental, unsynced-only, or a network error does not create a `.lrc`. Embedded tag lyrics are not visible to plugins, so a track that already has USLT/Vorbis lyrics may still get a sidecar.

### Cover Art Fetcher

Settings > Library > **Cover Art Fetcher**.

- **Fetch cover for current album** searches MusicBrainz for that artist + album, then downloads a **500px (or 250px) front JPEG** from the Cover Art Archive into `cover.jpg` in the album folder. Requires plugin API 15.
- An existing `cover.jpg` is never overwritten. Lookups require both artist and album tags and refuse an ambiguous or unrelated release (for example two different artists' albums that share a title). Album artist is taken from the library row whose path is the current file, not the first song that merely shares the title.
- **Automatic fetch** is off until you turn it on. Failures are quiet in automatic mode and are not retried for several hours, so a missing cover or an offline network does not toast every track.
- MusicBrainz is called at most once per second with a contactable User-Agent. Image URLs are probed with `HEAD` (no automatic redirect follow); `Location` is followed for up to five hops, including relative URLs, then `plugin.download_file_async` fetches the resolved URL. Images larger than 2 MiB are rejected.
- Only a `front` image with a 500px or 250px thumbnail is used. Back or unsized originals are not saved. The Lua API has no field for embedded pictures inside files. Folders that already show art from tags may still get a `cover.jpg`. After a successful save the plugin calls `plugin.refresh_library()`.
- Only absolute local files are used; `remote://` tracks and streams are skipped.

### Album Shuffle

Settings > Playback & Controls > **Album Shuffle**.

- **Start album shuffle** picks a random album through paged `plugin.library_get_albums`, resolves identity with `plugin.library_get_song(first_song_id)` (preferring `album_artist` so compilations keep every track), loads `plugin.get_album_tracks(artist, album)` in album order, and plays them with `plugin.play_list(paths, 1)`. Requires plugin API 15.
- When the queue is exhausted forward (`queue_exhausted` with `direction == 1`), or the last track ends naturally, another album is chosen. Immediate repeats are avoided when more than one album exists. An empty library is reported with a toast.
- Native shuffle would reorder tracks inside the album, so start this while the player is already in **sequential** mode (or select it with `plugin.set_play_mode("sequential")` on builds advertising `playback.settings`). Explicit Stop, picking a different track/queue, or **Stop album shuffle** ends the session so the plugin does not take over playback.

Host-side mocked tests for these three live in `plugins_examples/tests/` (`lua plugins_examples/tests/run.lua`).

## Testing and debugging

1. Write the plugin in any text editor. `luac -p MyPlugin.lua` catches syntax
   errors before copying.
2. Copy it to `<SD card>/.plugins/`.
3. Tap **Refresh Plugins** in Settings > System > Plugin Manager (this reloads
   every plugin and returns to Home), or restart.
4. Watch the player's output through ADB.

Errors look like this:

| Message | Meaning |
| --- | --- |
| `[plugins] failed to load <path>: <error>` | A syntax error or an error in the top-level code. |
| `[plugins] <context> error: <error>` | A callback failed; `<context>` names which one. |

Plugin Manager can switch each plugin off. Switching one off stops it from
loading; it doesn't undo what it already changed. Copied icons and EQ settings,
for example, stay until changed again.

## Extending the API (C developers)

To add a function plugins can call, follow the existing `l_plugin_*`
functions in `src/plugins/plugin_manager.c`: read the arguments with
`luaL_check*`/`luaL_opt*`, do the work, push the results, return their
count, and add the function to `plugin_funcs[]`.

- If it touches screens, widgets or other UI state, go through a
  `gui_plugin_*` function in `gui.c` (declared in `gui.h`), like `show_list`,
  `play_file` and the playback controls do.
- If it uses a self-contained module with no UI (`peq.c`, `http_client.c`),
  call it directly, like the `eq_*` and HTTP functions do.

When unsure, look at what the native UI does around the same call. If it also
updates icons, saves state or checks a condition, that belongs in the `gui.c`
bridge too, so a plugin can't skip it.

Add a capability token to `plugin_capabilities[]` so plugins can detect the
new feature, and document it here.
