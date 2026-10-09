#ifndef OGG_PROBE_H
#define OGG_PROBE_H

typedef enum {
    OGG_CODEC_UNKNOWN = 0,
    OGG_CODEC_OPUS,
    OGG_CODEC_VORBIS,
} ogg_codec_t;

/* Identifies the codec from the first complete packet of an Ogg logical
 * stream. Reads at most a bounded prefix and uses libogg for framing. */
ogg_codec_t ogg_detect_codec(const char *path);

#endif /* OGG_PROBE_H */
