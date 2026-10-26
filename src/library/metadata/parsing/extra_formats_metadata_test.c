/* Exercise real library tags through the shared Compas metadata matcher. */
#include "metadata.c"
#include <assert.h>

static void check_text(const track_metadata_t *tags) {
    assert(tags->has_title && strcmp(tags->title, "Format test") == 0);
    assert(tags->has_artist && strcmp(tags->artist, "Compas") == 0);
    assert(tags->has_album && strcmp(tags->album, "Pipeline") == 0);
    assert(tags->has_track_number && tags->track_number == 3);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    track_metadata_t tags = {0};
    read_wavpack_metadata(argv[1], &tags, false);
    check_text(&tags);
    assert(tags.has_replaygain && fabs(tags.replaygain_gain_db + 3.0) < 0.001);
    assert(!tags.picture_data && !tags.lyrics);
    memset(&tags, 0, sizeof(tags));
    read_caf_metadata(argv[2], &tags, false);
    check_text(&tags);
    assert(!tags.picture_data && !tags.lyrics);
    puts("WavPack and CAF metadata tests passed");
    return 0;
}
