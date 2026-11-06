#ifndef GUI_SUBSONIC_DOWNLOAD_HASH_H
#define GUI_SUBSONIC_DOWNLOAD_HASH_H
#include "../../../library/remote/subsonic/subsonic_cache.h"
#include <string.h>

static inline uint64_t subsonic_album_folder_hash(const char * artist, const char * album) {
    artist = artist ? artist : "";
    album = album ? album : "";
    uint64_t hash = subsonic_fnv64(SUBSONIC_FNV64_INIT, artist, strlen(artist));
    hash = subsonic_fnv64(hash, "\xff", 1);
    return subsonic_fnv64(hash, album, strlen(album));
}

static inline uint64_t subsonic_song_id_hash(const char * id) {
    return subsonic_fnv64(SUBSONIC_FNV64_INIT, id, strlen(id));
}
#endif
