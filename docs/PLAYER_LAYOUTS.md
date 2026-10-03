# Player layouts

You can redesign the Now Playing screen without touching C code. A layout is
an LVGL XML file that says what the screen looks like. The app takes care of
what it does: playback, seeking, cover art, lyrics, theme colors.

If you don't install a layout, nothing changes. The built-in layout is the
default, and it is also the fallback whenever a custom one fails to load.

## Included designs

The Player Layout selector includes **Gallery**, **Panorama** and **Vinyl**,
each fitted to all three players. Tap the cover to open or close lyrics.
Gallery and Panorama retain controls in their lyrics views; Vinyl uses the
default full lyrics page with a circular cover. Gallery and Vinyl use the player's frosted glass cover pipeline behind
the artwork. Gallery keeps the menu alongside its playback controls. The designs reuse the player's existing icons.

Hiby’s is also included, with full-width square artwork, the translucent
status bar over the cover, a brighter frosted footer and XML lyrics
transitions. Board variants cover R1, R3 Pro II and R3 II 2025; the two
shorter screens omit the album line to keep controls clear.

Hiby’s Graph duplicates Hiby’s with the existing **filled envelope** waveform
seek bar. It uses XML only, reuses all icons and includes all three board
variants. Tap the cover for lyrics and seek directly on the waveform.

## Quick start

1. Copy `assets/theme2/player_layouts/example_minimal.xml` and rename it, for
   example `clean.xml`. The file name (without `.xml`) is the name shown in
   Settings.
2. Edit it. Keep the widget names listed under [Widgets the app looks
   for](#widgets-the-app-looks-for); everything else is up to you.
3. Copy it to the SD card, into `.plugins/player_layouts/`.
4. On the player, open **Settings > Display > Player Layout > Layout** and
   pick it. The interface reloads with the new layout, and the choice is
   remembered across restarts.

To go back, pick **Default** in the same list.

After editing the layout that is already active, pick another layout and then
yours again (or call `plugin.reload_ui()`) to load the new version.

## Where layout files go

The app looks for `*.xml` files in these folders. If two folders have a file
with the same name, the later one in this list wins:

| Folder | Use it for |
| --- | --- |
| `/usr/resource/litegui/theme2/player_layouts/` | Layouts shipped with the firmware (from `assets/theme2/player_layouts/` in this repo). |
| `/usr/data/theme_overrides/player_layouts/` | Layouts installed on the device's internal storage. |
| `<SD card>/.plugins/player_layouts/` | Layouts on the SD card. The easiest place for your own. |

On the simulator, the folders are `assets/theme2/player_layouts/` and
`./music/.plugins/player_layouts/`.

File names (without `.xml`) are 1 to 63 letters, digits, `_`, `-` or `.`,
must not start with `.`, and `default` is reserved. A file can be up to
256 KB, and the list holds up to 24 layouts including Default.

### One file per screen size

The players have different screens: R1 is 480x800, R3 Pro II is 480x720 and
R3 II 2025 is 320x480. A layout built with percentages and flex rows can fit
all of them. If it doesn't, add a version for a specific screen next to it,
named `name@WIDTHxHEIGHT.xml`:

```
clean.xml            used on every screen without its own file
clean@320x480.xml    used instead on the R3 II 2025
```

Only `clean` appears in Settings. The example layout ships with a 320x480
version you can compare with.

## Writing the XML

A layout is one LVGL XML `<component>`: optional `<consts>`, `<styles>` and
`<animations>`, plus a `<view>` holding the widgets. The format is described
in LVGL's XML documentation (`lvgl/docs/src/xml/` once the project has been
built once). Things to know:

- The app uses the **LVGL 9.4** XML engine, so follow the 9.4 docs.
- Use LVGL's own widgets (`lv_obj`, `lv_label`, `lv_image`, `lv_slider`,
  `lv_button`, `lv_bar`, `lv_arc` and so on). A layout cannot use other
  components or other XML files.
- XML cannot call app code. Interactive widgets work through the names below.
- Sizes are in pixels. Use `#screen_w`, `#screen_h`, percentages and flex
  layouts to adapt to the screen.

### Ready-made fonts, images and sizes

These names can be used directly:

- **Fonts:** `player_title` and `player_meta` (the built-in Player's title and
  artist fonts), `font_16`, `font_20`, `font_22` and `font_28` (these follow
  the Font Size setting and a custom font), and `lv_font_default`. Example:
  `style_text_font="player_title"`.
- **Images:** the Player's own icons, which also follow theme overrides:
  `default_cover`, `btn_play`, `btn_pause`, `btn_prev`, `btn_prev_s`,
  `btn_next`, `btn_next_s`, `ic_more`, `collect_out`, `collect_in`,
  `quality_waveform`, `order`, `loop`, `single`, `random`, `btn_back`, the
  pressed versions `btn_play_s`, `btn_pause_s`, `ic_more_s`, `collect_out_s`,
  `order_s`, `loop_s`, `single_s`, `random_s` and `speed_s`, and the stock
  Now Playing artwork `topbar_bg` (480x50), `bottom_panel` (480x320),
  `progress` and `progress_bg` (440x12), `speed` and `dlna`. A name whose file
  the theme doesn't have shows nothing.
  Example: `<lv_image src="btn_next"/>`. These names don't work for
  `bg_image_src` inside `<styles>`; use an `<lv_image>` widget there instead.
- **Your own images:** for a layout file `<folder>/<name>.xml`, every `*.png`
  directly inside `<folder>/<name>/` is available by its file name without
  `.png`, so `<folder>/clean/heart.png` is `<lv_image src="heart"/>`. Names
  use letters, digits, `_` and `-`, at most 32 files are used (in name order),
  and other files are ignored. A name your XML declares itself wins over the
  folder, and the folder wins over the ready-made names above. Plugins can
  ship the folder next to their layout.
- **Sizes:** `screen_w` and `screen_h`, the screen size in pixels. Example:
  `width="#screen_w"`.

Anything you declare in your own `<consts>`, `<fonts>` or `<images>` takes
priority over these names.

## Widgets the app looks for

The app finds widgets by their `name` attribute and brings them to life. The
rest of your layout is decoration.

**Required.** Without these the layout is rejected and the default one is
used:

| Name | Type | What it does |
| --- | --- | --- |
| `cover_card` | any | Frame for the cover art. Give it a size, rounded corners, clipping. |
| `cover_img` | `lv_image` | The cover art. Put it inside `cover_card`; it is scaled to fill the card. Tapping it opens and closes lyrics. |
| `title` | `lv_label` | Song title. Scrolls when too long. |
| `play_btn` | `lv_image` | Play/pause. Its icon switches automatically and takes the accent color. |
| `progress_slider` | `lv_slider` | Seek bar. |

**Optional.** Leave out what you don't want:

| Name | Type | What it does |
| --- | --- | --- |
| `seek_style` | `lv_label` | Hidden configuration label selecting a waveform seek-bar renderer. See below. |
| `artist`, `album` | `lv_label` | Artist (or folder name) and album. Scroll when too long. Tapping opens the matching local artist or album when the playing path is in the library. |
| `pos_label`, `dur_label` | `lv_label` | Elapsed and total time. |
| `song_count` | `lv_label` | Position in the queue, for example "3/12". |
| `format_badge` | `lv_label` | Codec, bit depth and sample rate. |
| `quality_pill` | any | Container around the format badge. |
| `favorite_circle` | any | Background of the heart button. Tapping it toggles the favorite. |
| `favorite_icon` | `lv_image` | The heart. Switches between empty and full. |
| `order_btn` | `lv_image` | Play mode (in order, repeat, repeat one, shuffle). Tap to change. |
| `prev_btn`, `next_btn` | `lv_image` | Previous and next. Hold to seek. |
| `more_btn` | `lv_image` | Opens the song menu. |
| `dismiss_btn` | any | Leaves the Player, like Back. |
| `overlay_panel` | any | Full-screen surface the blurred cover background is drawn on. |
| `background_img` | `lv_image` | The blurred cover, inside `overlay_panel`. |
| `volume_slider` | `lv_slider` | Shows the volume. Display only, it can't be dragged. |
| `lyrics_area` | any | Where the lyrics are shown. See [Lyrics in your own area](#lyrics-in-your-own-area). |
| `lyrics_active` | `lv_label` | Its text color is the current lyrics line's color. Normally hidden. |
| `lyrics_toggle` | any | Tapping it opens and closes the lyrics, like tapping the cover. |
| `cover_thumbnail` | `lv_image` | A compact second cover that shares the decoded artwork. Tapping it toggles lyrics. |
| `play_circle` | any | Container around `play_btn`. Its whole rectangle is included in the play button's touch target. |
| `transport_color` | `lv_label` | Normally hidden. Its text color tints the transport icons, heart, and `dismiss_icon`, useful for light backgrounds. |
| `lyrics_marker` | any | Normally hidden. Enables an accent rule beside the current synchronized lyric. |
| `cover_fade` | `lv_image` | A noninteractive image matching the cover rectangle. The player fills it with a stable, spatially dithered fade to the layout root's background color. It leaves the cover and thumbnail images unchanged. |

A widget with the right name but the wrong type is ignored (for a required
one, the layout is rejected).

### Waveform seek bars

Keep the required `progress_slider` and add a hidden label to select its renderer:

```xml
<lv_label name="seek_style" text="waveform_bars" hidden="true" />
<lv_slider name="progress_slider" x="24" y="580" width="432" height="30"
           style_bg_color="0x789096" style_bg_opa="255" />
```

The available renderers are `waveform_bars` (rounded, mirrored bars),
`waveform_half` (bars rising from a baseline), and `waveform_envelope`
(contiguous filled columns). The slider's rectangle sets the waveform size;
its background color sets the neutral waveform color. Played bars and the
position marker follow the current accent. Reserve space below for time labels
and at least a 44px touch target. The slider still handles seeking normally.

Omitting the label, or using an unknown value, keeps the normal LVGL slider.
Each waveform renderer shows a regular rail while data is unavailable.
Local files are decoded by a separate background worker into 256 RMS bins and
cached under `.compas/waveforms` on the SD card. The cache is capped at 64
entries and checks the file's path, size, and modification time. Changing
tracks cancels the previous request. Network streams, DSD, APE, unknown-duration
files, and files exceeding the extraction limits use the rail fallback.
Extraction keeps a 90-second time limit. Up to 16 failed file identities are
remembered in memory until restart or eviction, so revisiting an unchanged
unsupported track does not repeatedly decode it. Changing its size or
modification time allows another attempt. Cancelled scans remain retryable.
Generating a waveform never changes the playing decoder, volume, or effects.

### What you get for free

- **Theme colors.** Labels get the theme's text colors, and the slider, heart
  outline and play button get the accent color. These are applied after the
  layout's `<styles>`, so to use your own color set it directly on the widget,
  for example `style_text_color="0xFFFFFF"`.
- **Bigger touch areas.** Small buttons get an invisible touch area of at
  least 44x44 pixels around them. It is placed once, when the screen is built,
  so a timeline that moves a button doesn't move its touch area.
- **Scrolling text.** `title`, `artist` and `album` scroll when the text is
  too long. Give them a fixed width (for example `width="100%"`).
  Use `height="content"` to keep the full font height available, including
  at larger font settings, so the marquee scrolls horizontally.
- **Artist and album taps.** Keep the `artist` and `album` labels outside the
  cover image's hit area: tapping the cover toggles lyrics, while tapping a
  metadata label opens its matching local library group. Streams and tracks
  that are not indexed locally leave these labels inert.
- **The blurred background.** If you leave out `overlay_panel` and
  `background_img`, the app adds hidden ones behind everything. To see the
  blurred cover, keep whatever sits on top of them transparent.
- **Song info for other features.** If you leave out `artist` or `album`, the
  app keeps hidden copies, so Remote Control and the quick drawer still show
  them.

## Animations and the lyrics view

Add a `<timeline>` with one of these names to animate the screen. Each one is
optional:

| Timeline | Plays when |
| --- | --- |
| `screen_enter` | the Player opens |
| `track_change` | a new song starts (after its title and artist are shown) |
| `lyrics_open` | lyrics open (tap on the cover) |
| `lyrics_close` | lyrics close |

Timelines animate style properties such as opacity, size, position offsets
(`translate`) and padding. See `ui_elements/animations.rst` in LVGL's XML
docs.

**Lyrics without timelines.** If the layout doesn't define both
`lyrics_open` and `lyrics_close`, the standard animation runs: the cover
shrinks to the top-left corner, the song info moves next to it, the controls
hide, and the lyrics fill the space below.

**Lyrics with timelines.** If both exist, your timelines move things, and the
app:

1. turns off the buttons' touch areas while lyrics are open,
2. when `lyrics_open` finishes, hides these widgets: `order_btn`, `prev_btn`,
   `play_btn`, `next_btn`, `more_btn`, `progress_slider`, `pos_label`,
   `dur_label`, `quality_pill`, `dismiss_btn` and `song_count` (except one
   that contains, or sits inside, the cover or the song info), and always
   `favorite_circle`,
3. shows the lyrics below whichever of the cover, title, artist or album ends
   lowest,
4. shows those widgets again when `lyrics_close` starts, so they can animate
   back in.

Anything else on the screen, such as decorations, is left as is: hide or fade
it in your timelines.

### Lyrics in your own area

Without `lyrics_area` the lyrics fill the width of the screen below the cover
and song info. Add a `lyrics_area` (normally an empty transparent `lv_obj`) and
they appear exactly inside its rectangle instead:

- **You own the placement.** The app does not run its cover animation and does
  not hide any controls. With no `lyrics_open` and `lyrics_close` timelines the
  lyrics simply appear in the area and disappear on close. With both, they play
  as above and the lyrics appear in the area when `lyrics_open` ends.
- **Taps.** Touch targets that overlap the area are turned off while the
  lyrics are open, so they don't take the taps meant for the lyrics. Tapping a
  line seeks to it, as always. Tapping empty space inside the area closes the
  lyrics. `lyrics_toggle`, if you have one, opens and closes them in any mode.
- **Line width.** Lines wrap to the width of the area (with the usual side
  margins), not the screen's.
- **Alignment and colors.** The area's `style_text_align` sets the alignment
  of the lines (centered by default). Its own `style_text_color` is the color
  of normal lines (gray by default), and the `style_text_color` of the
  `lyrics_active` label is the color of the current line (the accent color by
  default). The colors also apply when there is no `lyrics_area`. They are read
  once, when the screen is built. The text size is the Lyrics Text Size
  setting; a layout can't change it.

```xml
<lv_obj name="lyrics_area" x="0" y="50" width="480" height="430"
        style_bg_opa="0" style_border_width="0"
        style_text_color="0xffffff" style_text_align="center"/>
<lv_label name="lyrics_active" text="" hidden="true" style_text_color="0x1062f2"/>
```

`plugins_examples/player_layouts/hiby_stock.xml` is a complete layout built
this way, with `lyrics_open` and `lyrics_close` timelines that dim the cover.

Tips:

- End `lyrics_close` in the layout's normal state. Leaving the Player jumps
  straight to its end.
- The cover image isn't resized while `cover_card` changes size; it is fitted
  again when the animation ends.
- The lyrics text itself (font and highlighting) and the lyrics pane's side
  and bottom margins are not part of the layout; the colors and alignment
  can be set with the roles above.

## Using a layout from a plugin

A plugin can ship a layout and offer it in Settings:

```lua
plugin.set_player_layout({ xml = "player_layouts/clean.xml", name = "Clean" })
```

The path is relative to the SD card's `.plugins` folder. Called from the
plugin's top-level code, this only adds the layout to **Settings > Display >
Player Layout > Layout**; the user picks it there and the choice is remembered
while the plugin stays installed. Called from a callback, it switches to the
layout right away, for this session only. The background options of
`set_player_layout` (`flat`, `bg_color`, blur) are global and apply to every
layout, so a layout that wants its own background should paint it itself.
`plugins_examples/HiByStockPlayer.lua` is a complete example. See `plugin.set_player_layout` in
[PLUGINS.md](../PLUGINS.md) for the details. Published plugins, including ones
that ship layouts, live in the
[compas-plugins repository](https://github.com/Starnished66/compas-plugins).

## Building a layout into the firmware

Developers can compile a layout into the app instead of shipping an XML file.
LVGL's UI Editor (LVGL Pro) can export an XML component as C, which gives a
function like `lv_obj_t * my_player_create(lv_obj_t * parent);`.

1. In the editor, name the widgets as above, then export the component.
2. Copy the generated `.c` and `.h` into `src/ui/` and add the `.c` to
   `APP_SRCS` in the `Makefile`. The code must build against LVGL 9.5 in
   `lvgl/`. Fonts and icons are app objects there: use the C names, for
   example `app_font_player_title` and `asset_path("playing_plane/btn_play.png")`
   (`src/ui/player_layouts.c` lists them all).
3. Register the layout once, early in `gui_init()` and before
   `gui_player_init()`:

   ```c
   #include "player_layouts.h"
   #include "my_player.h"

   player_layouts_register_c("my_player", "My player", my_player_create);
   ```

   The first argument is the id saved in the settings, the second the name
   shown in Settings. The create function must return the root object it made,
   or NULL on failure.

4. To use timelines from exported code, register a function that returns
   them by name. It is called once per name when the screen is built, and each
   timeline must belong to `root`:

   ```c
   static lv_anim_timeline_t * my_player_timeline(lv_obj_t * root, const char * name) {
       if (strcmp(name, "track_change") == 0) return my_player_get_track_change(root);
       return NULL;
   }

   player_layouts_register_c_ex("my_player", "My player", my_player_create, my_player_timeline);
   ```

## If something goes wrong

- **The default layout shows instead of mine.** The file failed to load, or a
  required widget is missing or has the wrong type. The reason is printed to
  the app's error output (stderr). On the simulator, run it from a terminal
  to see it.
- **My layout isn't in the list.** Check the folder, the `.xml` extension, and
  that the name only uses letters, digits, `_`, `-` and `.`. Files with `@` in
  the name are screen-size versions and are not listed.
- **Part of the screen is cut off on one model.** Add a
  `name@WIDTHxHEIGHT.xml` version for that screen.
- **An attribute seems to do nothing.** LVGL ignores attributes it doesn't
  know, and in release builds it doesn't warn about them. Check the spelling
  against the 9.4 XML docs.

## Notes

- Only one XML layout is loaded at a time. Switching reloads the interface.
- The XML engine adds about 220 KB to the app. It only starts when an XML
  layout is used.
- The app sizes `background_img` to the full screen and places it at the
  top-left of its parent, so put it in a full-screen container at the
  top-left of the screen (as `overlay_panel` is in the example).
