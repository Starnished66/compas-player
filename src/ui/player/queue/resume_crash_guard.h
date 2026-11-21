#ifndef RESUME_CRASH_GUARD_H
#define RESUME_CRASH_GUARD_H

/* Boot-resume crash guard. Takes the resume mode a boot is about to use. For
 * mode 1 (start playing) it leaves a marker in internal storage and returns 1;
 * the marker is cleared after playback has run safely for a minute, or on a
 * clean exit. If a marker is already there, the last boot-resume died before
 * clearing it, so this returns 2 (resume paused) and clears it for the next
 * boot. Decided once per boot. Other modes pass through.
 * The saved resume setting is never changed. */
int resume_crash_guard_filter(int resume_mode);

/* Removes the marker (clean exit). */
void resume_crash_guard_clear(void);

#endif /* RESUME_CRASH_GUARD_H */
