#pragma once
/* Year, genre and track numbers of an album, recorded by the album loader
 * and read back by the album header (gui_album_header.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Order-sensitive hash of an album's song paths. */
uint64_t gui_album_facts_seed(int count);
uint64_t gui_album_facts_add(uint64_t hash, const char * path);
/* Copies a recorded album's facts; *track is malloc'd (count ints, 0 when the
 * tag is absent). False when nothing matching was recorded. */
bool gui_album_facts_lookup(uint64_t key, int count, int * year, char * genre, size_t genre_size,
                            int ** track);
