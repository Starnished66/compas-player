#ifndef APP_VERSION_H
#define APP_VERSION_H

/* Human-facing release identity, deliberately separate from BUILD_STAMP.
 * See app_version.c: its target object is rebuilt on every make invocation so
 * switching RELEASE_LABEL cannot leave an incremental build with stale text. */
const char * app_version_label(void);
const char * app_build_identifier(void);
/* Retained in the executable for model-safe player-only updates. */
const char * app_board_marker(void);
/* The board as a short name: "r1", "r3proii" or "r3ii_2025". */
const char * app_board_model(void);
/* The board as shown to people: "HiBy R1", "TempoTec V1", ... */
const char * app_board_display_name(void);

#endif
