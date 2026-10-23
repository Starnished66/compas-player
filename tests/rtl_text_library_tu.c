/* Test-only wrapper compiled as the production library translation unit so
 * its private source-selection callback can be exercised without audio. */
#include "../src/ui/gui_library.c"

#include <assert.h>

int rtl_text_test_set_album_source(const char * raw_title) {
    assert(raw_title);
    lv_obj_t * title_screen = lv_obj_create(NULL);
    assert(title_screen);
    group_songs_title_label = lv_label_create(title_screen);
    assert(group_songs_title_label);
    lv_label_set_text(group_songs_title_label, raw_title);
    assert(strcmp(lv_label_get_text(group_songs_title_label), raw_title) != 0);

    group_song_entry_t entry = {
        .path = "./fixture/rtl-source-track.flac",
        .title = "Arabic source identity fixture",
    };
    group_songs_entries = calloc(1, sizeof(*group_songs_entries));
    assert(group_songs_entries);
    group_songs_entries[0].path = strdup(entry.path);
    group_songs_entries[0].title = strdup(entry.title);
    assert(group_songs_entries[0].path && group_songs_entries[0].title);
    group_songs_count = 1;
    group_songs_source_is_album = true;
    group_songs_source_title = strdup(raw_title);
    assert(group_songs_source_title);

    set_player_source_group_songs(0);
    return strcmp(current_settings.last_source_name, raw_title) == 0;
}
