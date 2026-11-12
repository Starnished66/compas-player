#ifndef SACD_ISO_H
#define SACD_ISO_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t start_sector;
    uint32_t end_sector; /* inclusive area audio range */
    uint32_t total_frames; /* 75 Hz area timeline */
    bool dst;
    unsigned channels;
    unsigned speaker_config;
} sacd_area_t;

typedef struct {
    uint32_t start_sector;
    uint32_t sector_count;
    uint32_t start_frame; /* 75 Hz area timeline */
    uint32_t frame_count;
    char title[128];
    char artist[128];
} sacd_track_t;

typedef struct {
    uint32_t start_sector;
    uint32_t end_sector; /* inclusive area audio range */
    uint32_t total_frames; /* 75 Hz area timeline */
    bool dst;
    unsigned channels;
    unsigned speaker_config;
    unsigned track_count;
    sacd_track_t tracks[255];
    char title[128];
    char artist[128];
} sacd_toc_t;

/* Prefers the stereo area, then selects a supported multichannel area. */
bool sacd_read_area(FILE *file, sacd_area_t *out);
/* Reads the selected SACD area TOC and per-track metadata. Does not close FILE. */
bool sacd_read_toc(FILE *file, sacd_toc_t *out);

#endif
