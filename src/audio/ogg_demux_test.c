/* Standalone host regression tests for Ogg Opus demux bounds and empty pages.
 * Build with: cc -std=c11 -D_GNU_SOURCE -fsanitize=address,undefined \
 *   -g -O1 -Isrc/audio src/audio/ogg_demux.c src/audio/ogg_demux_test.c \
 *   -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc \
 *   -o /path/on/persistent/storage/ogg_demux_test
 */
#include "ogg_demux.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t crc_table[256];
static bool track_allocations;
static size_t max_tracked_allocation;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);

static void record_allocation(size_t size) {
    if (track_allocations && size > max_tracked_allocation)
        max_tracked_allocation = size;
}

void *__wrap_malloc(size_t size) {
    record_allocation(size);
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size) {
    size_t total = size && count > SIZE_MAX / size ? SIZE_MAX : count * size;
    record_allocation(total);
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
    record_allocation(size);
    return __real_realloc(ptr, size);
}

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_u64le(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static void init_crc(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t r = i << 24;
        for (unsigned j = 0; j < 8; ++j)
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
        crc_table[i] = r;
    }
}

static uint32_t page_crc(const uint8_t *p, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i)
        crc = (crc << 8) ^ crc_table[((crc >> 24) & 0xff) ^ p[i]];
    return crc;
}

static void write_page(FILE *f, uint8_t flags, uint64_t granule,
                       const uint8_t *laces, uint8_t lace_count,
                       const uint8_t *payload, size_t payload_size,
                       uint32_t sequence) {
    uint8_t header[27] = {0};
    memcpy(header, "OggS", 4);
    header[5] = flags;
    put_u64le(header + 6, granule);
    put_u32le(header + 14, 0x12345678);
    put_u32le(header + 18, sequence);
    header[26] = lace_count;
    assert(fwrite(header, 1, sizeof(header), f) == sizeof(header));
    if (lace_count) assert(fwrite(laces, 1, lace_count, f) == lace_count);
    if (payload_size) assert(fwrite(payload, 1, payload_size, f) == payload_size);

    size_t page_size = sizeof(header) + lace_count + payload_size;
    uint8_t *page = calloc(1, page_size);
    assert(page);
    memcpy(page, header, sizeof(header));
    if (lace_count) memcpy(page + sizeof(header), laces, lace_count);
    if (payload_size) memcpy(page + sizeof(header) + lace_count, payload, payload_size);
    uint32_t crc = page_crc(page, page_size);
    free(page);
    uint8_t encoded_crc[4];
    put_u32le(encoded_crc, crc);
    long end = ftell(f);
    assert(end >= (long)page_size);
    assert(fseek(f, end - (long)page_size + 22, SEEK_SET) == 0);
    assert(fwrite(encoded_crc, 1, 4, f) == 4);
    assert(fseek(f, 0, SEEK_END) == 0);
}

static FILE *temp_file(char *path) {
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w+b");
    assert(f);
    return f;
}

static FILE *fixture(char *path) {
    FILE *f = temp_file(path);
    const uint8_t head[19] = {
        'O','p','u','s','H','e','a','d', 1, 1, 0, 0,
        0x80, 0xbb, 0, 0, 0, 0, 0
    };
    const uint8_t head_lace[] = {19};
    const uint8_t tags[] = {'O','p','u','s','T','a','g','s'};
    const uint8_t tags_lace[] = {8};
    init_crc();
    write_page(f, 2, 0, head_lace, 1, head, sizeof(head), 0);
    write_page(f, 0, 0, tags_lace, 1, tags, sizeof(tags), 1);
    return f;
}

static FILE *head_only_fixture(char *path) {
    FILE *f = temp_file(path);
    const uint8_t head[19] = {
        'O','p','u','s','H','e','a','d', 1, 1, 0, 0,
        0x80, 0xbb, 0, 0, 0, 0, 0
    };
    const uint8_t head_lace[] = {19};
    init_crc();
    write_page(f, 2, 0, head_lace, 1, head, sizeof(head), 0);
    return f;
}

static void make_bad_tags(bool bad_vendor) {
    char path[] = "ogg-demux-bounds-XXXXXX";
    FILE *f = temp_file(path);
    const uint8_t head[19] = {
        'O','p','u','s','H','e','a','d', 1, 1, 0, 0,
        0x80, 0xbb, 0, 0, 0, 0, 0
    };
    const uint8_t head_lace[] = {19};
    uint8_t tags[20] = {'O','p','u','s','T','a','g','s'};
    size_t len;
    if (bad_vendor) {
        put_u32le(tags + 8, UINT32_MAX);
        len = 12;
    } else {
        put_u32le(tags + 8, 0);  /* empty vendor */
        put_u32le(tags + 12, 1); /* one comment */
        put_u32le(tags + 16, 0xfffffff0u);
        len = sizeof(tags);
    }
    uint8_t lace = (uint8_t)len;
    write_page(f, 2, 0, head_lace, 1, head, sizeof(head), 0);
    write_page(f, 0, 0, &lace, 1, tags, len, 1);
    assert(fflush(f) == 0);
    fclose(f);

    ogg_demux_t *d = ogg_demux_open(path);
    assert(d);
    assert(ogg_demux_get_comment_count(d) == 0);
    ogg_demux_close(d);
    unlink(path);
}

static void test_empty_pages_across_packet(void) {
    char path[] = "ogg-demux-empty-XXXXXX";
    FILE *f = fixture(path);
    assert(fseek(f, 0, SEEK_END) == 0);
    /* Empty page before the first audio packet. */
    write_page(f, 0, 0, NULL, 0, NULL, 0, 2);
    /* Packet starts and continues: a 255 lacing value means more follows. */
    uint8_t full_lace = 255;
    uint8_t first[255];
    memset(first, 'a', sizeof(first));
    write_page(f, 0, UINT64_MAX, &full_lace, 1, first, sizeof(first), 3);
    /* Empty continued page must not terminate or invent a packet segment. */
    write_page(f, 1, UINT64_MAX, NULL, 0, NULL, 0, 4);
    uint8_t last_lace = 1, last = 'z';
    write_page(f, 5, 256, &last_lace, 1, &last, 1, 5);
    fflush(f);

    assert(fflush(f) == 0);
    fclose(f);

    ogg_demux_t *d = ogg_demux_open(path);
    assert(d);
    uint8_t packet[300];
    uint32_t n = ogg_demux_read_packet(d, packet, sizeof(packet));
    assert(n == 256);
    for (unsigned i = 0; i < 255; ++i) assert(packet[i] == 'a');
    assert(packet[255] == 'z');
    assert(ogg_demux_read_packet(d, packet, sizeof(packet)) == 0);
    ogg_demux_close(d);
    unlink(path);
}

static void test_audio_open_discards_large_tags(void) {
    char path[] = "ogg-demux-large-XXXXXX";
    FILE *f = head_only_fixture(path);
    assert(fseek(f, 0, SEEK_END) == 0);

    /* A large but valid OpusTags packet with an empty vendor and one large
     * comment. Its 2 MiB body spans many Ogg pages. */
    const size_t tag_size = 2 * 1024 * 1024;
    uint8_t *tags = calloc(1, tag_size);
    assert(tags);
    memcpy(tags, "OpusTags", 8);
    put_u32le(tags + 8, 0);
    put_u32le(tags + 12, 1);
    put_u32le(tags + 16, (uint32_t)(tag_size - 20));
    memset(tags + 20, 'v', tag_size - 20);
    tags[20] = 'X';
    tags[21] = '=';

    size_t offset = 0;
    uint32_t sequence = 2;
    bool continued = false;
    while (tag_size - offset >= 255 * 255) {
        uint8_t laces[255];
        memset(laces, 255, sizeof(laces));
        write_page(f, continued ? 1 : 0, UINT64_MAX, laces, 255,
                   tags + offset, sizeof(laces) * 255, sequence++);
        offset += sizeof(laces) * 255;
        continued = true;
    }
    size_t remaining = tag_size - offset;
    uint8_t final_laces[255];
    uint8_t final_count = 0;
    while (remaining >= 255) {
        final_laces[final_count++] = 255;
        remaining -= 255;
    }
    final_laces[final_count++] = (uint8_t)remaining;
    size_t final_payload = tag_size - offset;
    write_page(f, (continued ? 1 : 0), 0, final_laces, final_count,
               tags + offset, final_payload, sequence++);
    free(tags);

    const uint8_t lace = 1, audio_byte = 0x42;
    write_page(f, 4, 1, &lace, 1, &audio_byte, 1, sequence);
    fflush(f);
    assert(fflush(f) == 0);
    fclose(f);

    max_tracked_allocation = 0;
    track_allocations = true;
    ogg_demux_t *d = ogg_demux_open_audio(path);
    track_allocations = false;
    assert(d);
    assert(max_tracked_allocation <= 128 * 1024);
    assert(ogg_demux_get_comment_count(d) == 0);
    uint8_t packet[8];
    assert(ogg_demux_read_packet(d, packet, sizeof(packet)) == 1);
    assert(packet[0] == audio_byte);
    ogg_demux_close(d);
    unlink(path);
}

int main(void) {
    init_crc();
    make_bad_tags(true);
    make_bad_tags(false);
    test_empty_pages_across_packet();
    test_audio_open_discards_large_tags();
    puts("ogg_demux tests passed");
    return 0;
}
