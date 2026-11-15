#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char title[256], artist[128], artwork_url[1536];
    uint64_t stream_id, revision;
} icy_metadata_t;
/* Parses one complete, NUL-terminated ICY block. Missing fields retain state. */
bool icy_metadata_parse(const char * block, icy_metadata_t * state);
