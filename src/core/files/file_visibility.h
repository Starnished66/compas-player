#ifndef FILE_VISIBILITY_H
#define FILE_VISIBILITY_H

#include <stdbool.h>
#include <string.h>

/* Download staging files never belong in a browser listing. */
static inline bool file_name_is_hidden(const char *name, bool show_hidden) {
    if (strncmp(name, ".compas-download-", 17) == 0) return true;
    if (name[0] != '.') return false;
    if (!show_hidden) return true;
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

#endif
