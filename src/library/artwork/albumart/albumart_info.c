#include "albumart_info.h"

#include "../../parsers/cue/cue_parser.h"

#include <stdio.h>
#include <string.h>

void albumart_info_from_path_tags(const char * track_path, const char * artist, const char * album,
                                          const char * album_artist, albumart_info_t * info) {
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", track_path ? track_path : "");
    if (track_path && cue_reference_is(track_path))
        (void) cue_reference_audio_path(track_path, info->path, sizeof(info->path));
    snprintf(info->artist, sizeof(info->artist), "%s", artist ? artist : "");
    snprintf(info->album, sizeof(info->album), "%s", album ? album : "");
    snprintf(info->albumartist, sizeof(info->albumartist), "%s", album_artist ? album_artist : "");
}

void albumart_info_from_song_row(const song_row_t * song, albumart_info_t * info) {
    albumart_info_from_path_tags(song->path, song->tags.artist, song->tags.album, song->tags.album_artist, info);
}
