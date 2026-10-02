# Vendored LVGL XML engine

LVGL 9.5 removed its XML engine (`src/others/xml`) and the bundled expat
parser (`src/libs/expat`), see lvgl/lvgl PR 9565. This directory carries the
v9.4.0 versions so the Player can load layouts from XML files at runtime
(`docs/PLAYER_LAYOUTS.md`).

## Origin

- Project: LVGL, https://github.com/lvgl/lvgl
- Tag: `v9.4.0`, commit `c016f72d4c125098287be5e83c0f1abed4706ee5`
- Copied unchanged except for the local modifications below:
  - `src/others/xml/**` (MIT, see LVGL's `LICENCE.txt`)
  - `src/libs/expat/**` (expat 2.6.3, MIT, `src/libs/expat/LICENSE.txt` kept)

## Files that are not upstream

These adapt the 9.4 sources to the 9.5 headers in `lvgl/`. Everything else is
upstream, so a re-sync is a straight copy plus the patches listed below.

- `src/lvgl.h`, `src/lvgl_private.h`: forward to the real umbrella headers and
  add the XML headers that the 9.4 umbrella headers used to include.
- `src/misc/lv_types.h`: forwards to the real header and adds the three opaque
  typedefs (`lv_xml_component_scope_t`, `lv_xml_parser_state_t`,
  `lv_xml_load_t`) that 9.4 declared there.
- `src/others/observer/lv_observer.h`: 9.5 moved the observer to `src/core/`.
- `src/others/xml/lv_xml_compat.h`, `lv_xml_compat.c`: the engine state that 9.4
  kept in `lv_global.h` (`xml_path_prefix`, the timeline event id).

Headers that are not shimmed (`../../misc/lv_style.h` and so on) resolve to the
real 9.5 headers through `-I lvgl/src/others/translation`, see `LV_XML_CFLAGS`
in the Makefile. That directory is two levels below `lvgl/src`, so the 9.4
relative include paths land on the right files without editing upstream.

## Local modifications to upstream files

- `lv_xml.c`, `lv_xml_component.c`, `parsers/lv_xml_obj_parser.c`: the
  `LV_GLOBAL_DEFAULT()->xml_*` macros now read `lv_xml_compat_globals`.
- `lv_xml_load.c`: `xml_loads` is a file-static list.
- `lv_xml_component.c`: the body of `lv_xml_component_unregister()` that frees
  a scope's lists moved to `scope_free_contents()`, which a failed
  `lv_xml_register_component_from_data()` now also calls (upstream leaked
  everything the metadata pass had created).
- `lv_xml.c`: `lv_xml_create_in_scope()` frees its parent list when the view
  fails to parse.

## Build notes

- `lv_conf.h` sets `LV_USE_XML 1` and `LV_USE_OBJ_NAME 1`. `LV_USE_TRANSLATION`
  stays off, so `lv_xml_translation.c` compiles to nothing.
- `lv_xml_test.c` (needs `LV_USE_TEST`) is left out of the copy. The expat
  `xmltok_impl.c` / `xmltok_ns.c` are textually included by `xmltok.c`, so the
  Makefile does not compile them on their own.
- `lv_init()` does not call `lv_xml_init()` in 9.5. `src/ui/player_layouts.c`
  calls it once, when the first XML layout is created (users of the built-in
  layout never pay for the engine's startup allocations). `lv_xml_deinit()` is
  never called: LVGL itself stays up across soft UI reloads.
- Compiled with `-Os`: the expat tokenizer alone is about 70 KB at `-O3`.
- Timeline properties include numeric `text_align` so layouts can switch
  metadata alignment between playback and lyrics views.
- The image parser recognizes 9.5 `contain` and `cover` alignment so stock
  assets retain their aspect ratio in XML layouts. The other 9.4 widget parsers
  built against 9.5 unchanged; none were dropped.
