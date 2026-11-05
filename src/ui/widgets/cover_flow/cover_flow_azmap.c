#include "cover_flow_azmap.h"

/* 0..25 for a-z or A-Z, 26 for anything else (digits, punctuation, empty, NULL). */
static int az_letter(const char * title) {
    unsigned char c = title ? (unsigned char) title[0] : 0;
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return c >= 'a' && c <= 'z' ? c - 'a' : 26;
}

void cover_flow_az_build(const cover_flow_item_t * items, int count, int jump[COVER_FLOW_AZ_LETTERS],
                         bool has[COVER_FLOW_AZ_LETTERS]) {
    for (int i = 0; i < COVER_FLOW_AZ_LETTERS; i++) jump[i] = -1;
    for (int i = count - 1; i >= 0; i--) jump[az_letter(items[i].title)] = i; /* lowest wins */
    for (int i = 0; i < COVER_FLOW_AZ_LETTERS; i++) has[i] = jump[i] >= 0;
    for (int i = 24; i >= 0; i--) /* a letter without items jumps forward */
        if (jump[i] < 0) jump[i] = jump[i + 1];
}
