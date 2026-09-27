#include "dsd_filter.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DSD_TAPS_PER_64 448 /* 56 groups: a multiple of 4 for the unrolled sum */
#define DSD_KAISER_BETA 8.6

struct dsd_filter_design {
    unsigned int multiple;
    unsigned int base_rate;
    int references;
    int groups;           /* taps / 8 */
    int bytes_per_output; /* multiple / 16: the decimation in bytes */
    float table[];        /* groups x 256 partial sums; MSB of a byte is its earliest bit */
};

/* 2 base rates x 3 multiples. */
#define DSD_DESIGN_CACHE 6
static pthread_mutex_t design_mutex = PTHREAD_MUTEX_INITIALIZER;
static dsd_filter_design_t * designs[DSD_DESIGN_CACHE];

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; term > 1e-12 * sum; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

/* Kaiser windowed-sinc lowpass, symmetric, unity DC gain. */
static void design_lowpass(double * taps, int num_taps, double cutoff_hz, double rate_hz) {
    int m = num_taps - 1;
    double c = 2.0 * cutoff_hz / rate_hz;
    double norm = bessel_i0(DSD_KAISER_BETA);
    double sum = 0.0;
    for (int n = 0; n < num_taps; n++) {
        double x = n - m / 2.0;
        double sinc = x == 0.0 ? c : sin(M_PI * c * x) / (M_PI * x);
        double r = 2.0 * n / m - 1.0;
        taps[n] = sinc * bessel_i0(DSD_KAISER_BETA * sqrt(1.0 - r * r)) / norm;
        sum += taps[n];
    }
    for (int n = 0; n < num_taps; n++) taps[n] /= sum;
}

static dsd_filter_design_t * build_design(unsigned int multiple, unsigned int base_rate) {
    int taps_count = DSD_TAPS_PER_64 * (int) (multiple / 64);
    int groups = taps_count / 8;
    dsd_filter_design_t * design = malloc(sizeof(*design) + sizeof(float) * 256u * (size_t) groups);
    double * taps = malloc(sizeof(double) * (size_t) taps_count);
    if (!design || !taps) {
        free(design);
        free(taps);
        return NULL;
    }
    design->multiple = multiple;
    design->base_rate = base_rate;
    design->references = 0;
    design->groups = groups;
    design->bytes_per_output = (int) (multiple / 16);
    design_lowpass(taps, taps_count, (double) base_rate, (double) multiple * base_rate);
    for (int g = 0; g < groups; g++) {
        for (int value = 0; value < 256; value++) {
            double acc = 0.0;
            for (int k = 0; k < 8; k++)
                acc += taps[g * 8 + k] * (((value >> (7 - k)) & 1) ? 1.0 : -1.0);
            design->table[g * 256 + value] = (float) acc;
        }
    }
    free(taps);
    return design;
}

const dsd_filter_design_t * dsd_filter_design_acquire(unsigned int multiple, unsigned int base_rate) {
    if ((multiple != 64 && multiple != 128 && multiple != 256) || (base_rate != 44100 && base_rate != 48000))
        return NULL;
    pthread_mutex_lock(&design_mutex);
    dsd_filter_design_t * found = NULL;
    int free_slot = -1;
    for (int i = 0; i < DSD_DESIGN_CACHE; i++) {
        if (designs[i] && designs[i]->multiple == multiple && designs[i]->base_rate == base_rate) found = designs[i];
        else if (!designs[i] && free_slot < 0) free_slot = i;
    }
    if (!found && free_slot >= 0) {
        found = build_design(multiple, base_rate);
        designs[free_slot] = found;
    }
    if (found) found->references++;
    pthread_mutex_unlock(&design_mutex);
    return found;
}

void dsd_filter_design_release(const dsd_filter_design_t * design) {
    if (!design) return;
    pthread_mutex_lock(&design_mutex);
    for (int i = 0; i < DSD_DESIGN_CACHE; i++) {
        if (designs[i] != design) continue;
        if (--designs[i]->references == 0) {
            free(designs[i]);
            designs[i] = NULL;
        }
        break;
    }
    pthread_mutex_unlock(&design_mutex);
}

int dsd_filter_groups(const dsd_filter_design_t * design) {
    return design->groups;
}

int dsd_filter_bytes_per_output(const dsd_filter_design_t * design) {
    return design->bytes_per_output;
}

void dsd_channel_reset(dsd_channel_state_t * ch) {
    memset(ch->bytes, DSD_SILENCE_BYTE, sizeof(ch->bytes));
    ch->byte_pos = 0;
    ch->byte_phase = 0;
}

bool dsd_channel_feed_byte(dsd_channel_state_t * ch, const dsd_filter_design_t * design, uint8_t byte,
                           float * out_sample) {
    int groups = design->groups;
    ch->bytes[ch->byte_pos] = byte;
    ch->bytes[ch->byte_pos + groups] = byte;
    if (++ch->byte_pos == groups) ch->byte_pos = 0;
    if (++ch->byte_phase < design->bytes_per_output) return false;
    ch->byte_phase = 0;

    /* The window runs oldest to newest, pairing byte g with taps 8g..8g+7.
     * groups is a multiple of 4 (56 per DSD64 multiple); four independent
     * sums keep the float adds from serializing on each other's latency. */
    const uint8_t * window = &ch->bytes[ch->byte_pos];
    const uint8_t * window_end = window + groups;
    const float * row0 = design->table, * row1 = row0 + 256, * row2 = row0 + 512, * row3 = row0 + 768;
    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
    for (; window < window_end; window += 4, row0 += 1024, row1 += 1024, row2 += 1024, row3 += 1024) {
        acc0 += row0[window[0]];
        acc1 += row1[window[1]];
        acc2 += row2[window[2]];
        acc3 += row3[window[3]];
    }
    *out_sample = (acc0 + acc1) + (acc2 + acc3);
    return true;
}
