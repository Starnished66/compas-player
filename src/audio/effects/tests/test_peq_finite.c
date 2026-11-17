#include "../peq.c"
#include <assert.h>

static void test_combined_response(void) {
    peq_set_preamp_db(0.0);
    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        peq_set_band(i, 1000.0, i < 3 ? 12.0 : 0.0, 1.0);
        peq_set_band_enabled(i, i < 3);
    }
    double frequency = 1000.0, response;
    peq_get_filter_response_db(&frequency, &response, 1, 48000);
    assert(fabs(response - 36.0) < 0.01);
}

int main(void) {
    peq_set_band(0, 1000.0, 300.0, 1.0);
    assert(peq_get_band(0)->gain_db == 30.0);
    peq_set_preamp_db(-300.0);
    assert(peq_get_preamp_db() == -30.0);
    take_published();
    state[0][0].x1 = 123.0f;
    peq_set_preamp_db(0.0);
    take_published();
    assert(state[0][0].x1 == 123.0f);
    chain_t chain = { .channels = 1, .preamp = 1.0f, .eq_off = true };
    float sample = 1.0f;
    chain_chunk(&chain, &sample, 1);
    assert(state[0][0].x1 == 123.0f);
    sample = NAN;
    chain_chunk(&chain, &sample, 1);
    assert(sample == 0.0f && state[0][0].x1 == 0.0f);
    double parsed;
    assert(!parse_finite("nan", &parsed));
    assert(!parse_finite("1junk", &parsed));
    assert(parse_finite("1.5\r", &parsed) && parsed == 1.5);
    test_combined_response();
    puts("PEQ finite samples, gain bounds and publish continuity passed");
}
