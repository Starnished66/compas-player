#!/usr/bin/env python3
"""Exercise the production plugin progress card with real headless LVGL."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/ui/gui_plugins.c").read_text()
begin = source.index("/* A plugin progress overlay")
end = source.index("static void plugin_list_apply_selection", begin)
production_block = source[begin:end]

test = r'''
#include "lvgl.h"
#include "screen_builders.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

lv_style_t style_theme_card_bg, style_theme_text_muted, style_theme_text_primary;
static lv_style_t progress_accent;
lv_color_t accent_lv_color(void) { return lv_color_hex(0x44aaff); }
lv_style_t *gui_theme_accent_style(void) { return &progress_accent; }
const lv_font_t *gui_theme_font(gui_font_role_t role) { (void)role; return LV_FONT_DEFAULT; }
uint32_t gui_anim_ms(uint32_t base) { return base; }

''' + production_block + r'''

static lv_color_t pixels[480 * 800];
static int flushes;
static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area; (void)pixels; flushes++;
    lv_display_flush_ready(display);
}
static lv_display_t *make_display(int32_t width, int32_t height) {
    lv_display_t *display = lv_display_create(width, height);
    assert(display);
    lv_display_set_flush_cb(display, flush_cb);
    lv_display_set_buffers(display, pixels, NULL,
                           (uint32_t)(width * height * sizeof(lv_color_t)),
                           LV_DISPLAY_RENDER_MODE_FULL);
    return display;
}
static void render(lv_display_t *display) {
    lv_refr_now(display);
    assert(flushes > 0);
    flushes = 0;
}

int main(void) {
    lv_init();
    lv_style_init(&style_theme_card_bg);
    lv_style_set_bg_color(&style_theme_card_bg, lv_color_hex(0x20242a));
    lv_style_set_bg_opa(&style_theme_card_bg, LV_OPA_COVER);
    lv_style_init(&style_theme_text_muted);
    lv_style_set_text_color(&style_theme_text_muted, lv_color_hex(0xb0b0b0));
    lv_style_init(&style_theme_text_primary);
    lv_style_set_text_color(&style_theme_text_primary, lv_color_white());
    lv_style_init(&progress_accent);
    lv_style_set_bg_color(&progress_accent, lv_color_hex(0x44aaff));
    lv_style_set_bg_opa(&progress_accent, LV_OPA_COVER);

    /* Boot-time call before display setup must fail without creating state. */
    assert(!gui_plugin_show_progress("boot", "not ready", -1));
    assert(!gui_plugin_progress_is_visible());

    const int sizes[][2] = {{480, 800}, {240, 320}};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        lv_display_t *display = make_display(sizes[s][0], sizes[s][1]);
        assert(gui_plugin_show_progress("Progress API Validation - a deliberately long plugin task title", "Resolving metadata", 0));
        assert(gui_plugin_progress_is_visible());
        lv_obj_update_layout(plugin_progress_card);
        assert(lv_obj_get_height(plugin_progress_title) <=
               gui_theme_font(GUI_FONT_ROLE_TITLE)->line_height + BOARD_SCALE_PX(2));
        assert(lv_obj_get_width(plugin_progress_card) <= sizes[s][0]);
        assert(lv_obj_get_x(plugin_progress_card) >= 0);
        assert(lv_obj_get_x(plugin_progress_card) + lv_obj_get_width(plugin_progress_card) <= sizes[s][0]);
        assert(lv_bar_get_value(plugin_progress_bar) == 0);
        assert(strcmp(lv_label_get_text(plugin_progress_percent), "0%") == 0);
        assert(lv_obj_get_style_bg_opa(plugin_progress_bar, LV_PART_MAIN) == LV_OPA_20);
        assert(lv_color_eq(lv_obj_get_style_bg_color(plugin_progress_bar, LV_PART_MAIN),
                           lv_color_hex(0xb0b0b0)));
        render(display);

        assert(gui_plugin_update_progress("Receiving data", 43));
        assert(lv_bar_get_value(plugin_progress_bar) == 43);
        assert(strcmp(lv_label_get_text(plugin_progress_percent), "43%") == 0);
        assert(gui_plugin_update_progress("Receiving data", -1));
        assert(lv_bar_get_mode(plugin_progress_bar) == LV_BAR_MODE_RANGE);
        int32_t initial_start = lv_bar_get_start_value(plugin_progress_bar);
        lv_tick_inc(450); lv_timer_handler();
        int32_t advanced_start = lv_bar_get_start_value(plugin_progress_bar);
        assert(advanced_start != initial_start);
        for (int i = 0; i < 8; i++) {
            assert(gui_plugin_update_progress("Still working", -1));
            lv_tick_inc(80); lv_timer_handler();
        }
        assert(lv_bar_get_start_value(plugin_progress_bar) != advanced_start);
        assert(lv_bar_get_value(plugin_progress_bar) - lv_bar_get_start_value(plugin_progress_bar) == 20);
        render(display);

        /* Replacement deletes the old card and starts a fresh determinate state. */
        uint32_t cards_before_replace = lv_obj_get_child_count(lv_layer_top());
        assert(gui_plugin_show_progress("Replacement", "Second task", 100));
        assert(lv_obj_get_child_count(lv_layer_top()) == cards_before_replace);
        assert(lv_bar_get_mode(plugin_progress_bar) == LV_BAR_MODE_NORMAL);
        assert(lv_bar_get_value(plugin_progress_bar) == 100);
        render(display);

        lv_obj_t *header = lv_obj_get_child(plugin_progress_card, 0);
        lv_obj_t *close = lv_obj_get_child(header, 1);
        lv_obj_send_event(close, LV_EVENT_CLICKED, NULL);
        assert(!gui_plugin_progress_is_visible());
        assert(!gui_plugin_update_progress("dismissed", 50));
        assert(!gui_plugin_progress_is_visible());

        /* External card deletion also retires the animation and static refs. */
        assert(gui_plugin_show_progress("External delete", "test", -1));
        lv_obj_t *deleted_bar = plugin_progress_bar;
        assert(lv_anim_get(deleted_bar, plugin_progress_bar_anim));
        lv_obj_delete(plugin_progress_card);
        assert(!gui_plugin_progress_is_visible());
        assert(!plugin_progress_bar && !plugin_progress_message && !plugin_progress_title);
        assert(!lv_anim_get(deleted_bar, plugin_progress_bar_anim));

        assert(gui_plugin_show_progress("Close", "idempotent", -1));
        gui_plugin_close_progress();
        gui_plugin_close_progress();
        assert(!gui_plugin_progress_is_visible());

        /* Display teardown deletes its layer/card and must not leave stale handles. */
        assert(gui_plugin_show_progress("Display delete", "teardown", -1));
        lv_display_delete(display);
        assert(!gui_plugin_progress_is_visible());
        assert(!plugin_progress_bar && !plugin_progress_message);
    }
    puts("plugin progress UI lifecycle passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="compas-progress-ui-") as temp:
    temp = Path(temp)
    c_file = temp / "progress_ui_test.c"
    executable = temp / "progress_ui_test"
    c_file.write_text(test)
    lvgl_objects = sorted((ROOT / "build_host/lvgl").rglob("*.o"))
    if not lvgl_objects:
        raise SystemExit("build_host LVGL objects not found; run `make build_host/lvgl/src/lv_init.o` first")
    archive = temp / "liblvgl.a"
    subprocess.run(["ar", "rcs", str(archive), *map(str, lvgl_objects)], check=True, cwd=ROOT)
    command = [
        "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-DLV_CONF_INCLUDE_SIMPLE=1", "-DBOARD_R1",
        "-I" + str(ROOT), "-I" + str(ROOT / "src/ui"), "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/core"),
        "-I" + str(ROOT / "lvgl"), str(c_file), str(archive), "-lm", "-pthread", "-o", str(executable),
    ]
    try:
        subprocess.run(command, check=True, cwd=ROOT, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        print(error.stderr)
        raise
    subprocess.run([str(executable)], check=True, cwd=ROOT)
