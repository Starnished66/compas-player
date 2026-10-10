/* Prints every sample's size and a hash of its bytes, read in order and then
 * at scattered seek targets. scripts/test_mp4_compact.sh builds this once
 * with fully expanded sample tables and once in compact mode with tiny table
 * windows, and requires identical output. */
#include "mp4_demux.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t buf[1 << 16];

static int dump(mp4_demux_t * d, uint32_t i) {
    uint32_t size = 0;
    if (!mp4_demux_read_sample(d, i, buf, sizeof(buf), &size)) {
        printf("%" PRIu32 " FAIL\n", i);
        return 1;
    }
    uint32_t hash = 2166136261u;
    for (uint32_t k = 0; k < size; k++) hash = (hash ^ buf[k]) * 16777619u;
    printf("%" PRIu32 " %" PRIu32 " %08" PRIx32 "\n", i, size, hash);
    return 0;
}

int main(int argc, char ** argv) {
    if (argc != 2) return 2;
    mp4_demux_t * d = mp4_demux_open(argv[1]);
    if (!d) {
        puts("open FAIL");
        return 1;
    }
    uint32_t count = mp4_demux_get_sample_count(d);
    int failed = 0;
    printf("samples %" PRIu32 "\n", count);
    for (uint32_t i = 0; i < count; i++) failed |= dump(d, i);
    /* Backward and forward jumps, each followed by a short sequential run
     * that crosses chunk and window boundaries. */
    uint32_t state = 12345;
    for (int n = 0; n < 200; n++) {
        state = state * 1103515245u + 12345u;
        uint32_t at = (state >> 8) % count;
        for (uint32_t k = 0; k < 40 && at + k < count; k++) failed |= dump(d, at + k);
    }
    failed |= dump(d, count - 1);
    failed |= dump(d, 0);
    mp4_demux_close(d);
    return failed;
}
