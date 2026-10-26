#include "audio_tempo.h"
#include "sonic.h"
#include <math.h>
#include <stdlib.h>
struct audio_tempo {
    sonicStream stream;
    float speed;
    uint64_t source_fed, source_delivered;
    double fraction;
    bool flushed;
};
audio_tempo_t * audio_tempo_create(unsigned rate, unsigned channels, double speed, uint64_t position) {
    if (rate < 8000 || rate > 96000 || channels < 1 || channels > 2 ||
        !isfinite(speed) || speed < 0.5 || speed > 2.0) return NULL;
    audio_tempo_t * t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->stream = sonicCreateStream((int) rate, (int) channels);
    if (!t->stream) { free(t); return NULL; }
    t->speed = (float) speed;
    sonicSetSpeed(t->stream, t->speed);
    t->source_fed = t->source_delivered = position;
    return t;
}
void audio_tempo_destroy(audio_tempo_t * t) {
    if (t) { sonicDestroyStream(t->stream); free(t); }
}
bool audio_tempo_feed(audio_tempo_t * t, const int16_t * pcm, size_t frames) {
    /* Feed only after draining available output, bounding Sonic's queues. */
    if (!t || t->flushed || !pcm || frames > 8192 || sonicSamplesAvailable(t->stream) != 0) return false;
    if (!sonicWriteShortToStream(t->stream, pcm, (int) frames)) return false;
    t->source_fed += frames;
    return true;
}
bool audio_tempo_flush(audio_tempo_t * t) {
    if (!t) return false;
    if (t->flushed) return true;
    if (!sonicFlushStream(t->stream)) return false;
    t->flushed = true;
    return true;
}
size_t audio_tempo_read(audio_tempo_t * t, int16_t * pcm, size_t capacity) {
    if (!t || !pcm || capacity > 8192) return 0;
    return (size_t) sonicReadShortFromStream(t->stream, pcm, (int) capacity);
}
bool audio_tempo_drained(const audio_tempo_t * t) {
    return t && t->flushed && sonicSamplesAvailable(t->stream) == 0;
}
uint64_t audio_tempo_source_fed(const audio_tempo_t * t) { return t ? t->source_fed : 0; }
uint64_t audio_tempo_advance(audio_tempo_t * t, size_t delivered, bool fully_delivered) {
    double advance = delivered * (double) t->speed + t->fraction;
    uint64_t whole = (uint64_t) advance;
    t->fraction = advance - whole;
    uint64_t remaining = t->source_fed - t->source_delivered;
    t->source_delivered += whole < remaining ? whole : remaining;
    /* Snap only after the last output was delivered, never after a partial
     * device write. Sonic output length is approximate at short EOF tails. */
    if (fully_delivered && audio_tempo_drained(t)) {
        t->source_delivered = t->source_fed;
        t->fraction = 0;
    }
    return t->source_delivered;
}
