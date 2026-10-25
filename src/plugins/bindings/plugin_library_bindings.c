#include "plugin_library_bindings.h"

#include "lauxlib.h"
#include "plugin_library_bindings_internal.h"

#include "gui_plugin_library.h"
#include "metadata_db.h"

#include <stdint.h>
#include <stdlib.h>

/* Shared tail for get_artist_albums()/get_album_tracks()/
 * get_next_album_tracks() below -- pushes a malloc'd gui_plugin_* string
 * array as a 1-indexed Lua table (or nil if it came back empty/NULL) and
 * frees the C array, since nothing on the Lua side needs it once copied in. */
int plugin_library_push_string_array_result(lua_State * L, char ** items, int count) {
    if (!items || count <= 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        lua_pushstring(L, items[i]);
        lua_rawseti(L, -2, i + 1);
    }
    gui_plugin_free_string_array(items, count);
    return 1;
}

/* plugin.get_artist_albums(artist) -> { album_name, ... } | nil */
int l_plugin_get_artist_albums(lua_State * L) {
    const char * artist = luaL_checkstring(L, 1);
    int count = 0;
    char ** albums = gui_plugin_get_artist_albums(artist, &count);
    return plugin_library_push_string_array_result(L, albums, count);
}

/* plugin.get_album_tracks(artist, album) -> { track_path, ... } | nil */
int l_plugin_get_album_tracks(lua_State * L) {
    const char * artist = luaL_checkstring(L, 1);
    const char * album = luaL_checkstring(L, 2);
    int count = 0;
    char ** tracks = gui_plugin_get_album_tracks(artist, album, &count);
    return plugin_library_push_string_array_result(L, tracks, count);
}

/* plugin.get_next_album_tracks(artist, current_album) -> { track_path, ... }
 * | nil -- nil both when current_album isn't found and when it's the
 * artist's last album (see gui_plugin_get_next_album_tracks()'s own
 * comment: no wraparound). */
int l_plugin_get_next_album_tracks(lua_State * L) {
    const char * artist = luaL_checkstring(L, 1);
    const char * current_album = luaL_checkstring(L, 2);
    int count = 0;
    char ** tracks = gui_plugin_get_next_album_tracks(artist, current_album, &count);
    return plugin_library_push_string_array_result(L, tracks, count);
}

/* ---- plugin.library_* -- paged, DB-backed library access (see gui.h's own
 * gui_plugin_library_* comment for the design intent: bounded per call,
 * never a whole-library dump). Every row pushes the same {id, path, title,
 * artist, album, album_artist} shape for a song, or {name, count,
 * first_song_id} for an artist/album group. ---- */

static void push_song_row(lua_State * L, const song_row_t * row) {
    /* Use display title fallback (file basename) for untagged tracks. */
    char display_title[128];
    metadata_db_song_display_title(row, display_title, sizeof(display_title));

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer) row->id); lua_setfield(L, -2, "id");
    lua_pushstring(L, row->path); lua_setfield(L, -2, "path");
    lua_pushstring(L, display_title); lua_setfield(L, -2, "title");
    lua_pushstring(L, row->tags.artist); lua_setfield(L, -2, "artist");
    lua_pushstring(L, row->tags.album); lua_setfield(L, -2, "album");
    lua_pushstring(L, row->tags.album_artist); lua_setfield(L, -2, "album_artist");
}

static void push_group_row(lua_State * L, const group_row_t * row) {
    lua_newtable(L);
    lua_pushstring(L, row->name); lua_setfield(L, -2, "name");
    lua_pushinteger(L, row->song_count); lua_setfield(L, -2, "count");
    lua_pushinteger(L, (lua_Integer) row->first_song_id); lua_setfield(L, -2, "first_song_id");
    /* Empty ("") for artists/album_artists -- only ever populated by
     * metadata_db_get_albums_page_filtered() (see that function's own
     * comment) -- disambiguates two different artists' same-titled albums,
     * which now show as separate rows here instead of silently merging. */
    lua_pushstring(L, row->album_artist); lua_setfield(L, -2, "album_artist");
}

static int push_song_rows_result(lua_State * L, const song_row_t * rows, int count) {
    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        push_song_row(L, &rows[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* plugin.library_song_count() -> integer */
int l_plugin_library_song_count(lua_State * L) {
    lua_pushinteger(L, (lua_Integer) gui_plugin_library_song_count());
    return 1;
}

/* plugin.library_get_songs(offset, limit, filters) -> { song, ... }, total
 * -- offset/limit default to 0/GUI_PLUGIN_LIBRARY_MAX_PAGE; filters is an
 * optional table with any of query/artist/album_artist/album (all optional
 * substring-or-exact filters, see gui_plugin_library_get_songs()'s own
 * comment). total is the match count across every page, for a plugin's own
 * "page N of M" UI. */
int l_plugin_library_get_songs(lua_State * L) {
    int offset = (int) luaL_optinteger(L, 1, 0);
    int limit = (int) luaL_optinteger(L, 2, GUI_PLUGIN_LIBRARY_MAX_PAGE);
    const char * query = NULL, * artist = NULL, * album_artist = NULL, * album = NULL;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "query"); query = lua_tostring(L, -1);
        lua_getfield(L, 3, "artist"); artist = lua_tostring(L, -1);
        lua_getfield(L, 3, "album_artist"); album_artist = lua_tostring(L, -1);
        lua_getfield(L, 3, "album"); album = lua_tostring(L, -1);
        /* query/artist/album_artist/album above all point into the Lua
         * stack slots pushed by lua_getfield() -- valid until something
         * else touches the stack, which gui_plugin_library_get_songs()
         * below doesn't do, so reading them after all four is safe. */
    }

    /* Heap-allocated, not a stack array -- song_row_t is ~1.25KB (a 600-
     * byte path plus one cached_tags_t), and GUI_PLUGIN_LIBRARY_MAX_
     * PAGE (200) of those is ~250KB. remote_control.c hit exactly this as a
     * real on-device stack-overflow crash (see build_library_json()'s own
     * comment) before it was fixed the same way; not worth trusting that
     * this callback's own thread stack happens to be big enough just
     * because it's the main thread rather than a bare pthread_create(). */
    song_row_t * rows = malloc(sizeof(song_row_t) * GUI_PLUGIN_LIBRARY_MAX_PAGE);
    int64_t total = 0;
    int n = rows ? gui_plugin_library_get_songs(query, artist, album_artist, album, offset, limit, rows, &total) : 0;
    push_song_rows_result(L, rows, n);
    free(rows);
    lua_pushinteger(L, (lua_Integer) total);
    return 2;
}

/* plugin.library_search(query, limit) -> { song, ... } */
int l_plugin_library_search(lua_State * L) {
    const char * query = luaL_checkstring(L, 1);
    int limit = (int) luaL_optinteger(L, 2, GUI_PLUGIN_LIBRARY_MAX_PAGE);

    song_row_t * rows = malloc(sizeof(song_row_t) * GUI_PLUGIN_LIBRARY_MAX_PAGE); /* see get_songs()'s own comment */
    int n = rows ? gui_plugin_library_search(query, limit, rows) : 0;
    int result = push_song_rows_result(L, rows, n);
    free(rows);
    return result;
}

/* plugin.library_get_song(id) -> song | nil */
int l_plugin_library_get_song(lua_State * L) {
    int64_t id = (int64_t) luaL_checkinteger(L, 1);
    song_row_t row;
    if (!gui_plugin_library_get_song(id, &row)) {
        lua_pushnil(L);
        return 1;
    }
    push_song_row(L, &row);
    return 1;
}

/* plugin.library_get_artists(offset, limit) -> { {name, count,
 * first_song_id}, ... } */
int l_plugin_library_get_artists(lua_State * L) {
    int offset = (int) luaL_optinteger(L, 1, 0);
    int limit = (int) luaL_optinteger(L, 2, GUI_PLUGIN_LIBRARY_MAX_PAGE);

    group_row_t * rows = malloc(sizeof(group_row_t) * GUI_PLUGIN_LIBRARY_MAX_PAGE); /* see get_songs()'s own comment */
    int n = rows ? gui_plugin_library_get_artists(offset, limit, rows) : 0;
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        push_group_row(L, &rows[i]);
        lua_rawseti(L, -2, i + 1);
    }
    free(rows);
    return 1;
}

/* plugin.library_get_albums(offset, limit, artist) -> { {name, count,
 * first_song_id}, ... } -- artist optional (nil/omitted means every album,
 * unfiltered), matches either that name's artist or album_artist tag. */
int l_plugin_library_get_albums(lua_State * L) {
    int offset = (int) luaL_optinteger(L, 1, 0);
    int limit = (int) luaL_optinteger(L, 2, GUI_PLUGIN_LIBRARY_MAX_PAGE);
    const char * artist_filter = luaL_optstring(L, 3, NULL);

    group_row_t * rows = malloc(sizeof(group_row_t) * GUI_PLUGIN_LIBRARY_MAX_PAGE); /* see get_songs()'s own comment */
    int n = rows ? gui_plugin_library_get_albums(offset, limit, artist_filter, rows) : 0;
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        push_group_row(L, &rows[i]);
        lua_rawseti(L, -2, i + 1);
    }
    free(rows);
    return 1;
}
