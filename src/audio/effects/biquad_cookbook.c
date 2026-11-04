#include "biquad_cookbook.h"

#include <math.h>

biquad_cookbook_t biquad_cookbook(biquad_cookbook_type_t type, double w0,
                                  double gain_db, double q) {
    const double A = pow(10.0, gain_db / 40.0);
    const double cos_w0 = cos(w0);
    const double alpha = sin(w0) / (2.0 * q);
    const double sqrt_A_2alpha = 2.0 * sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (type == BIQUAD_COOKBOOK_LOW_SHELF) {
        b0 = A * ((A + 1) - (A - 1) * cos_w0 + sqrt_A_2alpha);
        b1 = 2 * A * ((A - 1) - (A + 1) * cos_w0);
        b2 = A * ((A + 1) - (A - 1) * cos_w0 - sqrt_A_2alpha);
        a0 = (A + 1) + (A - 1) * cos_w0 + sqrt_A_2alpha;
        a1 = -2 * ((A - 1) + (A + 1) * cos_w0);
        a2 = (A + 1) + (A - 1) * cos_w0 - sqrt_A_2alpha;
    } else if (type == BIQUAD_COOKBOOK_HIGH_SHELF) {
        b0 = A * ((A + 1) + (A - 1) * cos_w0 + sqrt_A_2alpha);
        b1 = -2 * A * ((A - 1) + (A + 1) * cos_w0);
        b2 = A * ((A + 1) + (A - 1) * cos_w0 - sqrt_A_2alpha);
        a0 = (A + 1) - (A - 1) * cos_w0 + sqrt_A_2alpha;
        a1 = 2 * ((A - 1) - (A + 1) * cos_w0);
        a2 = (A + 1) - (A - 1) * cos_w0 - sqrt_A_2alpha;
    } else {
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cos_w0;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha / A;
    }
    return (biquad_cookbook_t){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}
