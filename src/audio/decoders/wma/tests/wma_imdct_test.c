/* Host test: the FFT-based wma_imdct against the direct O(N^2) synthesis.
 * Build: gcc -O2 -I src -o wma_imdct_test src/audio/decoders/wma/wma_imdct_test.c \
 *        src/audio/decoders/asf/asf_demux.c -lm -lpthread */
#include <stdio.h>
#include <stdlib.h>
#include "../wma_decoder.c"

/* Reference: direct type-IV synthesis (the previous implementation). */
static void ref_imdct(const float * coefs, float * out, int block_len) {
    int M = block_len;
    double wdel = M_PI / (4.0 * M);
    for (int n = 0; n < 2 * M; n++) {
        double base = wdel * (1.0 + M) + 2.0 * wdel * n;
        double sum = 0.0;
        for (int k = 0; k < M; k++) sum += coefs[k] * cos(base * (2 * k + 1));
        out[n] = (float) -sum;
    }
}

int main(void) {
    static float in[BLOCK_MAX_SIZE], a[2 * BLOCK_MAX_SIZE], b[2 * BLOCK_MAX_SIZE];
    int fail = 0;
    srand(1234);
    for (int m = 1 << BLOCK_MIN_BITS; m <= BLOCK_MAX_SIZE; m <<= 1) {
        imdct_tab_t t = {0};
        if (imdct_tab_init(&t, m) < 0) return 2;
        for (int i = 0; i < m; i++) in[i] = (float) rand() / RAND_MAX * 2.0f - 1.0f;
        ref_imdct(in, a, m);
        wma_imdct(&t, in, b);
        double peak = 0, err = 0;
        for (int i = 0; i < 2 * m; i++) {
            if (fabs(a[i]) > peak) peak = fabs(a[i]);
            if (fabs(a[i] - b[i]) > err) err = fabs(a[i] - b[i]);
        }
        printf("block %4d: peak %.3f max err %.3g rel %.3g\n", m, peak, err, err / peak);
        if (err / peak >= 1e-4) fail = 1;
        imdct_tab_free(&t);
    }
    puts(fail ? "FAIL" : "PASS");
    return fail;
}
