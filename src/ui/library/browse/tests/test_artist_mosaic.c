/* Exercise the shared artist page with real LVGL widgets and synthetic cached art. */
#include "../gui_artist_page.c"
#include <assert.h>

lv_style_t style_theme_card_bg, style_theme_text_primary, style_theme_text_muted;
lv_style_t pill_row_bg_style, list_row_pressed_style;
static lv_style_t accent;
lv_font_t lv_font_montserrat_16;
uint32_t gui_anim_ms(uint32_t ms) { return ms; }
int32_t ui_list_row_width(void) { return 460; }
static bool glyph(const lv_font_t * font, lv_font_glyph_dsc_t * dsc, uint32_t letter, uint32_t next) {
    (void) font; (void) letter; (void) next;
    memset(dsc, 0, sizeof(*dsc)); dsc->adv_w = 8; return true;
}
static gui_artist_summary_t facts;
static gui_artist_revisions_t revision;
static uint32_t commits;
player_settings_t current_settings;
static bool albums_ready, photo_ready, photo_missing, album_missing;
static const char * capture_dir;
static int photo_copies, photo_requests;
static int requests;

const char * i18n_tr(const char * text) { return text; }
const char * i18n_tr_n(const char * one, const char * many, long n) { return n == 1 ? one : many; }
const lv_font_t * gui_theme_font(gui_font_role_t role) { (void) role; return LV_FONT_DEFAULT; }
lv_style_t * gui_theme_accent_style(void) { return &accent; }
const char * asset_path(const char * path) { (void) path; return "S:/missing-placeholder.png"; }
void ui_clear_flag_on_press_cb(lv_event_t * event) { (void) event; }
void compact_list_set_header(lv_obj_t * list, lv_obj_t * header, int32_t height) {
    (void) list; (void) header; (void) height;
}
lv_obj_t * gui_play_buttons_build(lv_obj_t * parent, int32_t width, lv_event_cb_t play, void * pd,
                                 lv_event_cb_t shuffle, void * sd) {
    (void) width; (void) play; (void) pd; (void) shuffle; (void) sd;
    return lv_obj_create(parent);
}
bool gui_artist_data_snapshot(gui_artist_summary_t * summary, gui_artist_revisions_t * rev) {
    *summary = facts; *rev = revision; return facts.ready;
}
uint32_t library_thumbnails_commit_count(void) { return commits; }
bool library_thumbnails_copy_album(uint64_t key, uint16_t * pixels, bool * no_art) {
    *no_art = album_missing;
    if (album_missing) return false;
    if (!albums_ready) return false;
    for (size_t i = 0; i < TILE_BYTES / 2; i++) pixels[i] = (uint16_t) key;
    return true;
}
void library_thumbnails_request_album(lv_obj_t * list, int64_t song, uint64_t key) {
    (void) list; (void) song; (void) key; requests++;
}

bool library_thumbnails_copy_artist_photo(uint64_t key, uint16_t * pixels, bool * no_art) {
    assert(key == 0x9999);
    photo_copies++;
    *no_art = photo_missing;
    if (!photo_ready) return false;
    for (size_t i = 0; i < TILE_BYTES / 2; i++) pixels[i] = 0xf800;
    return true;
}
void library_thumbnails_request_artist_photo(lv_obj_t * list, int64_t song, uint64_t key,
                                             metadata_db_group_kind_t kind, const char * name) {
    (void) list; (void) kind;
    assert(song == 42 && key == 0x9999 && !strcmp(name, "Mosaic artist"));
    photo_requests++;
}

static void set_albums(int count) {
    facts = (gui_artist_summary_t) { .ready = true, .mosaic_count = count };
    for (int i = 0; i < count; i++) {
        static const uint16_t colors[4] = { 0x07e0, 0x001f, 0xffe0, 0xf81f };
        facts.mosaic_key[i] = colors[i];
        facts.mosaic_song_id[i] = 20 + i;
    }
    revision.summary++;
}

static void check_geometry(page_t * p, int count) {
    assert(p->tile_count == (count <= 0 ? 0 : count == 1 ? 1 : 4));
    lv_obj_update_layout(p->root);
    int half = p->cover_px / 2;
    for (int i = 0; i < TILES; i++) {
        assert(lv_obj_has_flag(p->tile_img[i], LV_OBJ_FLAG_HIDDEN) == (i >= p->tile_count));
        if (i >= p->tile_count) continue;
        int expected = p->tile_count == 1 ? p->cover_px : half;
        assert(lv_obj_get_width(p->tile_img[i]) == expected);
        assert(lv_obj_get_height(p->tile_img[i]) == expected);
        assert(lv_obj_get_x(p->tile_img[i]) == (p->tile_count == 1 ? 0 : (i % 2) * half));
        assert(lv_obj_get_y(p->tile_img[i]) == (p->tile_count == 1 ? 0 : (i / 2) * half));
    }
}

static void check_plain_mosaics(lv_obj_t * list) {
    static const int mapping[5][4] = { {0}, {0}, {0,1,1,0}, {0,1,2,0}, {0,1,2,3} };
    gui_artist_page_params_t params = { .name = "Mosaic artist" };
    for (int n = 0; n <= 4; n++) {
        set_albums(n);
        gui_artist_page_build(list, &params);
        page_t * p = page_for(list, false);
        check_geometry(p, n);
        gui_artist_page_poll(list, true);
        for (int i = 0; i < p->tile_count; i++) {
            assert(p->tile_album[i] == mapping[n][i]);
            assert(p->tile_state[i] == 1 && p->tile_shown[i] == 2);
            assert(p->tile_pixels[i][0] == facts.mosaic_key[mapping[n][i]]);
        }
        int height = p->header_height;
        const void * sources[4];
        for (int i = 0; i < p->tile_count; i++) sources[i] = lv_image_get_src(p->tile_img[i]);
        revision.duration++;
        gui_artist_page_poll(list, true);
        assert(p->header_height == height);
        for (int i = 0; i < p->tile_count; i++) assert(lv_image_get_src(p->tile_img[i]) == sources[i]);
    }
}

static void capture(page_t * p, const char * name) {
    if (!capture_dir) return;
    lv_draw_buf_t * image = lv_snapshot_take(p->box, LV_COLOR_FORMAT_RGB565);
    assert(image);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", capture_dir, name);
    FILE * out = fopen(path, "wb");
    assert(out);
    fprintf(out, "P6\n%u %u\n255\n", image->header.w, image->header.h);
    for (unsigned y = 0; y < image->header.h; y++) {
        const uint16_t * row = (const uint16_t *) (image->data + y * image->header.stride);
        for (unsigned x = 0; x < image->header.w; x++) {
            uint16_t v = row[x];
            unsigned char rgb[3] = { ((v >> 11) & 31) * 255 / 31,
                                     ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31 };
            assert(fwrite(rgb, sizeof(rgb), 1, out) == 1);
        }
    }
    fclose(out);
    lv_draw_buf_destroy(image);
}

static void check_photo_mosaics(lv_obj_t * list) {
    gui_artist_page_params_t params = { .name = "Mosaic artist" };
    for (int n = 0; n <= 4; n++) {
        set_albums(n);
        facts.artist_key = 0x9999;
        facts.artist_song_id = 42;
        current_settings.show_artist_images = true;
        photo_ready = photo_missing = false;
        gui_artist_page_build(list, &params);
        gui_artist_page_poll(list, true);
        page_t * p = page_for(list, false);
        check_geometry(p, n);
        int height = p->header_height;
        if (n == 4) capture(p, "before-synthetic-mosaic");
        for (int i = 0; i < p->tile_count; i++) assert(p->tile_shown[i] == 2);
        /* The artist cache finishes after album covers are already visible. */
        photo_ready = true;
        commits++;
        gui_artist_page_poll(list, true);
        int m = n ? n + 1 : 0; /* the photo is one more picture */
        check_geometry(p, m);
        assert(p->header_height == height);
        if (n == 4) capture(p, "after-synthetic-mosaic");
        static const int pictures[5][4] = { { 0 }, { 0 }, { 0, 1, 1, 0 }, { 0, 1, 2, 0 }, { 0, 1, 2, 3 } };
        for (int i = 0; i < p->tile_count; i++) {
            int album = pictures[m < 4 ? m : 4][i] - 1;
            assert(p->tile_album[i] == album);
            assert(p->tile_shown[i] == 2 && p->tile_state[i] == 1);
            assert(p->tile_pixels[i][0] == (album < 0 ? 0xf800 : facts.mosaic_key[album]));
        }
        /* Disabling images restores the existing album map with stable geometry. */
        current_settings.show_artist_images = false;
        gui_artist_page_poll(list, true);
        check_geometry(p, n);
        assert(p->header_height == height);
        for (int i = 0; i < p->tile_count; i++) assert(p->tile_album[i] >= 0 && p->tile_shown[i] == 2);
        int copies = photo_copies;
        photo_ready = false;
        gui_artist_page_build(list, &params);
        gui_artist_page_poll(list, true);
        assert(photo_copies == copies);
        /* A confirmed missing artist image never replaces the album mosaic. */
        current_settings.show_artist_images = true;
        photo_missing = true;
        commits++;
        gui_artist_page_poll(list, true);
        for (int i = 0; i < p->tile_count; i++) assert(p->tile_album[i] >= 0 && p->tile_shown[i] == 2);
    }
    assert(photo_requests > 0);
}

static bool external_photo(void * source, uint16_t * pixels, bool * no_art) {
    assert(source == &facts);
    return library_thumbnails_copy_artist_photo(0x9999, pixels, no_art);
}

static void check_external_and_pending(lv_obj_t * list) {
    set_albums(4);
    gui_artist_page_params_t params = { .name = "Mosaic artist", .source = &facts,
                                       .copy_artist = external_photo };
    photo_ready = photo_missing = false;
    current_settings.show_artist_images = true;
    gui_artist_page_build(list, &params);
    gui_artist_page_poll(list, true);
    page_t * p = page_for(list, false);
    uint16_t old[4];
    for (int i = 0; i < 4; i++) old[i] = p->tile_pixels[i][0];
    albums_ready = false;
    photo_ready = true;
    commits++;
    gui_artist_page_poll(list, true);
    assert(p->tile_pixels[0][0] == 0xf800);
    /* Shifted album tiles keep their previous pixels while cache data is pending. */
    for (int i = 1; i < 4; i++) {
        assert(p->tile_state[i] == 0 && p->tile_shown[i] == 2);
        assert(p->tile_pixels[i][0] == old[i]);
    }
    revision.duration++;
    gui_artist_page_poll(list, true);
    for (int i = 1; i < 4; i++) assert(p->tile_shown[i] == 2);
    albums_ready = true;
    commits++;
    gui_artist_page_poll(list, true);
    for (int i = 1; i < 4; i++) assert(p->tile_pixels[i][0] == facts.mosaic_key[i - 1]);
    /* Turning images off hides the photo even while its replacement is pending. */
    current_settings.show_artist_images = false;
    albums_ready = false;
    gui_artist_page_poll(list, true);
    assert(p->tile_state[0] == 0 && p->tile_shown[0] == 1);
    assert(lv_image_get_src(p->tile_img[0]) != &p->tile_dsc[0]);
    albums_ready = true;
    commits++;
    gui_artist_page_poll(list, true);
    assert(p->tile_state[0] == 1 && p->tile_pixels[0][0] == facts.mosaic_key[0]);
    current_settings.show_artist_images = true;
    gui_artist_page_poll(list, true);
    assert(p->tile_pixels[0][0] == 0xf800 && p->tile_shown[0] == 2);
    /* A terminal missing album must also clear the disabled artist photo. */
    current_settings.show_artist_images = false;
    album_missing = true;
    gui_artist_page_poll(list, true);
    assert(p->tile_state[0] == 2 && p->tile_shown[0] == 1);
    album_missing = false;
}

static void check_zero_key_remap(lv_obj_t * list) {
    set_albums(4);
    facts.mosaic_key[0] = 0; /* Subsonic album without coverArt. */
    gui_artist_page_params_t params = { .name = "Mosaic artist", .source = &facts,
                                       .copy_artist = external_photo };
    photo_ready = photo_missing = false;
    current_settings.show_artist_images = true;
    gui_artist_page_build(list, &params);
    gui_artist_page_poll(list, true);
    page_t * p = page_for(list, false);
    assert(p->tile_shown[0] == 1 && p->tile_shown[1] == 2);
    photo_ready = true;
    commits++;
    gui_artist_page_poll(list, true);
    assert(p->tile_pixels[0][0] == 0xf800 && p->tile_shown[0] == 2);
    assert(p->tile_album[1] == 0 && p->tile_key[1] == 0);
    assert(p->tile_state[1] == 2 && p->tile_shown[1] == 1);
    for (int i = 2; i < 4; i++) {
        assert(p->tile_shown[i] == 2);
        assert(p->tile_pixels[i][0] == facts.mosaic_key[i - 1]);
    }
}

static void check_request_cadence(lv_obj_t * list) {
    set_albums(4);
    facts.artist_key = 0x9999;
    facts.artist_song_id = 42;
    gui_artist_page_params_t params = { .name = "Mosaic artist" };
    albums_ready = photo_ready = photo_missing = false;
    current_settings.show_artist_images = true;
    gui_artist_page_build(list, &params);
    lv_tick_inc(400);
    gui_artist_page_poll(list, true);
    int requested = requests;
    assert(requested >= 4);
    lv_tick_inc(199);
    gui_artist_page_poll(list, true);
    assert(requests == requested);
    lv_tick_inc(201);
    gui_artist_page_poll(list, true);
    assert(requests == requested + 4);
    albums_ready = true;
}

int main(int argc, char ** argv) {
    capture_dir = argc > 1 ? argv[1] : NULL;
    lv_font_montserrat_16.line_height = 16;
    lv_font_montserrat_16.get_glyph_dsc = glyph;
    lv_init();
    lv_display_t * display = lv_display_create(480, 800);
    lv_obj_t * list = lv_obj_create(lv_screen_active());
    lv_obj_set_size(list, 480, 800);
    albums_ready = true;
    check_plain_mosaics(list);
    check_photo_mosaics(list);
    check_external_and_pending(list);
    check_zero_key_remap(list);
    check_request_cadence(list);
    gui_artist_page_release(list);
    gui_artist_page_forget(list);
    lv_obj_delete(list);
    lv_display_delete(display);
    lv_deinit();
    puts("artist_mosaic: PASS");
    return 0;
}
