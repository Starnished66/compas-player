#ifndef PLAYER_TIMELINE_LABELS_H
#define PLAYER_TIMELINE_LABELS_H

#include "lvgl.h"

/* Capture eligible XML timeline labels before playback. Returns true when at
 * least one label was captured. Unsupported labels keep rendering normally. */
bool player_timeline_labels_begin(lv_anim_timeline_t * timeline);

/* Commit animation endpoints, then restore captured labels after playback. */
void player_timeline_labels_end(void);

/* Cancel a capture and commit the timeline's current sampled property values. */
void player_timeline_labels_teardown(void);

#endif /* PLAYER_TIMELINE_LABELS_H */
