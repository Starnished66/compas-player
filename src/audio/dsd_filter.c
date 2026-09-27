#include "dsd_filter.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DSD_A_ORDER 5
#define DSD_B_BETA 12.0
#define DSD_C_BETA 11.7
#define DSD_C_INTEGRATION_STEPS 1024

struct dsd_filter_design {
    unsigned int multiple;
    unsigned int base_rate;
    int references;
    int groups;           /* stage A window in bytes */
    int bytes_per_a;      /* bytes per stage A output: the decimation R / 8 */
    int bytes_per_output; /* 4 stage A outputs per PCM sample */
    float b_taps[DSD_FILTER_B_TAPS];
    float c_taps[DSD_FILTER_C_TAPS];
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

static double kaiser(int n, int num_taps, double beta) {
    double r = 2.0 * n / (num_taps - 1) - 1.0;
    return bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
}

/* Stage A's response at stage C's normalized frequency nu (cycles per
 * stage C input sample). Stage A runs at twice stage C's rate, R bits per
 * output. */
static double stage_a_gain(double nu, int r_bits) {
    double x = M_PI * nu / 2.0;
    if (x == 0.0) return 1.0;
    return pow(sin(x) / (r_bits * sin(x / r_bits)), DSD_A_ORDER);
}

/* Fifth-order moving average of R bits, unity DC gain, zero-padded at the
 * oldest end to whole bytes. */
static void design_stage_a(double * taps, int padded, int r_bits) {
    int length = DSD_A_ORDER * (r_bits - 1) + 1; /* at most DSD_FILTER_A_MAX_GROUPS * 8 */
    double work[DSD_FILTER_A_MAX_GROUPS * 8] = { 1.0 };
    double next[DSD_FILTER_A_MAX_GROUPS * 8];
    int filled = 1;
    for (int order = 0; order < DSD_A_ORDER; order++) {
        memset(next, 0, sizeof(double) * (size_t) length);
        for (int i = 0; i < filled; i++)
            for (int k = 0; k < r_bits; k++) next[i + k] += work[i] / r_bits;
        filled += r_bits - 1;
        memcpy(work, next, sizeof(double) * (size_t) length);
    }
    memset(taps, 0, sizeof(double) * (size_t) padded);
    memcpy(taps + (padded - length), work, sizeof(double) * (size_t) length);
}

/* Half-band Kaiser lowpass: cutoff at a quarter of its input rate. */
static void design_stage_b(float * out) {
    double taps[DSD_FILTER_B_TAPS], sum = 0.0;
    int m = (DSD_FILTER_B_TAPS - 1) / 2;
    for (int n = 0; n < DSD_FILTER_B_TAPS; n++) {
        int x = n - m;
        double sinc = x == 0 ? 0.5 : sin(M_PI * 0.5 * x) / (M_PI * x);
        taps[n] = sinc * kaiser(n, DSD_FILTER_B_TAPS, DSD_B_BETA);
        sum += taps[n];
    }
    for (int n = 0; n < DSD_FILTER_B_TAPS; n++) out[n] = (float) (taps[n] / sum);
}

/* Lowpass at a quarter of its input rate whose passband is 1 / stage A's
 * gain: the ideal response integrated numerically, then Kaiser windowed. */
static void design_stage_c(float * out, int r_bits) {
    double taps[DSD_FILTER_C_TAPS], sum = 0.0;
    int m = (DSD_FILTER_C_TAPS - 1) / 2;
    const double cutoff = 0.25;
    const double step = cutoff / DSD_C_INTEGRATION_STEPS;
    for (int n = 0; n < DSD_FILTER_C_TAPS; n++) {
        int x = n - m;
        double acc = 0.0;
        for (int i = 0; i <= DSD_C_INTEGRATION_STEPS; i++) {
            double nu = i * step;
            double weight = (i == 0 || i == DSD_C_INTEGRATION_STEPS) ? 1.0 : (i & 1) ? 4.0 : 2.0; /* Simpson */
            acc += weight * cos(2.0 * M_PI * nu * x) / stage_a_gain(nu, r_bits);
        }
        taps[n] = 2.0 * acc * step / 3.0 * kaiser(n, DSD_FILTER_C_TAPS, DSD_C_BETA);
        sum += taps[n];
    }
    for (int n = 0; n < DSD_FILTER_C_TAPS; n++) out[n] = (float) (taps[n] / sum);
}

static dsd_filter_design_t * build_design(unsigned int multiple, unsigned int base_rate) {
    int r_bits = (int) (multiple / 8);
    int length = DSD_A_ORDER * (r_bits - 1) + 1;
    int groups = (length + 7) / 8;
    dsd_filter_design_t * design = malloc(sizeof(*design) + sizeof(float) * 256u * (size_t) groups);
    double * taps = malloc(sizeof(double) * (size_t) groups * 8);
    if (!design || !taps) {
        free(design);
        free(taps);
        return NULL;
    }
    design->multiple = multiple;
    design->base_rate = base_rate;
    design->references = 0;
    design->groups = groups;
    design->bytes_per_a = r_bits / 8;
    design->bytes_per_output = 4 * design->bytes_per_a;
    design_stage_a(taps, groups * 8, r_bits);
    for (int g = 0; g < groups; g++) {
        for (int value = 0; value < 256; value++) {
            double acc = 0.0;
            for (int k = 0; k < 8; k++)
                acc += taps[g * 8 + k] * (((value >> (7 - k)) & 1) ? 1.0 : -1.0);
            design->table[g * 256 + value] = (float) acc;
        }
    }
    free(taps);
    design_stage_b(design->b_taps);
    design_stage_c(design->c_taps, r_bits);
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

int dsd_filter_warmup_outputs(const dsd_filter_design_t * design) {
    /* Stage A fills in groups bytes, B in its taps' worth of A outputs (4
     * per sample), C in its taps' worth of B outputs (2 per sample). */
    return (design->groups + design->bytes_per_output - 1) / design->bytes_per_output +
           (DSD_FILTER_B_TAPS + 3) / 4 + (DSD_FILTER_C_TAPS + 1) / 2 + 1;
}

void dsd_channel_reset(dsd_channel_state_t * ch) {
    memset(ch->bytes, DSD_SILENCE_BYTE, sizeof(ch->bytes));
    memset(ch->a_out, 0, sizeof(ch->a_out));
    memset(ch->b_out, 0, sizeof(ch->b_out));
    ch->byte_pos = 0;
    ch->a_pos = 0;
    ch->b_pos = 0;
}

/* Stage A output from the newest `groups` bytes, oldest first. */
static float stage_a(const dsd_channel_state_t * ch, const dsd_filter_design_t * design) {
    int groups = design->groups;
    const uint8_t * window = &ch->bytes[ch->byte_pos + DSD_FILTER_A_RING - groups];
    const float * row = design->table;
    float acc0 = 0.0f, acc1 = 0.0f;
    int g = 0;
    for (; g + 1 < groups; g += 2, row += 512) {
        acc0 += row[window[g]];
        acc1 += row[256 + window[g + 1]];
    }
    if (g < groups) acc0 += row[window[g]];
    return acc0 + acc1;
}

static void push_a(dsd_channel_state_t * ch, float value) {
    ch->a_out[ch->a_pos] = value;
    ch->a_out[ch->a_pos + DSD_FILTER_B_RING] = value;
    ch->a_pos = (ch->a_pos + 1) & (DSD_FILTER_B_RING - 1);
}

/* Half-band: only the centre and odd offsets from it are non-zero. */
static float stage_b(const dsd_channel_state_t * ch, const dsd_filter_design_t * design) {
    const float * w = &ch->a_out[ch->a_pos + DSD_FILTER_B_RING - DSD_FILTER_B_TAPS];
    const float * h = design->b_taps;
    const int mid = (DSD_FILTER_B_TAPS - 1) / 2;
    float acc = h[mid] * w[mid];
    for (int k = 1; k <= mid; k += 2) acc += h[mid - k] * (w[mid - k] + w[mid + k]);
    return acc;
}

static float stage_c(const dsd_channel_state_t * ch, const dsd_filter_design_t * design) {
    const float * w = &ch->b_out[ch->b_pos + DSD_FILTER_C_RING - DSD_FILTER_C_TAPS];
    const float * h = design->c_taps;
    const int mid = (DSD_FILTER_C_TAPS - 1) / 2;
    float acc0 = h[mid] * w[mid], acc1 = 0.0f;
    int n = 0;
    for (; n + 1 < mid; n += 2) {
        acc0 += h[n] * (w[n] + w[DSD_FILTER_C_TAPS - 1 - n]);
        acc1 += h[n + 1] * (w[n + 1] + w[DSD_FILTER_C_TAPS - 2 - n]);
    }
    if (n < mid) acc0 += h[n] * (w[n] + w[DSD_FILTER_C_TAPS - 1 - n]);
    return acc0 + acc1;
}

float dsd_channel_frame(dsd_channel_state_t * ch, const dsd_filter_design_t * design, const uint8_t * bytes) {
    int per_a = design->bytes_per_a;
    for (int half = 0; half < 2; half++) {
        for (int quarter = 0; quarter < 2; quarter++) {
            for (int i = 0; i < per_a; i++) {
                uint8_t byte = *bytes++;
                ch->bytes[ch->byte_pos] = byte;
                ch->bytes[ch->byte_pos + DSD_FILTER_A_RING] = byte;
                ch->byte_pos = (ch->byte_pos + 1) & (DSD_FILTER_A_RING - 1);
            }
            push_a(ch, stage_a(ch, design));
        }
        float b = stage_b(ch, design);
        ch->b_out[ch->b_pos] = b;
        ch->b_out[ch->b_pos + DSD_FILTER_C_RING] = b;
        ch->b_pos = (ch->b_pos + 1) & (DSD_FILTER_C_RING - 1);
    }
    return stage_c(ch, design);
}
