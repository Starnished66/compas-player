#define _FILE_OFFSET_BITS 64
#include "src/library/metadata/parsing/metadata_parser_internal.h"
#include <assert.h>
#include <string.h>

void install_thread_crash_altstack(void) {}
bool main_loop_discard_pointer_input(void) { return false; }
void main_loop_discard_elapsed_time(void) {}

static void comment(track_metadata_t *m, const char *s) {
    apply_vorbis_comment_field(m, s, strlen(s), false);
}

static void id3_year(const char *id, unsigned version, const char *value, bool blobs, bool original_first, bool utf16) {
    unsigned char tag[256] = { 'I', 'D', '3', 0, 0, 0, 0, 0, 0, 0 };
    tag[3] = version;
    size_t pos = 10;
    for (int i = 0; i < 2; i++) {
        bool original = (i == 0) == original_first;
        const char *frame = original ? id : version == 2 ? "TYE" : version == 4 ? "TDRC" : "TYER";
        const char *text = original ? value : "2024";
        size_t id_len = version == 2 ? 3 : 4;
        size_t n = strlen(text) + 1;
        bool txxx = strcmp(frame, "TXXX") == 0;
        const char *description = utf16 ? "ORIGINALDATE" : "ORIGINALYEAR";
        if (txxx) n += strlen(description) + 1;
        if (utf16) n = 1 + 2 * (n - 1) + (txxx ? 4 : 2);
        memcpy(tag + pos, frame, id_len);
        size_t header = version == 2 ? 6 : 10;
        tag[pos + (version == 2 ? 5 : 7)] = n;
        pos += header;
        tag[pos++] = utf16 ? 1 : 3;
        if (utf16) { tag[pos++] = 0xfe; tag[pos++] = 0xff; }
        if (txxx) {
            for (size_t j = 0; j <= strlen(description); j++) {
                if (utf16) tag[pos++] = 0;
                tag[pos++] = description[j];
            }
        }
        if (utf16 && txxx) { tag[pos++] = 0xfe; tag[pos++] = 0xff; }
        for (size_t j = 0; j < strlen(text); j++) {
            if (utf16) tag[pos++] = 0;
            tag[pos++] = text[j];
        }
    }
    tag[8] = (pos - 10) >> 7;
    tag[9] = (pos - 10) & 127;
    FILE *f = tmpfile();
    assert(f);
    assert(fwrite(tag, 1, pos, f) == pos);
    rewind(f);
    track_metadata_t m = {0};
    assert(read_id3v2(f, &m, blobs));
    assert(m.has_release_year);
    assert(m.release_year == (strcmp(value, "bad") == 0 ? 2024 : 1987));
    fclose(f);
}

int main(void) {
    const char *originals[] = { "ORIGINALDATE=1987-06-12", "originalyear=1987" };
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned order = 0; order < 2; order++) {
            track_metadata_t m = {0};
            comment(&m, order ? originals[i] : "DATE=2024");
            comment(&m, order ? "DATE=2024" : originals[i]);
            comment(&m, "YEAR=2025");
            assert(m.has_release_year && m.release_year == 1987);
        }
    }
    track_metadata_t m = {0};
    comment(&m, "ORIGINALDATE=1987-06-12");
    comment(&m, "ORIGINALYEAR=bad");
    assert(m.has_release_year && m.release_year == 1987);
    memset(&m, 0, sizeof(m));
    comment(&m, "DATE=2024");
    assert(m.has_release_year && m.release_year == 2024);
    memset(&m, 0, sizeof(m));
    comment(&m, "ORIGINALDATE=1987-99-12");
    comment(&m, "DATE=2024");
    comment(&m, "ORIGINALYEAR=bad");
    assert(m.release_year == 2024 && !m.has_original_year);
    const char *ids[] = { "TDOR", "TORY", "TXXX", "TOR" };
    for (unsigned i = 0; i < 4; i++)
        for (unsigned blobs = 0; blobs < 2; blobs++)
            for (unsigned order = 0; order < 2; order++) {
                unsigned version = i == 0 ? 4 : i == 3 ? 2 : 3;
                id3_year(ids[i], version, i == 0 ? "1987-06-12" : "1987", blobs, order, false);
                id3_year(ids[i], version, "bad", blobs, order, false);
            }
    for (unsigned blobs = 0; blobs < 2; blobs++)
        id3_year("TXXX", 3, "1987-06-12T00:00:00", blobs, true, true);
    puts("original release year metadata tests passed");
}
