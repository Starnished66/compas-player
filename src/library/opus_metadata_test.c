/* Test the real Opus tag reader without linking unrelated metadata parsers.
 * Build with function/data sections and --gc-sections, like the existing
 * metadata_artwork_test harness. Fixtures live on persistent storage. */
#include "metadata.c"
#include <assert.h>

static void check_text(const track_metadata_t * tags) {
    assert(tags->has_title && strcmp(tags->title, "Opus test") == 0);
    assert(tags->has_artist && strcmp(tags->artist, "Compas") == 0);
    assert(tags->has_album && strcmp(tags->album, "Pipeline") == 0);
    assert(tags->has_replaygain && fabs(tags->replaygain_gain_db + 3.0) < 0.001);
}

int main(int argc, char ** argv) {
    assert(argc == 2);
    track_metadata_t tags = {0};
    read_opus_metadata(argv[1], &tags, false);
    check_text(&tags);
    assert(tags.lyrics == NULL && tags.picture_data == NULL);

    memset(&tags, 0, sizeof(tags));
    read_opus_metadata(argv[1], &tags, true);
    check_text(&tags);
    assert(tags.lyrics && strcmp(tags.lyrics, "Test lyrics") == 0);
    free(tags.lyrics);
    free(tags.picture_data);
    puts("Opus text, ReplayGain and lyrics metadata tests passed");
    return 0;
}
