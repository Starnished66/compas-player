#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <string.h>

/* macOS (HFS+) returns file names in decomposed Unicode (NFD), so "é" arrives
 * as "e" plus a combining accent. The player's card, written on Linux or
 * Windows, and so the database, hold the composed form (NFC); stored paths must
 * match what the player will look up. Only the name is rewritten, and only when
 * it changes. */

#undef readdir

struct dirent *compat_readdir(DIR *dir) {
    static _Thread_local struct dirent entry;
    struct dirent *found = readdir(dir);
    if (!found) return NULL;
    entry = *found;

    CFMutableStringRef text = CFStringCreateMutable(kCFAllocatorDefault, 0);
    if (!text) return &entry;
    CFStringAppendCString(text, found->d_name, kCFStringEncodingUTF8);
    CFStringNormalize(text, kCFStringNormalizationFormC);
    char composed[sizeof(entry.d_name)];
    if (CFStringGetCString(text, composed, sizeof(composed), kCFStringEncodingUTF8))
        memcpy(entry.d_name, composed, strlen(composed) + 1);
    CFRelease(text);
    return &entry;
}
