#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdio.h>

/* Diagnostic logging gated on TEST_BUILD_TAG. Production builds compile
 * these out entirely. Some messages fire repeatedly in tight loops
 * (e.g. audio reconnect), so gating avoids unnecessary flash writes
 * on deployed hardware where there is no reader for stderr output. */
#if defined(TEST_BUILD_TAG)
#define DBG_LOG_STDERR(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG_LOG_STDERR(...) ((void) 0)
#endif
#define DBG_LOG(...) DBG_LOG_STDERR(__VA_ARGS__)

#endif /* DEBUG_LOG_H */
