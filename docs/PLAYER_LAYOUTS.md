# Player layouts

The Player (Now Playing) screen can be built from more than the hand-written
layout in `src/ui/gui_player.c`. A layout is only widgets, geometry, styles and
animations. The app finds the widgets it cares about by name and attaches
everything that does something (taps, seeking, touch targets, theme colors,
the lyrics view). Three kinds of layout exist:

| Kind | Where it comes from | Registered by |
| --- | --- | --- |
| Built-in | `player_layout_create_builtin()` in `gui_player.c` | always, id `default` |
| Exported C | a component exported from XML to C by LVGL's UI Editor, compiled into the app | `player_layouts_register_c()` |
| XML file | an LVGL XML component read from the device at runtime | found by scanning, or `plugin.set_player_layout{xml=...}` |

The built-in layout is the default and the fallback. It renders exactly as it
always did.

## Choosing a layout

Settings > Display > Player Layout > Layout lists every registered layout and
applies the choice by rebuilding the UI (the same soft reload plugins use).
The choice is saved in `settings.txt` as `player_layout=<id>`; an empty value
or an id that no longer exists means the built-in layout. A plugin can select
an XML layout for the current session only, see [Plugins](#plugins).

If a layout cannot be built (missing or invalid file, a required widget
missing), the app logs a line to stderr and builds the built-in layout instead.
The Player is never empty and never crashes on a bad layout.

## XML files

### Where they are found

Files named `*.xml` are found in these directories, later ones replacing an
earlier file with the same name:

1. `<theme root>/player_layouts/`: `/usr/resource/litegui/theme2/player_layouts/`
   on the device (the repository's `assets/theme2/player_layouts/` ships there
   with the firmware image), `assets/theme2/player_layouts/` on the simulator.
2. `/usr/data/theme_overrides/player_layouts/` on the device, the writable
   override root that `asset_path()` checks first. Not used on the simulator.
3. `<SD card>/.plugins/player_layouts/` (`./music/.plugins/player_layouts/` on
   the simulator), next to the plugins.

The file name without `.xml` is the layout id and the name shown in Settings.
Ids use letters, digits, `_`, `-` and `.`. `default` is reserved. The list is
rescanned every time the UI is built and every time the Layout screen opens.

A layout must be one self-contained file, at most 256 KB. It is registered as
one component, so it cannot include other XML files.

### Board variants

For `foo.xml` the app uses `foo@<W>x<H>.xml` instead when that file exists
next to it: `foo@480x800.xml` (R1), `foo@480x720.xml` (R3 Pro II),
`foo@320x480.xml` (R3 II 2025). The variant is not listed separately.

### Shape of a file

One `<component>` with an optional `<consts>`, `<styles>` and `<animations>`
and a `<view>`. The `<view>` is the root of the layout and is created as the
only child of the Player screen. The format and the widgets available are
documented in LVGL's XML docs (`lvgl/docs/src/xml/`), with these limits:

- The XML engine is LVGL 9.4's, vendored in `third_party/lv_xml/` because 9.5
  removed it. Everything in the 9.4 docs applies, nothing newer does.
- The layout is registered under a fixed component name, so `<view>` tags must
  not refer to other components. Plain widgets (`lv_obj`, `lv_label`,
  `lv_image`, `lv_slider`, `lv_button`, `lv_bar`, `lv_arc`, `lv_switch` and the
  other built-in ones) are fine.
- Image names in `<styles>` (`bg_image_src`) cannot use the names below, since
  styles are read before the images are registered. Use an `<lv_image>` widget
  instead.
- There is no way to call app functions from XML. Behavior comes from the
  roles below.

`assets/theme2/player_layouts/example_minimal.xml` is a complete example: all
roles, a flex-based arrangement that fits every screen, and all four
timelines. Copy it to start a layout.

### Fonts, images and constants

Registered for XML layouts, usable by name:

- Fonts: `player_title` and `player_meta` (the Player's own fixed sizes),
  `font_16`, `font_20`, `font_22`, `font_28` (the general UI sizes, which follow
  Settings > Font Size and a custom font), and `lv_font_default`. Use them as
  `style_text_font="player_title"`.
- Images, as the file name without `.png` from `playing_plane/` (resolved
  through `asset_path()`, so theme overrides apply): `default_cover`,
  `collect_out`, `collect_in`, `quality_waveform`, `btn_prev`, `btn_prev_s`,
  `btn_next`, `btn_next_s`, `btn_play`, `btn_pause`, `ic_more`, `order`,
  `loop`, `single`, `random`, and `btn_back` (from `sub_back/`). Use them as
  `<lv_image src="btn_prev"/>`. The built-in layout loads the same files, so
  the image cache holds each one once.
- Constants: `screen_w` and `screen_h`, the screen size in pixels
  (`width="#screen_w"`).

Names the XML declares itself in `<consts>`, `<fonts>` or `<images>` take
precedence over these.

## Roles

A widget becomes a role by its `name` attribute (`lv_obj_set_name()` in C).
Names are looked up once when the screen is built, never per frame.

Required. If any is missing or has the wrong type, the layout is rejected:

| Name | Widget | What the app does |
| --- | --- | --- |
| `cover_card` | any | Container the cover is fitted into. Clipped and sized by the layout. |
| `cover_img` | `lv_image` | Shows the cover art. Tapping it opens and closes the lyrics view. Should be a child of `cover_card`: the app scales it to cover the card and centers it there. |
| `title` | `lv_label` | Track title. Scrolls when too long. |
| `play_btn` | `lv_image` | Play/pause icon, recolored to the accent. Tap toggles playback. |
| `progress_slider` | `lv_slider` | Seek bar, range 0 to 100. Gets the accent colors, a larger touch margin and exempts itself from the swipe-back gesture. |

Optional. A missing one (or one of the wrong type) is simply absent, and every
place the app updates it checks for that:

| Name | Widget | What the app does |
| --- | --- | --- |
| `artist`, `album` | `lv_label` | Artist (folder when untagged) and album. Scroll when too long. |
| `pos_label`, `dur_label` | `lv_label` | Elapsed and total time. |
| `song_count` | `lv_label` | Position in the queue ("3/12"), shown once a track plays. |
| `quality_pill` | any | Container of the format badge. Hidden while lyrics are open. |
| `format_badge` | `lv_label` | Codec, bit depth and sample rate. |
| `favorite_circle` | any | The heart's backdrop, which gets the accent outline. A touch target is added over it. |
| `favorite_icon` | `lv_image` | The heart glyph, swapped when the track is (un)favorited. |
| `order_btn` | `lv_image` | Play mode icon (order, loop, single, shuffle). Tap cycles the mode. |
| `prev_btn`, `next_btn` | `lv_image` | Previous and next. Tap skips, holding seeks. The pressed artwork (`btn_prev_s`, `btn_next_s`) is shown while held. |
| `more_btn` | `lv_image` | Opens the song menu. |
| `dismiss_btn` | any | Tap leaves the Player (same as back). |
| `overlay_panel` | any | Full-screen surface the blurred-cover background paints on. |
| `background_img` | `lv_image` | The blurred cover image, inside `overlay_panel`. |
| `volume_slider` | `lv_slider` | Mirrors the volume. An indicator only: it is made non-clickable. |

If a layout has no `overlay_panel`, `background_img`, `volume_slider`, `artist`
or `album`, the app creates hidden ones (the overlay behind everything)
because the background and volume code expect them, and because the artist and
album text is kept in those labels: Remote Control and the quick drawer read it
back from there, so it is tracked even when the layout does not show it. The
hidden labels are never laid out, scrolled, styled or moved by the lyrics view. `plugin.set_player_layout` background settings
(`flat`, `blur_radius`, ...) therefore still work with any layout, but the
blurred cover is only visible if the layout leaves what covers the overlay
transparent.

### What the app adds

- Touch targets. The transport icons and the heart are small, so the app adds
  invisible, larger targets beside them. For a custom layout each one is
  centered on the widget's final position and is at least 44x44. They are
  direct children of the Player screen, above the layout.
- Theme. Custom layouts get the theme's primary text color on `title` and
  `song_count`, the muted one on the other labels, the accent on the slider,
  the heart's outline and the play button's glyph, and the pressed-dimming on
  the icons. Properties the XML sets itself win over these.
- Text scrolling on `title`, `artist` and `album`: these labels scroll
  circularly. Give them a fixed width (for example `100%`) so they have a width
  to scroll within.
- The metadata block of the built-in layout (heights of the three labels) is
  not applied to other layouts: their labels are positioned by the layout.

## Animations

A layout may define these timelines (`<animations><timeline name="...">`).
All are optional. A missing one does nothing.

| Timeline | Played |
| --- | --- |
| `screen_enter` | when the Player screen is loaded |
| `track_change` | after a new track's title, artist and album are set |
| `lyrics_open`, `lyrics_close` | instead of the built-in morph, when opening and closing the lyrics view |

The lyrics pair works only when both exist. With the built-in morph (no pair),
the cover and metadata move to the top of the screen and the controls are
hidden; it works without `artist` and `album`, and moves a nested widget to the
same place on screen.

With the pair, the timelines own the motion. The app only:

- disables the touch targets for the duration;
- hides the layout's controls (every optional role above that is a control,
  except the cover and metadata) when `lyrics_open` ends, and shows them again
  when `lyrics_close` starts so they can animate in;
- puts the lyrics below the lowest of `cover_card`, `title`, `artist` and
  `album` when `lyrics_open` ends;
- refits the cover image to `cover_card`.

Timeline end values should match the layout's resting state, since leaving the
Player closes the lyrics by jumping `lyrics_close` to its end.

Timelines animate style properties only (see `ui_elements/animations.rst` in
LVGL's XML docs): opacity, size, padding, translate and so on. They do not scale
the cover image while the card resizes; it is refitted at the end.

## Exported C (XML to C)

LVGL's UI Editor can export a component as C:

```c
lv_obj_t * my_player_create(lv_obj_t * parent);
```

Add the exported `.c`/`.h` files to `APP_SRCS`, name the widgets with the roles
above in the XML before exporting, and register it once at startup, before the
first Player screen is built (for example at the top of `gui_init()`):

```c
#include "player_layouts.h"
#include "my_player.h"

player_layouts_register_c("my_player", "My player", my_player_create);
```

It then appears in Settings next to the others. A worked example, including
the optional timeline hook, is in `docs/player_layouts/README.md`.

## Plugins

`plugin.set_player_layout{...}` accepts three more fields next to the
background ones:

```lua
plugin.set_player_layout({
  xml  = "player_layouts/clean.xml",  -- file inside <SD card>/.plugins/
  id   = "clean",                     -- optional, default "plugin.<file name>"
  name = "Clean",                     -- optional, default the file name
})
```

`xml` is relative to the plugins folder. Absolute paths, `..` segments and
paths that resolve outside the folder (symlinks included) are rejected with a
Lua error, as is a file that does not exist. The layout is registered and
selected for this session only and is never saved. Called from the plugin's
top-level code it belongs to the plugin: it is registered again on each plugin
reload and gone when the plugin is. Called from a callback, the UI is rebuilt
to apply it and it stays selected until the app restarts. Choosing another
layout in Settings replaces it.

## Limits

- One XML layout is live at a time. Switching rebuilds the whole UI.
- A failed XML parse leaves no trace, but a file the parser accepts can still
  log warnings for attributes it does not know (LVGL's logging is off in
  release builds, so they are silent).
- The XML engine adds about 220 KB to the binary.
- `background_img` is always sized to the full screen and placed at its origin
  by the blurred-background code, wherever the layout put it.
