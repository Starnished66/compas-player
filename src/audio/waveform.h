#ifndef WAVEFORM_H
#define WAVEFORM_H

#include <stdbool.h>
#include <stdint.h>

#define WAVEFORM_BINS 256

typedef struct {
    uint8_t bins[WAVEFORM_BINS];
    bool ready;
} waveform_data_t;

/* Asynchronously request bins for a local file. A new path supersedes any
 * active request; passing NULL cancels work and clears the published result. */
void waveform_request(const char * path);

/* Copies the completed result for exactly path. Returns false and clears out
 * when no matching result is ready. */
bool waveform_copy(const char * path, waveform_data_t * out);

/* Cancel the active/pending request and clear the published result. */
void waveform_cancel(void);

/* Stop and join the service worker. Safe before the worker was started. */
void waveform_shutdown(void);

#endif
