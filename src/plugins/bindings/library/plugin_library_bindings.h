#ifndef PLUGIN_LIBRARY_BINDINGS_H
#define PLUGIN_LIBRARY_BINDINGS_H

#include "lua.h"

int l_plugin_get_artist_albums(lua_State * L);
int l_plugin_get_album_tracks(lua_State * L);
int l_plugin_get_next_album_tracks(lua_State * L);
int l_plugin_library_song_count(lua_State * L);
int l_plugin_library_get_songs(lua_State * L);
int l_plugin_library_search(lua_State * L);
int l_plugin_library_get_song(lua_State * L);
int l_plugin_library_get_artists(lua_State * L);
int l_plugin_library_get_albums(lua_State * L);

#endif /* PLUGIN_LIBRARY_BINDINGS_H */
