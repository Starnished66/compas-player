#pragma once
/* Cover flow: a horizontal album carousel (flat centre cover, tilted sides).
 * Self-contained; the plugin manager reaches it only through
 * cover_flow_bindings.c. */
#include <stdbool.h>
#include <stdint.h>

#define COVER_FLOW_MAX_ITEMS 5000

typedef struct {
    const char * title;
    const char * subtitle;
    int64_t song_id; /* any song of the album, for its cover */
} cover_flow_item_t;

typedef struct {
    const char * title;               /* header title */
    const cover_flow_item_t * items;  /* copied */
    int count;                        /* capped at COVER_FLOW_MAX_ITEMS */
    int selected;                     /* 0-based start item */
    void (*on_select)(int index);     /* tap on the centre cover, 0-based */
    void (*on_hold)(int index);       /* long press on the centre cover, may be NULL */
    void (*on_closed)(void);          /* cleanup on destruction, may be NULL */
    bool az_index;                    /* A-Z strip; the items should be alphabetical */
} cover_flow_params_t;

/* UI thread. Opens the screen, or replaces the content of the open one in
 * place. False if it cannot be shown now (for example during a transition). */
bool cover_flow_show(const cover_flow_params_t * params);
/* Drops the screen without navigating, for UI reload. */
void cover_flow_shutdown(void);
/* Navigation dropped stack entries: frees the screen if it was covered and is gone. */
void cover_flow_note_nav_removed(void);
