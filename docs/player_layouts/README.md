# Exporting a Player layout from LVGL's UI Editor

LVGL's UI Editor (LVGL Pro) can turn an XML component into C. The app can use
that C directly, so a layout drawn in the editor becomes a normal entry in
Settings > Display > Player Layout > Layout, with no XML file on the device.
The widget-name contract is in `docs/PLAYER_LAYOUTS.md`.

Nothing in this folder is built. The snippets below show what to add.

## 1. Design the component

Create one component whose `<view>` is the layout, and give the widgets the
role names from `docs/PLAYER_LAYOUTS.md` (`cover_card`, `cover_img`, `title`,
`play_btn`, `progress_slider` are required).
`assets/theme2/player_layouts/example_minimal.xml` is a component to start from.

The fonts and images the contract lists by name exist as real objects only
inside the app, so the exported C has to reference them as C symbols: see
`player_layouts.c` for the list (`app_font_player_title`, `app_font_16`, the
`asset_path()` image paths).

Timelines named `lyrics_open`, `lyrics_close`, `track_change` and
`screen_enter` are optional.

## 2. Export

Export the component to C. For a component named `my_player` the editor writes
a function of this shape:

```c
lv_obj_t * my_player_create(lv_obj_t * parent);
```

Copy the generated `.c` and `.h` into `src/ui/` and add the `.c` to `APP_SRCS`
in the `Makefile`. The generated code has to build against the LVGL in `lvgl/`
(9.5).

## 3. Register it

Once, before the first Player screen is built:

```c
#include "player_layouts.h"
#include "my_player.h"

void my_layouts_register(void) {
    player_layouts_register_c("my_player", "My player", my_player_create);
}
```

Call `my_layouts_register()` early in `gui_init()`, before `gui_player_init()`.
The id (`"my_player"`) is what `settings.txt` stores; the second argument is
the name shown in the list. Ids use letters, digits, `_`, `-` and `.`, and
`default` is taken.

The app builds the layout by calling `my_player_create(screen)` on an empty
screen, then looks up the roles by name. `create` must return the root object
it made, or NULL on failure (the app then falls back to the built-in layout).

## Timelines from exported code

An XML layout's timelines are found automatically. For exported code, how a
timeline is stored depends on the exporter, so the app asks you: register with
a lookup function.

```c
static lv_anim_timeline_t * my_player_timeline(lv_obj_t * root, const char * name) {
    if (strcmp(name, "track_change") == 0) return my_player_get_track_change(root);
    return NULL;
}

player_layouts_register_c_ex("my_player", "My player", my_player_create, my_player_timeline);
```

`root` is what `my_player_create()` returned. The function is called once per
name when the screen is built. The timeline must belong to `root`, so that it
is freed when the screen is.
