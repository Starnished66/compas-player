#ifndef ALBUMART_INFO_H
#define ALBUMART_INFO_H

#include "albumart.h"
#include "metadata_db.h"

void albumart_info_from_path_tags(const char * track_path, const char * artist,
                                  const char * album, const char * album_artist,
                                  albumart_info_t * info);
void albumart_info_from_song_row(const song_row_t * song, albumart_info_t * info);

#endif
