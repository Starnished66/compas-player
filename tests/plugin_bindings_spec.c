#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "app_version.h"
#include "gui.h"
#include "metadata_db.h"
#include "plugin_manager.h"

static bool fail_next_module_malloc;
static int module_malloc_count;
static int module_free_count;
static int string_array_free_count;
static int query_song_calls;
static int query_search_calls;
static int query_artists_calls;
static int query_albums_calls;
static int get_artist_album_calls;
static int get_album_tracks_calls;
static int get_next_album_tracks_calls;
static char captured_query[96];
static char captured_artist[96];
static char captured_album_artist[96];
static char captured_album[96];
static int captured_offset;
static int captured_limit;
static int64_t captured_total;
static int song_result_count;
static int group_result_count;
static bool missing_song;
static const char * artist_album_result[] = { "First Album", "Second Album" };
static const char * album_tracks_result[] = { "music/first.flac", "music/second.flac" };
static const char * next_album_tracks_result[] = { "music/next.flac" };
static song_row_t song_rows[2];
static group_row_t group_rows[2];

static void copy_or_empty(char * out, size_t size, const char * value) {
    snprintf(out, size, "%s", value ? value : "");
}

const char * app_version_label(void) { return "9.8.7"; }
const char * app_build_identifier(void) { return "test-build"; }

char ** gui_plugin_get_artist_albums(const char * artist, int * out_count) {
    ++get_artist_album_calls;
    copy_or_empty(captured_artist, sizeof(captured_artist), artist);
    *out_count = artist && strcmp(artist, "Missing") == 0 ? 0 : 2;
    if (!*out_count) return NULL;
    char ** result = calloc((size_t)*out_count, sizeof(*result));
    assert(result);
    for (int i = 0; i < *out_count; ++i) result[i] = strdup(artist_album_result[i]);
    return result;
}

char ** gui_plugin_get_album_tracks(const char * artist, const char * album, int * out_count) {
    ++get_album_tracks_calls;
    copy_or_empty(captured_artist, sizeof(captured_artist), artist);
    copy_or_empty(captured_album, sizeof(captured_album), album);
    *out_count = 2;
    char ** result = calloc((size_t)*out_count, sizeof(*result));
    assert(result);
    for (int i = 0; i < *out_count; ++i) result[i] = strdup(album_tracks_result[i]);
    return result;
}

char ** gui_plugin_get_next_album_tracks(const char * artist, const char * album, int * out_count) {
    ++get_next_album_tracks_calls;
    copy_or_empty(captured_artist, sizeof(captured_artist), artist);
    copy_or_empty(captured_album, sizeof(captured_album), album);
    *out_count = 1;
    char ** result = calloc(1, sizeof(*result));
    assert(result);
    result[0] = strdup(next_album_tracks_result[0]);
    return result;
}

void gui_plugin_free_string_array(char ** array, int count) {
    ++string_array_free_count;
    for (int i = 0; i < count; ++i) free(array[i]);
    free(array);
}

int64_t gui_plugin_library_song_count(void) { return 1234; }

int gui_plugin_library_get_songs(const char * query, const char * artist,
                                 const char * album_artist, const char * album,
                                 int offset, int limit, song_row_t * out_rows,
                                 int64_t * out_total) {
    ++query_song_calls;
    copy_or_empty(captured_query, sizeof(captured_query), query);
    copy_or_empty(captured_artist, sizeof(captured_artist), artist);
    copy_or_empty(captured_album_artist, sizeof(captured_album_artist), album_artist);
    copy_or_empty(captured_album, sizeof(captured_album), album);
    captured_offset = offset;
    captured_limit = limit;
    *out_total = captured_total;
    for (int i = 0; i < song_result_count; ++i) out_rows[i] = song_rows[i];
    return song_result_count;
}

int gui_plugin_library_search(const char * query, int limit, song_row_t * out_rows) {
    ++query_search_calls;
    copy_or_empty(captured_query, sizeof(captured_query), query);
    captured_limit = limit;
    for (int i = 0; i < song_result_count; ++i) out_rows[i] = song_rows[i];
    return song_result_count;
}

bool gui_plugin_library_get_song(int64_t id, song_row_t * out_row) {
    if (missing_song || id != song_rows[0].id) return false;
    *out_row = song_rows[0];
    return true;
}

int gui_plugin_library_get_artists(int offset, int limit, group_row_t * out_rows) {
    ++query_artists_calls;
    captured_offset = offset;
    captured_limit = limit;
    for (int i = 0; i < group_result_count; ++i) out_rows[i] = group_rows[i];
    return group_result_count;
}

int gui_plugin_library_get_albums(int offset, int limit, const char * artist_filter,
                                  group_row_t * out_rows) {
    ++query_albums_calls;
    captured_offset = offset;
    captured_limit = limit;
    copy_or_empty(captured_artist, sizeof(captured_artist), artist_filter);
    for (int i = 0; i < group_result_count; ++i) out_rows[i] = group_rows[i];
    return group_result_count;
}

void metadata_db_song_display_title(const song_row_t * row, char * out, size_t out_size) {
    const char * title = row->tags.title[0] ? row->tags.title : "Basename fallback";
    snprintf(out, out_size, "%s", title);
}

static void * spec_malloc(size_t size) {
    ++module_malloc_count;
    if (fail_next_module_malloc) {
        fail_next_module_malloc = false;
        return NULL;
    }
    return malloc(size);
}

static void spec_free(void * memory) {
    if (memory) ++module_free_count;
    free(memory);
}

#define malloc spec_malloc
#define free spec_free
#if defined(PLUGIN_BINDINGS_BASELINE)
#include "plugin_bindings_baseline_fixture.c"
#else
#include "bindings/plugin_info_bindings.c"
#include "bindings/plugin_library_bindings.c"
#endif
#undef free
#undef malloc

static void test_string_array_ownership(lua_State * L) {
    char ** values = calloc(2, sizeof(*values));
    assert(values);
    values[0] = strdup("first");
    values[1] = strdup("second");
#if defined(PLUGIN_BINDINGS_BASELINE)
    assert(push_string_array_result(L, values, 2) == 1);
#else
    assert(plugin_library_push_string_array_result(L, values, 2) == 1);
#endif
    assert(lua_istable(L, -1) && lua_rawlen(L, -1) == 2);
    lua_pop(L, 1);
    assert(string_array_free_count == 4);
}

static void run_lua(lua_State * L, const char * script) {
    int status = luaL_dostring(L, script);
    if (status != LUA_OK) fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
}

static void bind_test_api(lua_State * L) {
    lua_pushcfunction(L, l_plugin_api_version); lua_setglobal(L, "api_version");
    lua_pushcfunction(L, l_plugin_get_app_info); lua_setglobal(L, "get_app_info");
    lua_pushcfunction(L, l_plugin_has_capability); lua_setglobal(L, "has_capability");
    lua_pushcfunction(L, l_plugin_media_capabilities); lua_setglobal(L, "media_capabilities");
    lua_pushcfunction(L, l_plugin_get_artist_albums); lua_setglobal(L, "get_artist_albums");
    lua_pushcfunction(L, l_plugin_get_album_tracks); lua_setglobal(L, "get_album_tracks");
    lua_pushcfunction(L, l_plugin_get_next_album_tracks); lua_setglobal(L, "get_next_album_tracks");
    lua_pushcfunction(L, l_plugin_library_song_count); lua_setglobal(L, "library_song_count");
    lua_pushcfunction(L, l_plugin_library_get_songs); lua_setglobal(L, "library_get_songs");
    lua_pushcfunction(L, l_plugin_library_search); lua_setglobal(L, "library_search");
    lua_pushcfunction(L, l_plugin_library_get_song); lua_setglobal(L, "library_get_song");
    lua_pushcfunction(L, l_plugin_library_get_artists); lua_setglobal(L, "library_get_artists");
    lua_pushcfunction(L, l_plugin_library_get_albums); lua_setglobal(L, "library_get_albums");
}

static void prepare_data(void) {
    memset(song_rows, 0, sizeof(song_rows));
    song_rows[0].id = 41;
    snprintf(song_rows[0].path, sizeof(song_rows[0].path), "music/untagged.flac");
    snprintf(song_rows[0].tags.artist, sizeof(song_rows[0].tags.artist), "Boards of Canada");
    snprintf(song_rows[0].tags.album, sizeof(song_rows[0].tags.album), "Music Has the Right to Children");
    snprintf(song_rows[0].tags.album_artist, sizeof(song_rows[0].tags.album_artist), "Boards of Canada");
    snprintf(song_rows[1].tags.title, sizeof(song_rows[1].tags.title), "Tagged title");
    song_rows[1].id = 42;
    snprintf(song_rows[1].path, sizeof(song_rows[1].path), "music/tagged.flac");
    snprintf(song_rows[1].tags.artist, sizeof(song_rows[1].tags.artist), "Artist B");
    snprintf(song_rows[1].tags.album, sizeof(song_rows[1].tags.album), "Album B");
    snprintf(song_rows[1].tags.album_artist, sizeof(song_rows[1].tags.album_artist), "Various Artists");
    memset(group_rows, 0, sizeof(group_rows));
    snprintf(group_rows[0].name, sizeof(group_rows[0].name), "Boards of Canada");
    group_rows[0].song_count = 22;
    group_rows[0].first_song_id = 41;
    snprintf(group_rows[1].name, sizeof(group_rows[1].name), "Various Artists");
    group_rows[1].song_count = 88;
    group_rows[1].first_song_id = 42;
    snprintf(group_rows[1].album_artist, sizeof(group_rows[1].album_artist), "Various Artists");
    song_result_count = 2;
    group_result_count = 2;
    captured_total = 222;
}

static void test_info(lua_State * L) {
    run_lua(L, "assert(api_version() == PLUGIN_API_VERSION)");
    run_lua(L, "local i=get_app_info(); assert(i.version=='9.8.7' and i.build=='test-build' and i.platform=='host' and i.plugin_api==PLUGIN_API_VERSION)");
    run_lua(L, "assert(has_capability('ui.settings_dashboard')); assert(has_capability('network.http.download_progress')); assert(has_capability('playback.stay_on_screen')); assert(not has_capability('made.up.capability'))");
    run_lua(L, "local m=media_capabilities(); assert(#m.codecs==3 and m.codecs[1]=='mp3' and m.codecs[2]=='aac' and m.codecs[3]=='flac'); assert(#m.containers==3 and m.containers[1]=='mp3' and m.containers[2]=='adts' and m.containers[3]=='flac'); assert(m.max_sample_rate==0 and m.max_bit_depth==16 and m.max_channels==2); assert(m.direct_http_streaming and not m.range_seeking and not m.hls and not m.dash); assert(next(m.encryption_modes)==nil and next(m.drm_systems)==nil)");
}

static void test_library_queries(lua_State * L) {
    run_lua(L, "assert(library_song_count()==1234)");
    run_lua(L, "local a=get_artist_albums('Artist A'); assert(#a==2 and a[1]=='First Album' and a[2]=='Second Album')");
    assert(strcmp(captured_artist, "Artist A") == 0 && string_array_free_count == 1);
    run_lua(L, "assert(get_artist_albums('Missing')==nil)");
    assert(string_array_free_count == 1);
    run_lua(L, "local t=get_album_tracks('Artist A','Album A'); assert(#t==2 and t[1]=='music/first.flac' and t[2]=='music/second.flac')");
    assert(strcmp(captured_artist, "Artist A") == 0 && strcmp(captured_album, "Album A") == 0 && string_array_free_count == 2);
    run_lua(L, "local t=get_next_album_tracks('Artist A','Album A'); assert(#t==1 and t[1]=='music/next.flac')");
    assert(strcmp(captured_album, "Album A") == 0 && string_array_free_count == 3);

    run_lua(L, "local rows,total=library_get_songs(); assert(total==222 and #rows==2); local s=rows[1]; assert(s.id==41 and s.path=='music/untagged.flac' and s.title=='Basename fallback' and s.artist=='Boards of Canada' and s.album=='Music Has the Right to Children' and s.album_artist=='Boards of Canada'); assert(rows[2].title=='Tagged title')");
    assert(query_song_calls == 1 && captured_offset == 0 && captured_limit == GUI_PLUGIN_LIBRARY_MAX_PAGE);
    assert(captured_query[0] == '\0' && captured_artist[0] == '\0' && captured_album_artist[0] == '\0' && captured_album[0] == '\0');
    run_lua(L, "local rows,total=library_get_songs(7,25,{query='Boards',artist='Artist A',album_artist='Various Artists',album='Album X'}); assert(#rows==2 and total==222)");
    assert(query_song_calls == 2 && captured_offset == 7 && captured_limit == 25);
    assert(strcmp(captured_query, "Boards") == 0 && strcmp(captured_artist, "Artist A") == 0);
    assert(strcmp(captured_album_artist, "Various Artists") == 0 && strcmp(captured_album, "Album X") == 0);

    run_lua(L, "local rows=library_search('Boards',9); assert(#rows==2 and rows[1].title=='Basename fallback')");
    assert(query_search_calls == 1 && captured_limit == 9 && strcmp(captured_query, "Boards") == 0);
    run_lua(L, "local s=library_get_song(41); assert(s.id==41 and s.title=='Basename fallback'); assert(library_get_song(999)==nil)");
    missing_song = true;
    run_lua(L, "assert(library_get_song(41)==nil)");
    missing_song = false;

    run_lua(L, "local rows=library_get_artists(3,11); assert(#rows==2 and rows[1].name=='Boards of Canada' and rows[1].count==22 and rows[1].first_song_id==41 and rows[1].album_artist==''); assert(rows[2].album_artist=='Various Artists')");
    assert(query_artists_calls == 1 && captured_offset == 3 && captured_limit == 11);
    run_lua(L, "local rows=library_get_albums(4,12,'Artist X'); assert(#rows==2 and rows[1].name=='Boards of Canada')");
    assert(query_albums_calls == 1 && captured_offset == 4 && captured_limit == 12 && strcmp(captured_artist, "Artist X") == 0);
    assert(module_malloc_count == 5 && module_free_count == 5);

    song_result_count = 0;
    group_result_count = 0;
    run_lua(L, "local rows,total=library_get_songs(0,20); assert(type(rows)=='table' and #rows==0 and total==222); assert(#library_search('empty')==0); assert(#library_get_artists()==0); assert(#library_get_albums()==0)");
    song_result_count = 2;
    group_result_count = 2;

    int calls_before_oom = query_song_calls;
    int frees_before_oom = module_free_count;
    fail_next_module_malloc = true;
    run_lua(L, "local rows,total=library_get_songs(); assert(type(rows)=='table' and #rows==0 and total==0)");
    assert(query_song_calls == calls_before_oom && module_free_count == frees_before_oom);
}

int main(void) {
    prepare_data();
    lua_State * L = luaL_newstate();
    assert(L);
    luaL_openlibs(L);
    lua_pushinteger(L, PLUGIN_API_VERSION); lua_setglobal(L, "PLUGIN_API_VERSION");
    bind_test_api(L);
    test_info(L);
    test_library_queries(L);
    test_string_array_ownership(L);
    lua_close(L);
    assert(string_array_free_count == 4);
    puts("plugin info/library binding characterization: PASS");
    return 0;
}
