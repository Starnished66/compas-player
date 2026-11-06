#pragma once
/* One cover face decoded on a short-lived background thread. The UI thread holds
 * the request until it takes the result or drops it; whichever side finishes
 * last frees it. */
#include <stdbool.h>
#include <stdint.h>
#include "cover_flow.h"

typedef struct cover_flow_loader cover_flow_loader_t;

typedef struct {
    int item;         /* item index the request was made for */
    uint16_t * pixels; /* malloc'd face_px x rows RGB565, NULL if the decode failed; the caller frees */
    bool allow_generate; /* whether the request allowed generation */
} cover_flow_loader_result_t;

/* Starts decoding a face as face_px squared; `pixels` comes back `rows` rows tall. With `face` it
 * decodes `cover_ref` (copied), otherwise the album of `song_id`. NULL if no thread. */
cover_flow_loader_t * cover_flow_loader_start(int item, int64_t song_id, const char * cover_ref,
                                              cover_flow_face_fn face, bool allow_generate, int face_px, int rows);
/* Takes the result and frees the request (*l becomes NULL). False while it still runs. */
bool cover_flow_loader_take(cover_flow_loader_t ** l, cover_flow_loader_result_t * out);
/* Gives up the request (*l becomes NULL). Never blocks. */
void cover_flow_loader_drop(cover_flow_loader_t ** l);
