#include "mseb.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* These parameters and gain laws follow the stock R1 1.6 / 1.8.b1 MSEB
 * and Sound Field modules. This engine is separate from the ten-band PEQ:
 * the temperature control alone requires four cascaded shelves. */
enum { LOW_SHELF, PEAK, HIGH_SHELF };
typedef struct { double hz; float q, gain; int control, type; } filter_spec_t;
static const filter_spec_t specs[MSEB_FILTER_COUNT] = {
    {33, .4425f, -.06f, 0, HIGH_SHELF},
    {231, .4425f, -.06f, 0, HIGH_SHELF},
    {1617, .4425f, -.06f, 0, HIGH_SHELF},
    {11319, .4425f, -.06f, 0, HIGH_SHELF},
    {70, .7289f, .25f, 1, LOW_SHELF},
    {100, .8571f, .20f, 2, PEAK},
    {200, .6667f, .12f, 3, PEAK},
    {650, .4041f, .125f, 4, PEAK},
    {3000, 1.414f, .10f, 5, PEAK},
    {5800, .9991f, .10f, 6, PEAK},
    {9200, .9991f, .10f, 7, PEAK},
    {7500, .4041f, .10f, 8, PEAK},
    {10000, .7289f, .25f, 9, HIGH_SHELF},
};

typedef struct { double b0, b1, b2, a1, a2; } coefficients_t;
typedef struct { float b0, b1, b2, a1, a2; } pcm_coefficients_t;
typedef struct { float x1, x2, y1, y2; } history_t;
typedef struct {
    coefficients_t coefficients[MSEB_FILTER_COUNT];
    bool active[MSEB_FILTER_COUNT];
    float compensation_db, gain;
} response_t;

static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
static mseb_config_t requested = { .width = 1.0 };
static response_t prepared_response;
static unsigned int prepared_rate;
static atomic_bool active = false;
static atomic_uint observed_rate = 44100;
static void prepare_response(const mseb_config_t *, unsigned int, response_t *);
static atomic_bool discontinuity = false;
/* Only the PCM thread accesses the following caches and filter histories. */
static mseb_config_t current;
static unsigned int current_rate;
static int current_channels;
static bool current_s32, running;
static pcm_coefficients_t pcm_coefficients[MSEB_FILTER_COUNT];
static history_t histories[MSEB_FILTER_COUNT][2];
static response_t pcm_response;

static unsigned int valid_rate(unsigned int rate) {
    return rate ? rate : 44100;
}

bool mseb_set_config(const mseb_config_t * config) {
    if (!config || !isfinite(config->width) || config->width < 0 || config->width > 2) return false;
    for (int i = 0; i < MSEB_CONTROL_COUNT; ++i)
        if (config->values[i] < -100 || config->values[i] > 100) return false;
    /* Sliders run on the control thread: prepare the expensive frequency
     * response outside the publication lock. Width-only edits reuse it. */
    response_t response;
    unsigned int rate = atomic_load_explicit(&observed_rate, memory_order_relaxed);
    pthread_mutex_lock(&config_mutex);
    bool reusable = prepared_rate == rate;
    for (int i = 0; i < MSEB_CONTROL_COUNT; ++i)
        reusable &= requested.values[i] == config->values[i];
    if (reusable) response = prepared_response;
    pthread_mutex_unlock(&config_mutex);
    if (!reusable) prepare_response(config, rate, &response);
    pthread_mutex_lock(&config_mutex);
    requested = *config;
    prepared_response = response;
    prepared_rate = rate;
    atomic_store_explicit(&active, config->enabled, memory_order_release);
    pthread_mutex_unlock(&config_mutex);
    return true;
}

void mseb_get_config(mseb_config_t * config) {
    if (!config) return;
    pthread_mutex_lock(&config_mutex);
    *config = requested;
    pthread_mutex_unlock(&config_mutex);
}

void mseb_reset(void) {
    mseb_config_t neutral = { .width = 1.0 };
    (void)mseb_set_config(&neutral);
    mseb_note_pcm_discontinuity();
}

void mseb_note_pcm_discontinuity(void) {
    atomic_store_explicit(&discontinuity, true, memory_order_relaxed);
}

static coefficients_t make_coefficients(int i, int value, unsigned int rate) {
    const filter_spec_t * spec = &specs[i];
    /* The stock setter multiplies in single precision before coefficient
     * setup in double precision; preserve its parameter rounding. */
    const double gain = (float)((float)value * spec->gain);
    const double a = pow(10.0, gain / 40.0);
    /* Avoid invalid or aliased filters on content below the stock cutoff.
     * At conventional PCM rates the stock frequencies are used exactly. */
    const double frequency = fmin(spec->hz, .49 * (double)rate);
    const double w = 6.28318530717958 * frequency / (double)rate;
    const double c = cos(w), alpha = sin(w) / (2.0 * spec->q);
    const double beta = 2.0 * sqrt(a) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (spec->type == PEAK) {
        b0 = 1 + alpha * a; b1 = -2 * c; b2 = 1 - alpha * a;
        a0 = 1 + alpha / a; a1 = -2 * c; a2 = 1 - alpha / a;
    } else if (spec->type == LOW_SHELF) {
        b0 = a * ((a + 1) - (a - 1) * c + beta);
        b1 = 2 * a * ((a - 1) - (a + 1) * c);
        b2 = a * ((a + 1) - (a - 1) * c - beta);
        a0 = (a + 1) + (a - 1) * c + beta;
        a1 = -2 * ((a - 1) + (a + 1) * c);
        a2 = (a + 1) + (a - 1) * c - beta;
    } else {
        b0 = a * ((a + 1) + (a - 1) * c + beta);
        b1 = -2 * a * ((a - 1) + (a + 1) * c);
        b2 = a * ((a + 1) + (a - 1) * c - beta);
        a0 = (a + 1) - (a - 1) * c + beta;
        a1 = 2 * ((a - 1) - (a + 1) * c);
        a2 = (a + 1) - (a - 1) * c - beta;
    }
    return (coefficients_t){b0/a0, b1/a0, b2/a0, a1/a0, a2/a0};
}

static double band_response_db(const coefficients_t * c, double frequency, unsigned int rate) {
    double w = 6.28318530717958 * frequency / (double)rate;
    double nr = c->b0 + c->b1 * cos(w) + c->b2 * cos(2*w);
    double ni = -c->b1 * sin(w) - c->b2 * sin(2*w);
    double dr = 1 + c->a1 * cos(w) + c->a2 * cos(2*w);
    double di = -c->a1 * sin(w) - c->a2 * sin(2*w);
    return 10.0 * log10(fmax(nr*nr+ni*ni, 1e-30) / fmax(dr*dr+di*di, 1e-30));
}

static void prepare_response(const mseb_config_t * config, unsigned int rate, response_t * out) {
    memset(out, 0, sizeof(*out));
    out->gain = 1;
    bool any = false;
    for (int b = 0; b < MSEB_FILTER_COUNT; ++b) {
        int value = config->values[specs[b].control];
        out->active[b] = value != 0;
        out->coefficients[b] = make_coefficients(b, value, rate);
        any |= out->active[b];
    }
    if (!any) return;
    float response[128], weights[128];
    const float log_min = (float)log10(20.0);
    for (int i = 0; i < 128; ++i) {
        float exponent = (float)(3.0f * (float)i / 128.0f + log_min);
        float frequency = (float)pow(10.0, exponent);
        response[i] = 0;
        for (int b = 0; b < MSEB_FILTER_COUNT; ++b)
            if (out->active[b]) response[i] += (float)band_response_db(&out->coefficients[b], frequency, rate);
        weights[i] = i < 30 ? 1.0f + (float)i / 29.0f
                           : .5f + ((float)(128-i) / 99.0f) * 1.5f;
    }
    float low = -100, high = 100, mid = 0;
    for (int n = 0; n < 10; ++n) {
        mid = (low + high) * .5f;
        float positive = 0, negative = 0;
        for (int i = 0; i < 128; ++i) {
            float weighted = (response[i] + mid) * weights[i];
            if (weighted > 0) positive += weighted;
            else negative -= weighted;
        }
        if (negative < 1e-6f || positive / negative > .2f) high = mid;
        else low = mid;
    }
    out->compensation_db = mid;
    out->gain = (float)pow(10.0, (float)(mid / 20.0f));
}

double mseb_get_compensation_db(unsigned int rate) {
    mseb_config_t config; response_t response;
    mseb_get_config(&config);
    prepare_response(&config, valid_rate(rate), &response);
    return response.compensation_db;
}

void mseb_get_response_db(const double * frequencies, double * response, size_t count, unsigned int rate) {
    if (!frequencies || !response) return;
    mseb_config_t config; response_t prepared;
    mseb_get_config(&config);
    rate = valid_rate(rate);
    prepare_response(&config, rate, &prepared);
    for (size_t i = 0; i < count; ++i) {
        double db = prepared.compensation_db;
        if (isfinite(frequencies[i]) && frequencies[i] > 0 && frequencies[i] < rate*.5)
            for (int b = 0; b < MSEB_FILTER_COUNT; ++b)
                if (prepared.active[b]) db += band_response_db(&prepared.coefficients[b], frequencies[i], rate);
        response[i] = db;
    }
}

static bool begin_block(size_t frames, int channels, unsigned int rate, bool s32) {
    if (rate) atomic_store_explicit(&observed_rate, rate, memory_order_relaxed);
    if (!atomic_load_explicit(&active, memory_order_acquire) ||
        channels < 1 || channels > 2 || !frames || !rate) {
        if (running) memset(histories, 0, sizeof(histories));
        running = false;
        return false;
    }
    mseb_config_t config;
    response_t response;
    unsigned int response_rate;
    /* Only bounded structure copies happen under this lock, never response
     * setup. The disabled PCM fast path above takes no lock. */
    pthread_mutex_lock(&config_mutex);
    config = requested;
    response = prepared_response;
    response_rate = prepared_rate;
    pthread_mutex_unlock(&config_mutex);
    if (!config.enabled) { running = false; return false; }
    bool reset = atomic_exchange_explicit(&discontinuity, false, memory_order_relaxed);
    bool changed = !running;
    for (int i = 0; i < MSEB_CONTROL_COUNT; ++i) changed |= current.values[i] != config.values[i];
    if (changed || rate != current_rate) {
        if (response_rate == rate) pcm_response = response;
        else {
            /* A real stream-rate change must be applied immediately. Unlike
             * slider edits, this bounded setup occurs once at the format
             * boundary, using the new rate rather than stock's stale cache. */
            prepare_response(&config, rate, &pcm_response);
        }
        for (int i = 0; i < MSEB_FILTER_COUNT; ++i) {
            coefficients_t * c = &pcm_response.coefficients[i];
            pcm_coefficients[i] = (pcm_coefficients_t){c->b0,c->b1,c->b2,c->a1,c->a2};
            if (!pcm_response.active[i]) memset(histories[i], 0, sizeof(histories[i]));
        }
    }
    if (reset || !running || current_rate != rate || current_channels != channels || current_s32 != s32)
        memset(histories, 0, sizeof(histories));
    current = config; current_rate = rate; current_channels = channels; current_s32 = s32; running = true;
    return true;
}

static void process_frame(float samples[2], int channels, float minimum, float maximum) {
    for (int ch = 0; ch < channels; ++ch) {
        float sample = samples[ch] * pcm_response.gain;
        for (int b = 0; b < MSEB_FILTER_COUNT; ++b) {
            if (!pcm_response.active[b]) continue;
            pcm_coefficients_t * c = &pcm_coefficients[b]; history_t * h = &histories[b][ch];
            float feed = c->b0*sample + c->b1*h->x1 + c->b2*h->x2;
            float feedback = c->a1*h->y1 + c->a2*h->y2;
            float output = feed - feedback;
            h->x2=h->x1; h->x1=sample; h->y2=h->y1; h->y1=output;
            sample=output;
            if (!isfinite(sample)) { sample=0; memset(h,0,sizeof(*h)); }
        }
        /* Stock MSEB saturates its PCM output before the separate Sound
         * Field module mixes channels. Preserve that stage boundary. */
        samples[ch] = fmaxf(minimum, fminf(maximum, sample));
    }
    if (channels == 2 && current.width != 1) {
        float mid = (samples[0]+samples[1])*.5f;
        float side = (samples[0]-samples[1])*(float)current.width*.5f;
        samples[0]=mid+side; samples[1]=mid-side;
    }
}

bool mseb_process(int16_t * samples, size_t frames, int channels, unsigned int rate) {
    if (!samples || !begin_block(frames,channels,rate,false)) return false;
    for (size_t i=0;i<frames;++i) {
        float frame[2]={0};
        for(int ch=0;ch<channels;++ch) frame[ch]=(float)samples[i*(size_t)channels+ch];
        process_frame(frame,channels,-32768.0f,32767.0f);
        for(int ch=0;ch<channels;++ch) samples[i*(size_t)channels+ch]=(int16_t)fmaxf(-32768,fminf(32767,frame[ch]));
    }
    return true;
}

bool mseb_process_s32(int32_t * samples, size_t frames, int channels, unsigned int rate) {
    if (!samples || !begin_block(frames,channels,rate,true)) return false;
    for (size_t i=0;i<frames;++i) {
        float frame[2]={0};
        for(int ch=0;ch<channels;++ch) frame[ch]=(float)samples[i*(size_t)channels+ch];
        process_frame(frame,channels,-8388608.0f,8388607.0f);
        for(int ch=0;ch<channels;++ch) samples[i*(size_t)channels+ch]=(int32_t)fmaxf(-8388608,fminf(8388607,frame[ch]));
    }
    return true;
}
