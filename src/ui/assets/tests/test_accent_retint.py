#!/usr/bin/env python3
"""Compare production retint loops with the pre-retint implementation using real PNGs and padded rows.

Run: python3 src/ui/assets/tests/test_accent_retint.py [theme-parent-directory]
Requires Pillow and a host C compiler. No external source is built.
"""
import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from PIL import Image

ROOT = Path(__file__).resolve().parents[4]


def source(path, head=False):
    if head:
        return subprocess.check_output(['git', 'show', '73e9d7bd7^:' + path], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def function(text, name):
    start = re.search(r'^(?:static )?(?:bool|void|const void \*) ' + name + r'\(', text, re.M).start()
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


assets = source('src/ui/assets/assets.c')
drawer = source('src/ui/shell/gui_shell_drawer.c')
transport = source('src/ui/player/transport/player_transport_images.c')
header = source('src/ui/assets/assets.h')
types = header[header.index('typedef struct asset_tint_rgb'):header.index('bool asset_decoded_image_open')]
code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
typedef struct { uint8_t blue, green, red, alpha; } lv_color32_t;
typedef struct { uint8_t blue, green, red; } lv_color_t;
typedef struct { uint32_t w, h, stride, cf; } header_t;
typedef struct { header_t header; uint8_t *data; } lv_draw_buf_t;
typedef struct { void *decoded; } lv_image_decoder_dsc_t;
typedef struct { lv_color_t color; } lv_style_value_t;
#define LV_COLOR_FORMAT_ARGB8888 1
#define GUI_BREEZE_TEXT_ON_LIGHT 0x123456
#define LV_STYLE_TEXT_COLOR 1
#define LV_STYLE_RES_FOUND 1
static bool play_btn_glyph_on_light;
static lv_color_t lv_color_hex(uint32_t x) {
    return (lv_color_t) {x & 255, (x >> 8) & 255, x >> 16};
}
static void *gui_player_backdrop_text_style(bool x) { (void)x; return NULL; }
static int lv_style_get_prop(void *a, int b, lv_style_value_t *v) {
    (void)a; (void)b; v->color = lv_color_hex(0x123456); return LV_STYLE_RES_FOUND;
}
static const uint8_t *original;
static header_t original_header;
static unsigned decodes, drops;
static const char *asset_path(const char *p) { return p; }
static void lv_image_cache_drop(const void *p) { (void)p; drops++; }
static void lv_image_decoder_close(lv_image_decoder_dsc_t *d) {
    lv_draw_buf_t *b = d->decoded;
    if (b) { free(b->data); free(b); }
}
'''+types
code += function(assets, 'asset_decoded_image_close')
code += r'''
static bool asset_decoded_image_open(asset_decoded_image_t *im, const char *path) {
    asset_decoded_image_close(im);
    lv_draw_buf_t *b = calloc(1, sizeof(*b)); assert(b);
    b->header = original_header;
    size_t n = b->header.stride * b->header.h;
    b->data = malloc(n); assert(b->data); memcpy(b->data, original, n);
    im->decoder.decoded = b; im->path = strdup(path); im->open = true;
    decodes++; return true;
}
'''
code += function(assets, 'asset_decoded_image_prepare_retint')
for name in ['toggle_corners_opaque', 'mask_toggle_circle', 'recolor_toggle_on_accent',
             'prepare_toggle_on_image', 'load_toggle_off_image']:
    code += function(drawer, name)
code += function(transport, 'recolor_play_btn_accent')
for path, name in [('src/ui/shell/gui_shell_drawer.c', 'recolor_toggle_on_accent'),
                   ('src/ui/player/transport/player_transport_images.c', 'recolor_play_btn_accent')]:
    code += function(source(path, True), name).replace(name, 'head_' + name)
code += r'''
int check(const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t stride, bool play) {
    original = pixels; original_header = (header_t) {w,h,stride,1};
    decodes = drops = 0;
    asset_decoded_image_t im = {0};
    size_t n = h * stride;
    uint8_t *expected = malloc(n); assert(expected);
    lv_draw_buf_t ref = {original_header, expected};
    const uint32_t accents[] = {0, 0xffffff, 0xff00ff, 0x009ff6, 0x123456, 0xffff00};
    for (unsigned i = 0; i < 60; i++) {
        lv_color_t accent = lv_color_hex(accents[i % 6]);
        play_btn_glyph_on_light = (i % 4 == 1 || i % 4 == 2);
        memcpy(expected, pixels, n);
        if (play) {
            assert(asset_decoded_image_prepare_retint(&im, "play", true, NULL));
            recolor_play_btn_accent(&im, accent);
            head_recolor_play_btn_accent(&ref, accent);
        } else {
            prepare_toggle_on_image(&im, "toggle", accent);
            mask_toggle_circle(&ref);
            head_recolor_toggle_on_accent(&ref, accent);
        }
        assert(im.tint_keys);
        lv_draw_buf_t *b = im.decoder.decoded;
        if (memcmp(expected, b->data, n)) { free(expected); asset_decoded_image_close(&im); return i+1; }
        assert(decodes == 1);
        assert(drops == i);
    }
    size_t extras = im.tint_rgb_count;
    assert(asset_decoded_image_prepare_retint(&im, "changed", play, NULL));
    assert(decodes == 2);
    asset_decoded_image_close(&im);
    assert(!im.tint_keys && !im.tint_rgb && !im.open);
    const char *loaded = NULL;
    decodes = 0;
    load_toggle_off_image(&im, "off", &loaded);
    load_toggle_off_image(&im, "off", &loaded);
    assert(decodes == 1);
    load_toggle_off_image(&im, "plugin-new", &loaded);
    assert(decodes == 2);
    asset_decoded_image_close(&im); loaded = NULL;
    load_toggle_off_image(&im, "plugin-new", &loaded);
    assert(decodes == 3);
    asset_decoded_image_close(&im);
    free(expected);
    return -(int)extras;
}
'''

code += r'''
static unsigned sync_calls, eq_calls, library_calls;
static void gui_accent_sync(void) { sync_calls++; }
static void gui_equalizer_accent_changed(void) { eq_calls++; }
static void gui_library_accent_changed(void) { library_calls++; }
'''
code += function(source('src/ui/settings/gui_settings.c'), 'gui_settings_accent_changed')
code += r'''
void check_settings(void) {
    gui_settings_accent_changed(false);
    assert(sync_calls == 1 && eq_calls == 0 && library_calls == 0);
    gui_settings_accent_changed(true);
    assert(sync_calls == 2 && eq_calls == 1 && library_calls == 1);
}
'''

parent = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'assets'
paths = []
for theme in ['theme1', 'theme2']:
    base = parent / theme
    if not base.is_dir():
        continue
    paths += [(p, False) for p in sorted((base / 'pull_down').glob('*_s.png'))]
    paths += [(base / 'playing_plane' / name, True) for name in ['btn_play.png', 'btn_pause.png']
              if (base / 'playing_plane' / name).is_file()]
if not paths:
    raise SystemExit('No real theme PNGs found; pass their parent directory.')
with tempfile.TemporaryDirectory(prefix='accent-test-') as tmp:
    c = Path(tmp) / 'test.c'
    so = Path(tmp) / 'test.so'
    c.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-shared', '-fPIC', str(c), '-lm', '-o', str(so)], check=True)
    lib = ctypes.CDLL(str(so))
    lib.check_settings()
    lib.check.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_bool]
    def check(im, play, name):
        im = im.convert('RGBA')
        w, h = im.size
        raw = im.tobytes('raw', 'BGRA')
        stride = 4 * w + 16
        padded = b''.join(raw[y*w*4:(y+1)*w*4] + b'\xa5'*16 for y in range(h))
        result = lib.check(padded, w, h, stride, play)
        assert result <= 0, f'{name}: mismatch at retint {result}'
        return -result
    for path, play in paths:
        extras = check(Image.open(path), play, str(path))
        print(f'PASS {path.relative_to(parent)}: 60 retints, {extras} RGB exceptions')
    # Exercise arbitrary custom near-white colors and transparent colored pixels.
    im = Image.new('RGBA', (84, 84))
    im.putdata([((240+x%16), (240+y%16), (240+(x+y)%16), (x*13+y*7)%256)
                for y in range(84) for x in range(84)])
    check(im, True, 'custom near-white')
    check(im, False, 'custom square')
    print(f'PASS {len(paths)} real PNGs plus custom art; exact full-buffer comparisons, decode reuse, off paths')
