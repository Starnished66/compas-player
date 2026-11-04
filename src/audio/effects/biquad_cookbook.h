#ifndef BIQUAD_COOKBOOK_H
#define BIQUAD_COOKBOOK_H

typedef enum {
    BIQUAD_COOKBOOK_PEAK,
    BIQUAD_COOKBOOK_LOW_SHELF,
    BIQUAD_COOKBOOK_HIGH_SHELF
} biquad_cookbook_type_t;

/* Normalised (a0 == 1) direct form I coefficients. */
typedef struct { double b0, b1, b2, a1, a2; } biquad_cookbook_t;

/* RBJ audio-EQ-cookbook peak or shelf, with the shelf bandwidth given as Q.
 * w0 is the already-prewarped angular frequency in radians per sample: the
 * callers keep their own frequency clamping and constants. Pure double math. */
biquad_cookbook_t biquad_cookbook(biquad_cookbook_type_t type, double w0,
                                  double gain_db, double q);

#endif
