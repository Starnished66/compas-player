#ifndef RESUME_CRASH_GUARD_H
#define RESUME_CRASH_GUARD_H

#include <stdbool.h>

/* Boot-resume crash guard. Takes the resume mode a boot is about to use. For
 * mode 1 (start playing) it leaves a marker in internal storage and returns 1;
 * the marker is cleared after playback has run safely for five minutes, or on
 * a clean shutdown or restart. If a marker is already there, the last
 * boot-resume died before clearing it, so this returns 2 (resume paused),
 * clears it for the next boot and leaves a second marker for the paused boot.
 * Decided once per boot. Other modes pass through, but a boot that shows the
 * track without playing it (paused or disabled resume) leaves the second
 * marker too, with the same five-minute clear.
 * The saved resume setting is never changed. */
int resume_crash_guard_filter(int resume_mode);

/* Second stage: true when the last boot was a guard-paused boot that did not
 * finish safely (its marker was still there). Then this boot restores no queue
 * or track at all. Decided once per boot; the marker is cleared for the next. */
bool resume_crash_guard_skip_restore(void);

/* True when this boot paused its resume because the marker was there. */
bool resume_crash_guard_tripped(void);

/* Removes both markers (clean shutdown or restart). */
void resume_crash_guard_clear(void);

#endif /* RESUME_CRASH_GUARD_H */
