#ifndef COMPAS_SACD_READER_H
#define COMPAS_SACD_READER_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct sacd_reader sacd_reader_t;
/* Borrows f; returns DSD sample count per channel. */
sacd_reader_t *sacd_reader_open(FILE *f, uint64_t *samples);
sacd_reader_t *sacd_reader_open_dff(FILE *f, uint64_t start, uint64_t size,
                                    unsigned channels, uint64_t index_start, uint64_t index_size,
                                    uint64_t *samples);
unsigned sacd_reader_channels(const sacd_reader_t *reader);
bool sacd_reader_is_dst(const sacd_reader_t *reader);
void sacd_reader_close(sacd_reader_t *reader);
bool sacd_reader_eof(const sacd_reader_t *reader);
/* 1..16 bytes per channel, within one SACD frame. PCM/DoP reads are aligned. */
bool sacd_reader_bytes(sacd_reader_t *reader, uint8_t *channels[], unsigned count);
bool sacd_reader_seek(sacd_reader_t *reader, uint64_t byte_per_channel);
#endif
