#define _POSIX_C_SOURCE 200809L
#include "src/library/cue_parser.h"
#include "src/library/metadata.h"
#include "src/library/sacd_iso.h"
#include "src/library/file_browser.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTOR 2048
#define SECTORS 1000

/* The host object bundle contains the app entry-point consumers too. */
void install_thread_crash_altstack(void) {}
void main_loop_discard_elapsed_time(void) {}

static bool count_song(const char *path, void *user) {
    int *count = user;
    assert(strstr(path, "/disc.iso") != NULL);
    (*count)++;
    return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint8_t *sector(uint8_t *iso, unsigned n) { return iso + (size_t)n * SECTOR; }

static void make_iso(uint8_t *iso) {
    uint8_t *master = sector(iso, 510);
    memcpy(master, "SACDMTOC", 8);
    master[8] = 1; master[9] = 20;
    put32(master + 64, 600); put32(master + 68, 600);
    put16(master + 84, 4); put16(master + 86, 4);
    master[138] = 2; /* ISO-8859-1 */
    uint8_t *master_text = sector(iso, 511);
    memcpy(master_text, "SACDText", 8);
    put16(master_text + 16, 40); put16(master_text + 18, 50);
    memcpy(master_text + 40, "Album", 6); memcpy(master_text + 50, "Artist", 7);

    uint8_t *area = sector(iso, 600);
    memcpy(area, "TWOCHTOC", 8); put16(area + 10, 4);
    area[20] = 4; area[21] = 3; area[32] = 2; area[33] = 0;
    area[90] = 2; /* first area locale: ISO-8859-1 */
    area[64] = 1; area[65] = 0; area[66] = 0; area[69] = 2;
    put32(area + 72, 700); put32(area + 76, 949);

    uint8_t *trl1 = sector(iso, 601);
    memcpy(trl1, "SACDTRL1", 8);
    put32(trl1 + 8, 700); put32(trl1 + 12, 800);
    put32(trl1 + 8 + 255 * 4, 101); put32(trl1 + 12 + 255 * 4, 150);
    uint8_t *trl2 = sector(iso, 602);
    memcpy(trl2, "SACDTRL2", 8);
    trl2[8] = 0; trl2[9] = 2; trl2[10] = 0;
    trl2[8 + 255 * 4] = 0; trl2[9 + 255 * 4] = 18; trl2[10 + 255 * 4] = 0;
    trl2[12] = 0; trl2[13] = 22; trl2[14] = 0;
    trl2[12 + 255 * 4] = 0; trl2[13 + 255 * 4] = 38; trl2[14 + 255 * 4] = 0;
    uint8_t *txt = sector(iso, 603);
    memcpy(txt, "SACDTTxt", 8); put16(txt + 8, 32);
    txt[32] = 1; txt[36] = 1; txt[37] = 0;
    memcpy(txt + 38, "First Track", 12);
}

int main(void) {
    char dir[] = "build_ui_test/sacd-iso-spec-XXXXXX";
    assert(mkdtemp(dir));
    char path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/disc.iso", dir) > 0);
    char game_path[PATH_MAX];
    assert(snprintf(game_path, sizeof(game_path), "%s/game.iso", dir) > 0);
    uint8_t *image = calloc(SECTORS, SECTOR); assert(image);
    make_iso(image);
    FILE *file = fopen(path, "wb+"); assert(file);
    assert(fwrite(image, SECTOR, SECTORS, file) == SECTORS);
    free(image);
    FILE *game = fopen(game_path, "wb"); assert(game);
    assert(fwrite("not an SACD", 1, 11, game) == 11); fclose(game);
    assert(file_browser_is_playable_name(game_path)); /* Classifier remains extension-only. */
    int visited = 0, walked = 0, skipped = 0;
    assert(file_browser_walk_all_songs_excluding_top_level(dir, NULL, count_song, &visited,
                                                            &walked, NULL, &skipped));
    assert(walked == 1 && visited == 1 && skipped == 0);
    sacd_toc_t *toc = calloc(1, sizeof(*toc)); assert(toc);
    assert(sacd_read_toc(file, toc));
    assert(toc->start_sector == 700 && toc->end_sector == 949);
    assert(toc->track_count == 2 && toc->total_frames == 4500 && !toc->dst);
    assert(strcmp(toc->title, "Album") == 0 && strcmp(toc->artist, "Artist") == 0);
    assert(toc->tracks[0].start_sector == 700 && toc->tracks[0].sector_count == 101);
    assert(toc->tracks[0].start_frame == 150 && toc->tracks[0].frame_count == 1350);
    assert(toc->tracks[1].start_frame == 1650 && toc->tracks[1].frame_count == 2850);
    assert(strcmp(toc->tracks[0].title, "First Track") == 0);
    assert(strcmp(toc->tracks[1].title, "Track 2") == 0);
    sacd_area_t area_info;
    assert(sacd_read_area(file, &area_info));
    assert(area_info.start_sector == 700 && area_info.end_sector == 949);
    assert(area_info.total_frames == 4500 && !area_info.dst);
    fclose(file);

    cue_sheet_t sheet = {0};
    assert(cue_parse_embedded(path, &sheet));
    assert(sheet.track_count == 2 && strcmp(sheet.title, "Album") == 0);
    assert(strcmp(sheet.tracks[0].title, "First Track") == 0);
    char ref[PATH_MAX];
    assert(cue_reference_format(path, 2, ref, sizeof(ref)));
    cue_sheet_t resolved = {0}; unsigned ordinal = 0;
    assert(cue_reference_resolve(ref, &resolved, &ordinal) && ordinal == 2);
    assert(strcmp(resolved.tracks[1].audio_path, path) == 0);
    assert(resolved.tracks[0].start_seconds == 2.0 && resolved.tracks[0].end_seconds == 22.0);
    assert(resolved.tracks[1].start_seconds == 22.0 && resolved.tracks[1].end_seconds == 60.0);
    assert(file_browser_is_playable_name(path));
    track_metadata_t metadata = {0};
    metadata_read(ref, &metadata);
    assert(metadata.has_title && strcmp(metadata.title, "Track 2") == 0);
    assert(metadata.has_artist && strcmp(metadata.artist, "Artist") == 0);
    assert(metadata.has_album && strcmp(metadata.album, "Album") == 0);
    assert(metadata.has_track_number && metadata.track_number == 2);
    free(metadata.picture_data); free(metadata.lyrics);
    cue_sheet_free(&resolved); cue_sheet_free(&sheet);

    file = fopen(path, "rb+"); assert(file);
    uint8_t bad_master[SECTOR]; assert(fseeko(file, (off_t)510 * SECTOR, SEEK_SET) == 0);
    assert(fread(bad_master, 1, SECTOR, file) == SECTOR);
    memset(bad_master + 64, 0xff, 4); /* invalid first area location */
    memset(bad_master + 72, 0xff, 4); /* invalid fallback area location */
    assert(fseeko(file, (off_t)510 * SECTOR, SEEK_SET) == 0);
    assert(fwrite(bad_master, 1, SECTOR, file) == SECTOR); fflush(file);
    assert(!sacd_read_toc(file, toc));
    fclose(file); free(toc); unlink(path); unlink(game_path); rmdir(dir);
    puts("sacd_iso_spec passed");
    return 0;
}
