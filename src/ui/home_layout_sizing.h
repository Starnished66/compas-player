#ifndef HOME_LAYOUT_SIZING_H
#define HOME_LAYOUT_SIZING_H

#include <stdint.h>

/* Scale a theme's 480px-reference Home measurements for the active panel. */
static inline int32_t home_layout_scale_px(int32_t value, int32_t display_width) {
    if (value < 0 || display_width <= 0) return value;
    int64_t scaled = ((int64_t)value * display_width + 240) / 480;
    return scaled > INT32_MAX ? INT32_MAX : (int32_t)scaled;
}

static inline int32_t home_layout_scale_py(int32_t value, int32_t display_height) {
    if (value < 0 || display_height <= 0) return value;
    int64_t scaled = ((int64_t)value * display_height + 400) / 800;
    return scaled > INT32_MAX ? INT32_MAX : (int32_t)scaled;
}

/* Fit preferred row heights into a Home viewport while retaining their
 * authored proportions and never crossing the font/touch minimum. If the
 * minima do not fit, return them unchanged so the list remains scrollable. */
static inline int32_t home_layout_fit_row_heights(const int32_t * preferred,
                                                   const int32_t * minimum,
                                                   int32_t count,
                                                   int32_t available,
                                                   int32_t maximum,
                                                   int32_t * out) {
    if (!preferred || !minimum || !out || count <= 0) return 0;
    if (maximum < 1) maximum = 1;

    int64_t minimum_total = 0;
    for (int32_t i = 0; i < count; ++i) {
        int32_t min_h = minimum[i] > maximum ? maximum : minimum[i];
        if (min_h < 1) min_h = 1;
        out[i] = min_h;
        minimum_total += min_h;
    }

    if (available <= 0 || minimum_total > available) return (int32_t)minimum_total;
    int32_t budget = available;
    int64_t maximum_total = (int64_t)maximum * count;
    if (budget > maximum_total) budget = (int32_t)maximum_total;

    /* Binary-search one common scale factor, with each row floored at its
     * own minimum and capped at the builder's existing maximum. */
    double low = 0.0;
    double high = (double)maximum;
    for (int iteration = 0; iteration < 40; ++iteration) {
        double factor = (low + high) * 0.5;
        int64_t total = 0;
        for (int32_t i = 0; i < count; ++i) {
            int32_t min_h = minimum[i] > maximum ? maximum : minimum[i];
            if (min_h < 1) min_h = 1;
            int32_t pref = preferred[i] > 0 ? preferred[i] : min_h;
            double scaled = (double)pref * factor;
            int32_t h = scaled >= maximum ? maximum : (int32_t)(scaled + 0.5);
            if (h < min_h) h = min_h;
            total += h;
        }
        if (total <= budget) low = factor;
        else high = factor;
    }

    int32_t total = 0;
    for (int32_t i = 0; i < count; ++i) {
        int32_t min_h = minimum[i] > maximum ? maximum : minimum[i];
        if (min_h < 1) min_h = 1;
        int32_t pref = preferred[i] > 0 ? preferred[i] : min_h;
        double scaled = (double)pref * low;
        int32_t h = scaled >= maximum ? maximum : (int32_t)(scaled + 0.5);
        if (h < min_h) h = min_h;
        out[i] = h;
        total += h;
    }

    /* Integer rounding can leave a few pixels unused. Add them in order;
     * this does not disturb the relative sizing by more than one pixel. */
    while (total < budget) {
        for (int32_t i = 0; total < budget && i < count; ++i) {
            if (out[i] < maximum) {
                ++out[i];
                ++total;
            }
        }
    }
    return total;
}

#endif
