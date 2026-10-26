#ifndef ARTWORK_PRIORITY_H
#define ARTWORK_PRIORITY_H

typedef enum {
    ARTWORK_PRIO_WARMER = 0,    /* Background thumbnail cache warmer */
    ARTWORK_PRIO_THUMBNAIL = 1, /* Visible album row on active screen */
    ARTWORK_PRIO_PLAYER = 2,    /* Current player cover art or lyrics backdrop */
} artwork_priority_t;

#endif /* ARTWORK_PRIORITY_H */
