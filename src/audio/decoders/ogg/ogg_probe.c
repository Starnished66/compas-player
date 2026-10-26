#include "ogg_probe.h"

#include <ogg/ogg.h>
#include <stdio.h>
#include <string.h>

#define OGG_PROBE_READ_CHUNK 4096
#define OGG_PROBE_MAX_BYTES (128 * 1024)

ogg_codec_t ogg_detect_codec(const char *path) {
    if (!path) return OGG_CODEC_UNKNOWN;

    FILE *f = fopen(path, "rb");
    if (!f) return OGG_CODEC_UNKNOWN;

    ogg_sync_state sync;
    ogg_stream_state stream;
    int sync_initialized = 0;
    int stream_initialized = 0;
    ogg_codec_t codec = OGG_CODEC_UNKNOWN;

    if (ogg_sync_init(&sync) != 0) goto done;
    sync_initialized = 1;

    size_t total_read = 0;
    unsigned char chunk[OGG_PROBE_READ_CHUNK];
    while (total_read < OGG_PROBE_MAX_BYTES) {
        size_t want = sizeof(chunk);
        if (want > OGG_PROBE_MAX_BYTES - total_read)
            want = OGG_PROBE_MAX_BYTES - total_read;
        size_t got = fread(chunk, 1, want, f);
        if (got == 0) break;
        total_read += got;

        char *buffer = ogg_sync_buffer(&sync, (long)got);
        if (!buffer) break;
        memcpy(buffer, chunk, got);
        if (ogg_sync_wrote(&sync, (long)got) != 0) break;

        ogg_page page;
        int page_result;
        while ((page_result = ogg_sync_pageout(&sync, &page)) != 0) {
            /* Do not resynchronize through damaged data or accept a later
             * chained stream as the file's initial codec. */
            if (page_result < 0 || ogg_page_version(&page) != 0) goto done;
            if (!stream_initialized) {
                if (ogg_page_bos(&page) == 0) goto done;
                if (ogg_stream_init(&stream, ogg_page_serialno(&page)) != 0)
                    goto done;
                stream_initialized = 1;
            } else if (ogg_page_bos(&page) != 0 ||
                       ogg_page_serialno(&page) != stream.serialno) {
                goto done;
            }
            if (ogg_stream_pagein(&stream, &page) != 0) goto done;

            ogg_packet packet;
            int packet_result = ogg_stream_packetout(&stream, &packet);
            if (packet_result < 0) goto done;
            if (packet_result == 0) {
                /* The initial header should fit in a small prefix. Continue
                 * only within OGG_PROBE_MAX_BYTES if it spans pages. */
                continue;
            }
            if (packet.bytes < 0 || (size_t)packet.bytes > OGG_PROBE_MAX_BYTES)
                goto done;
            if (packet.bytes >= 8 && memcmp(packet.packet, "OpusHead", 8) == 0) {
                codec = OGG_CODEC_OPUS;
            } else if (packet.bytes >= 7 && packet.packet[0] == 1 &&
                       memcmp(packet.packet + 1, "vorbis", 6) == 0) {
                codec = OGG_CODEC_VORBIS;
            }
            goto done;
        }
    }

done:
    if (stream_initialized) ogg_stream_clear(&stream);
    if (sync_initialized) ogg_sync_clear(&sync);
    fclose(f);
    return codec;
}
