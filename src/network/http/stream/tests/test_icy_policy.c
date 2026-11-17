#include "src/audio/engine/audio_internal.h"
#include "src/library/remote/remote_track.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static bool wanted;
static int opens;
http_stream_t * __wrap_http_stream_open(const char * url, bool verify, bool icy, uint64_t generation) {
    assert(generation == 0 || generation == 7);
    (void)url; (void)verify; assert(icy == wanted); ++opens; return NULL;
}
bool __wrap_remote_track_meta_copy_fresh_for_path(const char * path, remote_track_meta_t * out) {
    (void)path; memset(out, 0, sizeof(*out));
    strcpy(out->stream_url, "https://provider.test/song"); return true;
}
int main(void) {
    decoder_t decoder;
    wanted = true;
    assert(!decoder_open(&decoder, "http://station.test/live"));
    wanted = false;
    assert(!decoder_open(&decoder, "https://server.test/rest/stream.view?id=1"));
    assert(!decoder_open_next(&decoder, "remote://provider/song", 7));
    assert(opens == 3);
    puts("Decoder requests ICY only for radio, excluding Subsonic and provider songs");
}
