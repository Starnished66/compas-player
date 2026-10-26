#define _FILE_OFFSET_BITS 64
#include "sacd_iso.h"

#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>

#define SACD_SECTOR_SIZE 2048u
#define SACD_MASTER_SECTOR 510u
#define SACD_MASTER_SECTORS 2u /* Header and first-language text only. */
#define SACD_AREA_TOC_MAX 96u

static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool read_sector(FILE *file, uint64_t file_size, uint32_t sector, uint8_t *out) {
    uint64_t offset = (uint64_t)sector * SACD_SECTOR_SIZE;
    if (offset > file_size || SACD_SECTOR_SIZE > file_size - offset ||
        fseeko(file, (off_t)offset, SEEK_SET) != 0)
        return false;
    return fread(out, 1, SACD_SECTOR_SIZE, file) == SACD_SECTOR_SIZE;
}

/* SACD text is NUL terminated. Support ISO-646 and ISO-8859-1, and omit
 * unsupported multibyte character sets rather than returning invalid UTF-8. */
static void copy_text(char *out, size_t cap, const uint8_t *p, size_t n, unsigned charset) {
    size_t used = 0;
    if (charset != 1 && charset != 2) return;
    for (size_t i = 0; i < n && p[i] && used + 1 < cap; ++i) {
        unsigned c = p[i];
        if (charset == 1 && c > 0x7f) { out[0] = '\0'; return; }
        if (charset == 2 && c >= 0x80) {
            if (used + 2 >= cap) break;
            out[used++] = (char)(0xc0 | (c >> 6));
            out[used++] = (char)(0x80 | (c & 0x3f));
        } else out[used++] = (char)c;
    }
    out[used] = '\0';
}

static void parse_master_text(const uint8_t *master, sacd_toc_t *out, unsigned charset) {
    const uint8_t *block = master + SACD_SECTOR_SIZE;
    if (memcmp(block, "SACDText", 8) != 0) return;
    uint16_t title = be16(block + 16), artist = be16(block + 18);
    if (title >= 32 && title < SACD_SECTOR_SIZE)
        copy_text(out->title, sizeof(out->title), block + title, SACD_SECTOR_SIZE - title, charset);
    if (artist >= 32 && artist < SACD_SECTOR_SIZE)
        copy_text(out->artist, sizeof(out->artist), block + artist, SACD_SECTOR_SIZE - artist, charset);
}

static void parse_track_text(const uint8_t *sector, unsigned ntracks, unsigned charset,
                             sacd_track_t *tracks) {
    if (memcmp(sector, "SACDTTxt", 8) != 0) return;
    for (unsigned i = 0; i < ntracks; ++i) {
        uint16_t pos = be16(sector + 8 + i * 2);
        if (pos < 8 + ntracks * 2 || pos >= SACD_SECTOR_SIZE) continue;
        const uint8_t *p = sector + pos;
        size_t remain = SACD_SECTOR_SIZE - pos;
        if (remain < 4) continue;
        unsigned records = p[0];
        p += 4; remain -= 4;
        for (unsigned r = 0; r < records && remain >= 3; ++r) {
            unsigned type = p[0];
            p += 2; remain -= 2;
            size_t len = 0;
            while (len < remain && p[len]) len++;
            if (len) {
                if (type == 1) copy_text(tracks[i].title, sizeof(tracks[i].title), p, len, charset);
                else if (type == 2) copy_text(tracks[i].artist, sizeof(tracks[i].artist), p, len, charset);
            }
            if (len == remain) break; /* Missing terminator: don't underflow remain. */
            p += len + 1; remain -= len + 1;
            while (remain && *p == 0) { p++; remain--; }
        }
    }
}

static bool parse_area_header(FILE *file, uint64_t file_size, uint32_t toc_sector,
                              uint16_t master_area_size, uint8_t *area,
                              sacd_area_t *out) {
    if (!toc_sector || !master_area_size || master_area_size > SACD_AREA_TOC_MAX ||
        !read_sector(file, file_size, toc_sector, area) || memcmp(area, "TWOCHTOC", 8) != 0)
        return false;
    uint16_t area_size = be16(area + 10);
    unsigned channels = area[32], speaker_config = area[33] >> 3;
    unsigned frame_format = area[21] & 0x0f, tracks = area[69];
    uint32_t first_sector = be32(area + 72), last_sector = be32(area + 76);
    uint64_t file_sectors = file_size / SACD_SECTOR_SIZE;
    if (!area_size || area_size > master_area_size || area_size > SACD_AREA_TOC_MAX ||
        channels != 2 || speaker_config != 0 || area[20] != 4 ||
        (frame_format != 0 && frame_format != 2 && frame_format != 3) ||
        tracks == 0 || first_sector == 0 || first_sector > last_sector ||
        (uint64_t)last_sector >= file_sectors)
        return false;
    uint32_t total = (uint32_t)area[64] * 60u * 75u +
                     (uint32_t)area[65] * 75u + area[66];
    if (!total || area[65] >= 60 || area[66] >= 75) return false;
    out->start_sector = first_sector;
    out->end_sector = last_sector;
    out->total_frames = total;
    out->dst = frame_format == 0;
    return true;
}

static uint32_t select_stereo_area(FILE *file, uint64_t file_size, const uint8_t *master,
                                   sacd_area_t *out, uint8_t *header) {
    for (unsigned candidate = 0; candidate < 2; ++candidate) {
        unsigned offset = candidate == 0 ? 64 : 72;
        unsigned size_offset = candidate == 0 ? 84 : 86;
        uint32_t toc_sector = be32(master + offset);
        uint16_t toc_size = be16(master + size_offset);
        if (parse_area_header(file, file_size, toc_sector, toc_size, header, out))
            return toc_sector;
    }
    return 0;
}

bool sacd_read_area(FILE *file, sacd_area_t *out) {
    if (!file || !out) return false;
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 0) return false;
    uint8_t master[SACD_SECTOR_SIZE], area[SACD_SECTOR_SIZE];
    if (!read_sector(file, (uint64_t)st.st_size, SACD_MASTER_SECTOR, master) ||
        memcmp(master, "SACDMTOC", 8) != 0 || master[8] > 1 || master[9] > 20 ||
        !select_stereo_area(file, (uint64_t)st.st_size, master, out, area)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

static bool parse_stereo_area(FILE *file, uint64_t file_size,
                              const uint8_t *master, sacd_toc_t *out) {
    uint8_t area[SACD_SECTOR_SIZE], table[SACD_SECTOR_SIZE];
    sacd_area_t selected_area;
    uint32_t toc_sector = select_stereo_area(file, file_size, master, &selected_area, area);
    if (!toc_sector) return false;
    unsigned tracks = area[69], area_size = be16(area + 10);

    uint32_t first_sector = selected_area.start_sector;
    uint32_t last_sector = selected_area.end_sector;
    uint32_t total = selected_area.total_frames;
    uint64_t file_sectors = file_size / SACD_SECTOR_SIZE;

    uint8_t list1[SACD_SECTOR_SIZE], list2[SACD_SECTOR_SIZE];
    bool have_list1 = false, have_list2 = false, have_text = false;
    unsigned charset = area[90] & 0x07;
    for (unsigned i = 1; i < area_size; ++i) {
        uint64_t sector64 = (uint64_t)toc_sector + i;
        if (sector64 > UINT32_MAX || !read_sector(file, file_size, (uint32_t)sector64, table))
            return false;
        if (!have_list1 && memcmp(table, "SACDTRL1", 8) == 0) {
            memcpy(list1, table, sizeof(list1)); have_list1 = true;
        } else if (!have_list2 && memcmp(table, "SACDTRL2", 8) == 0) {
            memcpy(list2, table, sizeof(list2)); have_list2 = true;
        } else if (!have_text && memcmp(table, "SACDTTxt", 8) == 0) {
            parse_track_text(table, tracks, charset, out->tracks);
            have_text = true;
        }
    }
    if (!have_list1 || !have_list2) return false;

    uint32_t previous_sector_end = first_sector, previous_start_sector = 0, previous_frame_end = 0;
    for (unsigned i = 0; i < tracks; ++i) {
        uint32_t start = be32(list1 + 8 + i * 4);
        uint32_t count = be32(list1 + 8 + 255 * 4 + i * 4);
        const uint8_t *ts = list2 + 8 + i * 4;
        const uint8_t *td = list2 + 8 + 255 * 4 + i * 4;
        uint32_t start_frame = ((uint32_t)ts[0] * 60u + ts[1]) * 75u + ts[2];
        uint32_t frame_count = ((uint32_t)td[0] * 60u + td[1]) * 75u + td[2];
        uint64_t sector_end = (uint64_t)start + count;
        uint64_t frame_end = (uint64_t)start_frame + frame_count;
        /* ScarletBook rip boundaries can share the final sector of a prior
         * track; sacd-ripper computes non-final track spans through the next
         * track's start LSN (inclusive). Permit exactly that one-sector overlap. */
        if (!count || !frame_count || start < first_sector ||
            (i && (start <= previous_start_sector || (uint64_t)start + 1 < previous_sector_end)) ||
            sector_end - 1 > last_sector || sector_end > file_sectors ||
            ts[1] >= 60 || ts[2] >= 75 || td[1] >= 60 || td[2] >= 75 ||
            start_frame < previous_frame_end || frame_end > total)
            return false;
        sacd_track_t *track = &out->tracks[i];
        track->start_sector = start;
        track->sector_count = count;
        track->start_frame = start_frame;
        track->frame_count = frame_count;
        if (!track->title[0]) snprintf(track->title, sizeof(track->title), "Track %u", i + 1);
        previous_sector_end = (uint32_t)sector_end;
        previous_start_sector = start;
        previous_frame_end = (uint32_t)frame_end;
    }
    out->start_sector = selected_area.start_sector;
    out->end_sector = selected_area.end_sector;
    out->total_frames = selected_area.total_frames;
    out->dst = selected_area.dst;
    out->track_count = tracks;
    unsigned master_charset = master[138] & 0x07; /* first Master TOC locale charset */
    parse_master_text(master, out, master_charset);
    return true;
}

bool sacd_read_toc(FILE *file, sacd_toc_t *out) {
    if (!file || !out) return false;
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 0) return false;
    uint64_t file_size = (uint64_t)st.st_size;
    uint8_t *master = malloc(SACD_MASTER_SECTORS * SACD_SECTOR_SIZE);
    if (!master) return false;
    bool ok = false;
    if (!read_sector(file, file_size, SACD_MASTER_SECTOR, master) ||
        memcmp(master, "SACDMTOC", 8) != 0 || master[8] > 1 || master[9] > 20)
        goto done;
    /* Read only the first language; the remaining master text blocks are unused. */
    for (unsigned i = 1; i < SACD_MASTER_SECTORS; ++i)
        if (!read_sector(file, file_size, SACD_MASTER_SECTOR + i, master + i * SACD_SECTOR_SIZE))
            goto done;
    ok = parse_stereo_area(file, file_size, master, out);
done:
    free(master);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}
