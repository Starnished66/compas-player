#include "app_version.h"

/* BUILD_STAMP remains a fixed-width machine value consumed by the bootloader
 * when it compares internal and SD-card builds. CI supplies APP_RELEASE_LABEL
 * (for example "Weekly Beta 2026-09-02"); ordinary local/host builds use the
 * explicit fallback below instead of pretending to be a published beta. */
#ifndef APP_RELEASE_LABEL
  #define APP_RELEASE_LABEL "Development Build"
#endif

const char * app_board_marker(void) {
#if defined(BOARD_R3PROII)
    return "COMPAS_PLAYER_BOARD:r3proii";
#elif defined(BOARD_R3II_2025)
    return "COMPAS_PLAYER_BOARD:r3ii_2025";
#elif defined(BOARD_TEMPOTEC_V1)
    return "COMPAS_PLAYER_BOARD:tempotec_v1";
#else
    return "COMPAS_PLAYER_BOARD:r1";
#endif
}

const char * app_version_label(void) {
#ifdef TEST_BUILD_TAG
    return "Test Build " TEST_BUILD_TAG;
#else
    return APP_RELEASE_LABEL;
#endif
}

const char * app_build_identifier(void) {
#ifdef TEST_BUILD_TAG
    return TEST_BUILD_TAG;
#elif defined(BUILD_STAMP)
    return BUILD_STAMP;
#else
    return "unknown";
#endif
}
