#include "library_az_table.h"

int library_az_letter(const char * name) {
    unsigned char c = name ? (unsigned char) name[0] : 0;
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return c >= 'a' && c <= 'z' ? c - 'a' : 26;
}

void library_az_table_build(library_az_name_of_t name_of, void * ctx, int count, int jump[LIBRARY_AZ_LETTERS]) {
    for (int i = 0; i < LIBRARY_AZ_LETTERS; i++) jump[i] = -1;
    for (int i = count - 1; i >= 0; i--) jump[library_az_letter(name_of(i, ctx))] = i; /* lowest wins */
    for (int i = 24; i >= 0; i--) /* a letter without rows jumps forward */
        if (jump[i] < 0) jump[i] = jump[i + 1];
}
