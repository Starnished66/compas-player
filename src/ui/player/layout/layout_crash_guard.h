#ifndef LAYOUT_CRASH_GUARD_H
#define LAYOUT_CRASH_GUARD_H

#include <stdbool.h>

/* Player layout crash guard. Takes the layout id the player screen is about to
 * build and returns the id to build. A non-default layout is recorded by id in
 * a marker in internal storage, cleared a few seconds after the screen is
 * built. If at the first call of a boot the marker names the id about to be
 * built, the last boot died building or showing that layout: this returns the
 * Default layout for that id, every boot, until the user selects a layout. Any
 * other id (another pick, a plugin session layout) is built normally and takes
 * over the marker; the tripped id stays recorded and suppressed. The saved
 * layout setting is never changed. */
const char * layout_crash_guard_filter(const char * layout_id);

/* True while a layout is being replaced by Default because of a crash. */
bool layout_crash_guard_tripped(void);

/* The user selected a layout: removes the marker and lifts the fallback, so
 * the next build uses the selection. */
void layout_crash_guard_clear(void);

/* Clean shutdown or restart: removes the marker, unless a layout is tripped,
 * in which case its id stays recorded for the next boot. */
void layout_crash_guard_clean_exit(void);

#endif /* LAYOUT_CRASH_GUARD_H */
