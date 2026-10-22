#include "cue_parser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char temp[] = "/tmp/compas-cue-parser-XXXXXX";
    int fd = mkstemp(temp);
    assert(fd >= 0);
    close(fd);
    char cue[128];
    snprintf(cue, sizeof(cue), "%s.cue", temp);
    assert(rename(temp, cue) == 0);
    FILE *f = fopen(cue, "wb");
    assert(f);
    fputs("PERFORMER \"Album Artist\"\nTITLE \"Album Name\"\nFILE \"disc "
          "one.flac\" WAVE\nTRACK 01 AUDIO\nTITLE \"First\"\nINDEX 01 "
          "00:00:00\nTRACK 02 AUDIO\nINDEX 01 03:00:00\nFILE \"disc two.flac\" "
          "WAVE\nTRACK 03 AUDIO\nTITLE \"Third\"\nINDEX 01 00:00:00\n",
          f);
    fclose(f);
    cue_sheet_t s;
    assert(cue_parse_file(cue, &s));
    assert(s.track_count == 3 && !strcmp(s.title, "Album Name"));
    assert(!strcmp(s.tracks[0].performer, "Album Artist"));
    assert(!strcmp(s.tracks[0].audio_path, "/tmp/disc one.flac"));
    assert(s.tracks[0].end_seconds == 180.0 && s.tracks[1].end_seconds == -1);
    assert(!strcmp(s.tracks[2].audio_path, "/tmp/disc two.flac"));
    assert(!strcmp(s.audio_path, s.tracks[0].audio_path));
    cue_sheet_free(&s);
    char ref[700], base[600];
    unsigned ordinal = 0;
    assert(cue_reference_format("/mnt/music/a.flac", 2, ref, sizeof(ref)));
    assert(cue_reference_is(ref));
    assert(cue_reference_parse(ref, base, sizeof(base), &ordinal));
    assert(ordinal == 2 && !strcmp(base, "/mnt/music/a.flac"));
    assert(!cue_reference_parse("a.flac#compas-cue=0", base, sizeof(base), &ordinal));
    assert(!cue_reference_parse("a.flac#compas-cue=2x", base, sizeof(base), &ordinal));
    assert(!cue_reference_format("a.flac#compas-cue=1", 2, ref, sizeof(ref)));
    char cue_ref[700], actual_path[600];
    assert(cue_reference_format(cue, 3, cue_ref, sizeof(cue_ref)));
    assert(cue_reference_audio_path(cue_ref, actual_path, sizeof(actual_path)));
    assert(!strcmp(actual_path, "/tmp/disc two.flac"));
    f = fopen(cue, "wb");
    assert(f);
    fputs("FILE \"C:\\Music\\song.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", f);
    fclose(f);
    assert(cue_parse_file(cue, &s));
    assert(!strcmp(s.tracks[0].audio_path, "C:\\Music\\song.flac"));
    cue_sheet_free(&s);
    f = fopen(cue, "wb");
    assert(f);
    fputs("FILE \"song.flac#compas-cue=1\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", f);
    fclose(f);
    assert(!cue_parse_file(cue, &s));
    f = fopen(cue, "wb");
    assert(f);
    fputs("FILE \"audio.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:60:00\n", f);
    fclose(f);
    assert(!cue_parse_file(cue, &s));
    f = fopen(cue, "wb");
    assert(f);
    fputs("FILE \"unterminated cue sheet", f);
    fclose(f);
    assert(!cue_parse_file(cue, &s));
    f = fopen(cue, "wb");
    assert(f);
    fputs("FILE \"audio.flac\" WAVE\n", f);
    for (int i = 0; i < 1025; i++) {
        int frames = i * 75;
        fprintf(f, "TRACK %04d AUDIO\nINDEX 01 %02d:%02d:%02d\n", i + 1, frames / 4500,
                (frames / 75) % 60, frames % 75);
    }
    fclose(f);
    assert(!cue_parse_file(cue, &s));
    if (argc > 1) {
        assert(cue_parse_embedded(argv[1], &s));
        assert(s.track_count == 2);
        assert(s.tracks[0].start_seconds == 12345.0 / 48000.0);
        assert(s.tracks[1].start_seconds == 100000.0 / 48000.0);
        assert(s.tracks[0].end_seconds == s.tracks[1].start_seconds);
        assert(s.tracks[1].end_seconds == 150000.0 / 48000.0);
        assert(!strcmp(s.tracks[0].audio_path, argv[1]));
        cue_sheet_free(&s);
    }
    if (argc > 2)
        assert(!cue_parse_embedded(argv[2], &s));
    if (argc > 3) {
        assert(cue_parse_embedded(argv[3], &s));
        assert(s.track_count == 2 && !strcmp(s.title, "Comment Album"));
        assert(!strcmp(s.performer, "Comment Performer"));
        assert(!strcmp(s.tracks[0].title, "One"));
        assert(s.tracks[0].end_seconds == 10.0 / 75.0);
        assert(!strcmp(s.tracks[1].audio_path, argv[3]));
        cue_sheet_free(&s);
    }
    if (argc > 4) {
        assert(cue_parse_embedded(argv[4], &s));
        assert(s.track_count == 2 && !strcmp(s.title, "Comment Album"));
        assert(!strcmp(s.performer, "Comment Performer"));
        assert(s.tracks[0].end_seconds == 10.0 / 75.0);
        assert(!strcmp(s.tracks[0].audio_path, argv[4]));
        cue_sheet_free(&s);
    }
    if (argc > 5) {
        assert(cue_parse_embedded(argv[5], &s));
        assert(s.track_count == 2 && s.tracks[1].start_seconds == 100000.0 / 48000.0);
        cue_sheet_free(&s);
    }
    if (argc > 6)
        assert(!cue_parse_embedded(argv[6], &s));
    unlink(cue);
    return 0;
}
