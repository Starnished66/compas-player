#!/usr/bin/env python3
"""Exercise the production Player navigation replacement helper in C stubs."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'src/ui/gui_navigation.c').read_text()


def function(name):
    match = re.search(r'^(?:static )?(?:int|void|bool) ' + name +
                      r'\([^;]*?\) \{.*?^\}', src, re.M | re.S)
    if not match:
        raise RuntimeError('missing production function ' + name)
    return match.group()


production = '\n'.join(function(name) for name in (
    'player_transition_discard_cache', 'back_target_cache_discard',
    'back_target_cache_note_replaced', 'gui_navigation_replace_player_screen'))

body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define NAV_STACK_MAX 16
typedef struct lv_obj_t { int id; } lv_obj_t;
typedef struct lv_draw_buf_t { int id; } lv_draw_buf_t;
typedef void (*lv_async_cb_t)(void *);
static lv_obj_t *nav_stack[NAV_STACK_MAX];
static int nav_depth;
static lv_obj_t *active_screen;
static lv_obj_t *player_screen;
static lv_obj_t *back_target_cache_screen;
static lv_draw_buf_t *player_transition_cache_buf;
static lv_draw_buf_t *back_target_cache_buf;
static bool player_transition_cache_dirty;
static int player_async_count;
static int back_async_count;
static lv_obj_t *loaded_screen;
static lv_obj_t *player_async_target;

static lv_obj_t *gui_player_get_screen(void) { return player_screen; }
static void lv_screen_load(lv_obj_t *screen) { loaded_screen = screen; active_screen = screen; }
static void lv_draw_buf_destroy(lv_draw_buf_t *buf) { (void)buf; }
static void player_transition_cache_async_cb(void *unused) { (void)unused; }
static void back_target_cache_rebuild_cb(void *unused) { (void)unused; }
static void lv_async_call(lv_async_cb_t callback, void *data) {
    (void)data;
    if (callback == player_transition_cache_async_cb) {
        player_async_target = gui_player_get_screen();
        player_async_count++;
    } else if (callback == back_target_cache_rebuild_cb) {
        back_async_count++;
    } else {
        assert(!"unexpected async callback");
    }
}

''' + production + r'''

static void reset(void) {
    for (int i = 0; i < NAV_STACK_MAX; i++) nav_stack[i] = NULL;
    nav_depth = 0;
    active_screen = player_screen = back_target_cache_screen = NULL;
    player_transition_cache_buf = back_target_cache_buf = NULL;
    player_transition_cache_dirty = false;
    player_async_count = back_async_count = 0;
    loaded_screen = NULL;
    player_async_target = NULL;
}

int main(void) {
    lv_obj_t home = {1}, old_player = {2}, rebuilt_player = {3};
    lv_obj_t list = {4}, overlay = {5}, frozen = {6}, other_back = {7};
    lv_draw_buf_t player_buf = {1}, back_buf = {2};

    /* A covered Player can occur multiple times in the existing path. Replace
     * every occurrence while preserving history and the visible screen. */
    reset();
    nav_stack[0] = &home; nav_stack[1] = &old_player;
    nav_stack[2] = &list; nav_stack[3] = &old_player; nav_stack[4] = &overlay;
    nav_depth = 5;
    active_screen = &overlay;
    player_screen = &rebuilt_player;
    player_transition_cache_buf = &player_buf;
    back_target_cache_screen = &old_player;
    back_target_cache_buf = &back_buf;
    gui_navigation_replace_player_screen(&old_player, &rebuilt_player, false);
    assert(nav_stack[0] == &home && nav_stack[1] == &rebuilt_player);
    assert(nav_stack[2] == &list && nav_stack[3] == &rebuilt_player);
    assert(nav_stack[4] == &overlay && nav_depth == 5);
    assert(active_screen == &overlay && loaded_screen == NULL);
    assert(back_target_cache_screen == &rebuilt_player && back_target_cache_buf == NULL);
    assert(player_transition_cache_buf == NULL && player_transition_cache_dirty);
    assert(player_async_count == 1 && player_async_target == &rebuilt_player);
    assert(back_async_count == 1);

    /* An active Player rebuild hands off from a temporary frozen frame. */
    reset();
    nav_stack[0] = &home; nav_stack[1] = &old_player; nav_depth = 2;
    active_screen = &frozen;
    player_screen = &rebuilt_player;
    gui_navigation_replace_player_screen(&old_player, &rebuilt_player, true);
    assert(nav_stack[1] == &rebuilt_player);
    assert(loaded_screen == &rebuilt_player && active_screen == &rebuilt_player);
    assert(player_async_count == 1 && player_async_target == &rebuilt_player);

    /* A different back-target belongs to another screen and stays intact. */
    reset();
    nav_stack[0] = &home; nav_stack[1] = &old_player; nav_depth = 2;
    active_screen = &list;
    player_screen = &rebuilt_player;
    back_target_cache_screen = &other_back;
    back_target_cache_buf = &back_buf;
    gui_navigation_replace_player_screen(&old_player, &rebuilt_player, false);
    assert(back_target_cache_screen == &other_back && back_target_cache_buf == &back_buf);
    assert(back_async_count == 0 && active_screen == &list && loaded_screen == NULL);

    /* Null arguments are a no-op, including cache state and visible screen. */
    reset();
    nav_stack[0] = &old_player; nav_depth = 1;
    active_screen = &list; player_screen = &rebuilt_player;
    player_transition_cache_buf = &player_buf;
    gui_navigation_replace_player_screen(NULL, &rebuilt_player, true);
    gui_navigation_replace_player_screen(&old_player, NULL, true);
    assert(nav_stack[0] == &old_player && active_screen == &list);
    assert(player_transition_cache_buf == &player_buf && !player_transition_cache_dirty);
    assert(player_async_count == 0 && back_async_count == 0 && loaded_screen == NULL);

    puts("gui navigation Player replacement regression passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='compas-player-nav-replace-') as folder:
    folder = pathlib.Path(folder)
    c_file = folder / 'test.c'
    binary = folder / 'test'
    c_file.write_text(body)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c_file), '-o', str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
