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
    unsigned track_count;
    sacd_track_t tracks[255];
    char title[128];
    char artist[128];
} sacd_toc_t;

/* Reads only the master and selected stereo area headers for playback. */
bool sacd_read_area(FILE *file, sacd_area_t *out);
/* Reads the stereo SACD area TOC and per-track metadata. Does not close FILE. */
bool sacd_read_toc(FILE *file, sacd_toc_t *out);

#endif
