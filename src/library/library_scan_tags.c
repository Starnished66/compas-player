#include "library_scan_tags.h"
#include <stdio.h>
#include <string.h>

void library_scan_tags_from_track(const track_metadata_t * meta, cached_tags_t * out) {
    memset(out, 0, sizeof(*out));
    out->track_number = meta->has_track_number ? meta->track_number : -1;
    out->disc_number = meta->has_disc_number ? meta->disc_number : -1;
    out->release_year = meta->has_release_year ? meta->release_year : -1;
    snprintf(out->title, sizeof(out->title), "%s", meta->has_title ? meta->title : "");
    snprintf(out->artist, sizeof(out->artist), "%s", meta->has_artist ? meta->artist : "Unknown Artist");
    snprintf(out->album, sizeof(out->album), "%s", meta->has_album ? meta->album : "Unknown Album");
    const char * album_artist = meta->has_album_artist ? meta->album_artist
                               : (meta->has_artist ? meta->artist : "Unknown Artist");
    snprintf(out->album_artist, sizeof(out->album_artist), "%s", album_artist);
    snprintf(out->genre, sizeof(out->genre), "%s", meta->has_genre ? meta->genre : "Unknown Genre");
}

bool library_scan_tags_equal(const cached_tags_t * a, const cached_tags_t * b) {
    return strcmp(a->title, b->title) == 0 && strcmp(a->artist, b->artist) == 0 &&
           strcmp(a->album, b->album) == 0 && strcmp(a->album_artist, b->album_artist) == 0 &&
           strcmp(a->genre, b->genre) == 0 && a->track_number == b->track_number &&
           a->disc_number == b->disc_number && a->release_year == b->release_year;
}
