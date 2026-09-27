/* Host tests for the DSD decoder: synthetic DSF/DFF tones from a
 * second-order delta-sigma modulator, checked for level, in-band noise,
 * bit order, container equivalence, seek continuity, the 24-bit path, the
 * supported rates, malformed headers, and 64-bit file offsets (a sparse
 * file past 4 GiB; run the MIPS build under qemu to cover 32-bit long).
 *
 * `make dsd-decoder-selftest` */
#include "dsd_decoder.h"
#include "dsd_filter.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TONE_HZ 1000.0
#define TONE_AMP 0.5 /* SACD 0 dB */

static void put_le(FILE * f, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) fputc((int) ((v >> (8 * i)) & 0xff), f);
}

static void put_be(FILE * f, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; i--) fputc((int) ((v >> (8 * i)) & 0xff), f);
}

static uint8_t reverse_bits(uint8_t v) {
    uint8_t r = 0;
    for (int b = 0; b < 8; b++) if (v & (1 << b)) r |= (uint8_t) (0x80 >> b);
    return r;
}

/* Two channels of modulated tone, MSB first in time. Channel 1 is inverted
 * so a channel mix-up shows. Returns bytes per channel. */
static uint8_t * modulate_tone(unsigned int rate, double seconds, double hz, double amp, size_t * out_bytes) {
    size_t bytes = (size_t) (rate * seconds) / 8;
    uint8_t * data = calloc(2, bytes);
    double i1[2] = {0}, i2[2] = {0}, y[2] = {1, 1};
    for (size_t n = 0; n < bytes * 8; n++) {
        double x = amp * sin(2.0 * M_PI * hz * (double) n / rate);
        for (int c = 0; c < 2; c++) {
            double in = c ? -x : x;
            i1[c] += in - y[c];
            i2[c] += i1[c] - y[c];
            y[c] = i2[c] >= 0 ? 1 : -1;
            if (y[c] > 0) data[c * bytes + n / 8] |= (uint8_t) (0x80 >> (n % 8));
        }
    }
    *out_bytes = bytes;
    return data;
}

static uint8_t * modulate(unsigned int rate, double seconds, size_t * out_bytes) {
    return modulate_tone(rate, seconds, TONE_HZ, TONE_AMP, out_bytes);
}

/* Independent reference: the documented Kaiser design (448 taps per DSD64
 * multiple, beta 8.6, cutoff at the base rate) convolved bit by bit in
 * double precision, one output per multiple/2 bits, history starting as
 * DSD silence. The decoder's byte tables must reproduce it. */
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; term > 1e-12 * sum; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

static double * reference_decode(const uint8_t * bits_msb, size_t bytes, unsigned int multiple, unsigned int base,
                                 uint64_t * out_frames) {
    int n_taps = 448 * (int) (multiple / 64), m = n_taps - 1;
    double * taps = malloc(sizeof(double) * n_taps), sum = 0, c = 2.0 * base / ((double) multiple * base);
    for (int n = 0; n < n_taps; n++) {
        double x = n - m / 2.0, r = 2.0 * n / m - 1.0;
        taps[n] = (x == 0 ? c : sin(M_PI * c * x) / (M_PI * x)) * bessel_i0(8.6 * sqrt(1 - r * r)) / bessel_i0(8.6);
        sum += taps[n];
    }
    for (int n = 0; n < n_taps; n++) taps[n] /= sum;
    size_t bits_total = bytes * 8, step = multiple / 2;
    uint64_t frames = bits_total / step;
    double * out = malloc(sizeof(double) * frames);
    for (uint64_t f = 0; f < frames; f++) {
        /* The newest bit in the window is the last bit of output f. */
        long newest = (long) ((f + 1) * step) - 1;
        double acc = 0;
        for (int k = 0; k < n_taps; k++) {
            long bit = newest - (n_taps - 1) + k;
            int value;
            if (bit < 0) value = (0x69 >> (7 - (int) ((bit % 8 + 8) % 8))) & 1; /* silence prefill */
            else value = (bits_msb[bit / 8] >> (7 - bit % 8)) & 1;
            acc += taps[k] * (value ? 1.0 : -1.0);
        }
        out[f] = acc;
    }
    free(taps);
    *out_frames = frames;
    return out;
}

/* Amplitude of `hz` on channel `ch` over whole cycles after the start-up
 * transient, from 24-bit output. */
static double tone_amplitude(const int32_t * pcm, uint64_t frames, unsigned int rate, int ch, double hz) {
    uint64_t start = rate / 10;
    uint64_t n = (uint64_t) ((double) (frames - start) * hz / rate) * rate / (uint64_t) hz;
    double re = 0, im = 0;
    for (uint64_t i = 0; i < n; i++) {
        double x = pcm[(start + i) * 2 + ch] / 8388607.0;
        re += x * cos(2 * M_PI * hz * (double) i / rate);
        im += x * sin(2 * M_PI * hz * (double) i / rate);
    }
    return 2.0 * hypot(re, im) / n;
}

static int32_t * decode_all_wide(const char * path, uint64_t * frames, unsigned int * pcm_rate) {
    dsd_decoder_t * dec = dsd_open_file(path);
    assert(dec);
    uint64_t total = dsd_get_total_pcm_frame_count(dec), got = 0;
    int32_t * pcm = malloc(sizeof(int32_t) * 2 * (size_t) (total + 1));
    decoder_read_result_t r;
    while ((r = dsd_read_pcm_frames_s32(dec, 1000, pcm + got * 2)).frames) got += r.frames;
    assert(got == total);
    *frames = got;
    *pcm_rate = dsd_get_pcm_sample_rate(dec);
    dsd_close(dec);
    return pcm;
}

static void write_dsf(const char * path, unsigned int rate, const uint8_t * data, size_t bytes, bool lsb_first,
                      uint32_t block, uint32_t bits_field) {
    size_t blocks = (bytes + block - 1) / block;
    uint64_t data_size = (uint64_t) blocks * block * 2;
    FILE * f = fopen(path, "wb");
    assert(f);
    fwrite("DSD ", 1, 4, f); put_le(f, 28, 8); put_le(f, 28 + 52 + 12 + data_size, 8); put_le(f, 0, 8);
    fwrite("fmt ", 1, 4, f); put_le(f, 52, 8); put_le(f, 1, 4); put_le(f, 0, 4); put_le(f, 2, 4); put_le(f, 2, 4);
    put_le(f, rate, 4); put_le(f, bits_field, 4); put_le(f, (uint64_t) bytes * 8, 8); put_le(f, block, 4); put_le(f, 0, 4);
    fwrite("data", 1, 4, f); put_le(f, 12 + data_size, 8);
    for (size_t b = 0; b < blocks; b++)
        for (int c = 0; c < 2; c++)
            for (uint32_t i = 0; i < block; i++) {
                size_t at = b * block + i;
                uint8_t v = at < bytes ? data[c * bytes + at] : 0;
                fputc(lsb_first ? reverse_bits(v) : v, f);
            }
    fclose(f);
}

static void write_dff(const char * path, unsigned int rate, const uint8_t * data, size_t bytes, const char * cmpr) {
    FILE * f = fopen(path, "wb");
    assert(f);
    uint64_t prop = 4 + (12 + 4) + (12 + 2 + 8) + (12 + 4 + 1 + 15);
    uint64_t sound = (uint64_t) bytes * 2;
    fwrite("FRM8", 1, 4, f); put_be(f, 4 + (12 + 4) + (12 + prop) + (12 + sound), 8); fwrite("DSD ", 1, 4, f);
    fwrite("FVER", 1, 4, f); put_be(f, 4, 8); put_be(f, 0x01050000, 4);
    fwrite("PROP", 1, 4, f); put_be(f, prop, 8); fwrite("SND ", 1, 4, f);
    fwrite("FS  ", 1, 4, f); put_be(f, 4, 8); put_be(f, rate, 4);
    fwrite("CHNL", 1, 4, f); put_be(f, 10, 8); put_be(f, 2, 2); fwrite("SLFTSRGT", 1, 8, f);
    fwrite("CMPR", 1, 4, f); put_be(f, 20, 8); fwrite(cmpr, 1, 4, f); fputc(15, f); fwrite("not compressed ", 1, 15, f);
    fwrite("DSD ", 1, 4, f); put_be(f, sound, 8);
    for (size_t i = 0; i < bytes; i++) { fputc(data[i], f); fputc(data[bytes + i], f); }
    fclose(f);
}

static int16_t * decode_all(const char * path, uint64_t * frames, unsigned int * pcm_rate) {
    dsd_decoder_t * dec = dsd_open_file(path);
    assert(dec);
    assert(dsd_get_channels(dec) == 2);
    uint64_t total = dsd_get_total_pcm_frame_count(dec);
    int16_t * pcm = malloc(sizeof(int16_t) * 2 * (size_t) (total + 1));
    uint64_t got = 0;
    for (;;) {
        decoder_read_result_t r = dsd_read_pcm_frames_s16(dec, 1000, pcm + got * 2);
        if (r.frames == 0) { assert(r.status == DECODER_READ_EOF); break; }
        got += r.frames;
    }
    assert(got == total);
    *frames = got;
    if (pcm_rate) *pcm_rate = dsd_get_pcm_sample_rate(dec);
    dsd_close(dec);
    return pcm;
}

/* Fits the tone on channel `ch` over whole cycles after the start-up
 * transient; returns its amplitude and the residual's RMS (both 0..1). */
static void analyze(const int16_t * pcm, uint64_t frames, unsigned int rate, int ch, double * amp, double * residual) {
    uint64_t start = rate / 10;
    uint64_t cycles = (frames - start) * (uint64_t) TONE_HZ / rate;
    uint64_t n = cycles * rate / (uint64_t) TONE_HZ;
    double re = 0, im = 0;
    for (uint64_t i = 0; i < n; i++) {
        double x = pcm[(start + i) * 2 + ch] / 32767.0;
        re += x * cos(2 * M_PI * TONE_HZ * (double) i / rate);
        im += x * sin(2 * M_PI * TONE_HZ * (double) i / rate);
    }
    re *= 2.0 / n;
    im *= 2.0 / n;
    double err = 0;
    for (uint64_t i = 0; i < n; i++) {
        double x = pcm[(start + i) * 2 + ch] / 32767.0;
        double fit = re * cos(2 * M_PI * TONE_HZ * (double) i / rate) + im * sin(2 * M_PI * TONE_HZ * (double) i / rate);
        err += (x - fit) * (x - fit);
    }
    *amp = hypot(re, im);
    *residual = sqrt(err / n);
}

static bool open_fails(const char * path) {
    dsd_decoder_t * dec = dsd_open_file(path);
    if (dec) dsd_close(dec);
    return dec == NULL;
}

static void patch_u32(const char * path, long offset, uint32_t value) {
    FILE * f = fopen(path, "r+b");
    assert(f && fseek(f, offset, SEEK_SET) == 0);
    put_le(f, value, 4);
    fclose(f);
}

int main(void) {
    char dir[] = "/tmp/compas-dsd-test-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);

    size_t bytes;
    uint8_t * tone = modulate(2822400, 1.0, &bytes);

    /* Level and in-band noise, both channels, and the output rate. */
    write_dsf("lsb.dsf", 2822400, tone, bytes, true, 4096, 1);
    uint64_t frames;
    unsigned int rate;
    int16_t * lsb = decode_all("lsb.dsf", &frames, &rate);
    assert(rate == 88200 && frames == (uint64_t) bytes / 4);
    for (int ch = 0; ch < 2; ch++) {
        double amp, residual;
        analyze(lsb, frames, rate, ch, &amp, &residual);
        /* The residual is dominated by this simple 2nd-order modulator's own
         * noise across the 0-44.1 kHz output band, not by the decoder. */
        printf("DSD64 ch%d: tone %.3f dBFS (ideal %.3f), residual %.1f dB below the tone\n", ch,
               20 * log10(amp), 20 * log10(TONE_AMP), 20 * log10(amp / residual));
        assert(fabs(20 * log10(amp / TONE_AMP)) < 0.05);
        assert(20 * log10(amp / residual) > 55.0);
    }

    /* Bit-exact structure: the byte-table decimator matches the reference
     * convolution to float rounding on the 24-bit path. */
    {
        uint64_t ref_frames, wide_frames;
        unsigned int wide_rate;
        double * ref = reference_decode(tone, bytes, 64, 44100, &ref_frames);
        int32_t * got = decode_all_wide("lsb.dsf", &wide_frames, &wide_rate);
        assert(ref_frames == wide_frames);
        double worst = 0;
        for (uint64_t i = 0; i < ref_frames; i++) {
            double d = fabs(got[i * 2] / 8388607.0 - ref[i]);
            if (d > worst) worst = d;
        }
        printf("DSD64 vs reference convolution: worst difference %.2e (24-bit LSB %.2e)\n", worst, 1.0 / 8388607);
        assert(worst < 4.0 / 8388607);
        free(ref);
        free(got);
    }

    /* Anti-aliasing: an 80 kHz ultrasonic tone would fold to 8.2 kHz at
     * 88.2 kHz output; the filter must remove it (design: >= 88 dB). */
    {
        size_t hf_bytes;
        uint8_t * hf = modulate_tone(2822400, 1.0, 80000.0, 0.25, &hf_bytes);
        write_dsf("hf.dsf", 2822400, hf, hf_bytes, true, 4096, 1);
        uint64_t hf_frames;
        unsigned int hf_rate;
        int32_t * pcm = decode_all_wide("hf.dsf", &hf_frames, &hf_rate);
        double alias = tone_amplitude(pcm, hf_frames, hf_rate, 0, 88200.0 - 80000.0);
        printf("80 kHz tone alias at 8.2 kHz: %.1f dB below the tone\n", 20 * log10(0.25 / (alias + 1e-12)));
        assert(20 * log10(0.25 / (alias + 1e-12)) > 85.0);
        free(pcm);
        free(hf);
    }
    /* Channel 1 carries the inverted tone. */
    assert(lsb[(frames / 2) * 2] == -lsb[(frames / 2) * 2 + 1] || abs(lsb[(frames / 2) * 2] + lsb[(frames / 2) * 2 + 1]) < 64);

    /* MSB-first DSF and DFF carry the same bits and must decode identically. */
    write_dsf("msb.dsf", 2822400, tone, bytes, false, 4096, 8);
    write_dff("tone.dff", 2822400, tone, bytes, "DSD ");
    uint64_t frames_msb, frames_dff;
    int16_t * msb = decode_all("msb.dsf", &frames_msb, NULL);
    int16_t * dff = decode_all("tone.dff", &frames_dff, NULL);
    assert(frames_msb == frames && frames_dff == frames);
    assert(memcmp(lsb, msb, frames * 4) == 0 && memcmp(lsb, dff, frames * 4) == 0);

    /* Seeking reproduces continuous playback exactly, across a block edge,
     * near the start, and at the end. */
    const char * seek_files[] = { "lsb.dsf", "tone.dff" };
    uint64_t targets[] = { 0, 3, 1021, 1024, 30001, frames - 5, frames };
    for (int file = 0; file < 2; file++) {
        dsd_decoder_t * dec = dsd_open_file(seek_files[file]);
        assert(dec);
        for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
            int16_t chunk[2 * 64];
            assert(dsd_seek_to_pcm_frame(dec, targets[t]));
            decoder_read_result_t r = dsd_read_pcm_frames_s16(dec, 64, chunk);
            uint64_t expect = frames - targets[t] < 64 ? frames - targets[t] : 64;
            assert(r.frames == expect);
            if (expect) assert(memcmp(chunk, lsb + targets[t] * 2, expect * 4) == 0);
            else assert(r.status == DECODER_READ_EOF);
        }
        dsd_close(dec);
    }

    /* 24-bit path matches the 16-bit path to within rounding. */
    dsd_decoder_t * dec = dsd_open_file("lsb.dsf");
    int32_t * wide = malloc(sizeof(int32_t) * 2 * (size_t) frames);
    uint64_t wide_frames = 0;
    for (;;) {
        decoder_read_result_t r = dsd_read_pcm_frames_s32(dec, 777, wide + wide_frames * 2);
        if (!r.frames) break;
        wide_frames += r.frames;
    }
    assert(wide_frames == frames);
    for (uint64_t i = 0; i < frames * 2; i++) {
        assert(wide[i] >= -8388608 && wide[i] <= 8388607);
        assert(labs(lrint(wide[i] / 256.0) - lsb[i]) <= 1);
    }
    dsd_close(dec);
    free(wide);

    /* The other supported rates, including the 48 kHz family and DSD256. */
    struct { unsigned int dsd; unsigned int pcm; } rates[] = {
        { 5644800, 88200 }, { 11289600, 88200 }, { 3072000, 96000 }, { 6144000, 96000 } };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        size_t rate_bytes;
        uint8_t * data = modulate(rates[i].dsd, 0.25, &rate_bytes);
        write_dsf("rate.dsf", rates[i].dsd, data, rate_bytes, true, 4096, 1);
        uint64_t rate_frames;
        unsigned int pcm_rate;
        int16_t * pcm = decode_all("rate.dsf", &rate_frames, &pcm_rate);
        double amp, residual;
        analyze(pcm, rate_frames, pcm_rate, 0, &amp, &residual);
        printf("%u Hz DSD -> %u Hz: tone %.3f dBFS, residual %.1f dB below\n", rates[i].dsd, pcm_rate,
               20 * log10(amp), 20 * log10(amp / residual));
        assert(pcm_rate == rates[i].pcm && fabs(20 * log10(amp / TONE_AMP)) < 0.05 && 20 * log10(amp / residual) > 55.0);
        free(pcm);
        free(data);
    }

    /* Decoders at one rate share a single filter table. */
    const dsd_filter_design_t * a = dsd_filter_design_acquire(64, 44100);
    const dsd_filter_design_t * b = dsd_filter_design_acquire(64, 44100);
    assert(a && a == b && dsd_filter_groups(a) == 56 && dsd_filter_bytes_per_output(a) == 4);
    dsd_filter_design_release(a);
    dsd_filter_design_release(b);
    assert(!dsd_filter_design_acquire(512, 44100) && !dsd_filter_design_acquire(64, 32000));

    /* Malformed or unsupported files are refused, never crash. */
    write_dsf("bad.dsf", 2822400, tone, bytes, true, 4096, 1);
    patch_u32("bad.dsf", 28 + 12 + 32, 0);            /* block size 0 */
    assert(open_fails("bad.dsf"));
    write_dsf("bad.dsf", 2822400, tone, bytes, true, 4096, 2); /* bits per sample 2 */
    assert(open_fails("bad.dsf"));
    write_dsf("bad.dsf", 2822401, tone, bytes, true, 4096, 1); /* not a DSD rate */
    assert(open_fails("bad.dsf"));
    write_dsf("bad.dsf", 22579200, tone, bytes, true, 4096, 1); /* DSD512 */
    assert(open_fails("bad.dsf"));
    write_dff("dst.dff", 2822400, tone, bytes, "DST "); /* compressed DFF */
    assert(open_fails("dst.dff"));
    FILE * f = fopen("short.dsf", "wb");
    fwrite("DSD ", 1, 4, f);
    fclose(f);
    assert(open_fails("short.dsf") && open_fails("missing.dsf"));

    /* A file cut short of its declared length ends cleanly. */
    write_dsf("cut.dsf", 2822400, tone, bytes, true, 4096, 1);
    assert(truncate("cut.dsf", 28 + 52 + 12 + 2 * 4096 * 5 + 100) == 0);
    dec = dsd_open_file("cut.dsf");
    assert(dec && dsd_get_total_pcm_frame_count(dec) == 5 * 4096 / 4); /* real length known at open */
    int16_t chunk[2 * 4096];
    uint64_t cut_frames = 0;
    decoder_read_result_t r;
    while ((r = dsd_read_pcm_frames_s16(dec, 4096, chunk)).frames) cut_frames += r.frames;
    assert(r.status == DECODER_READ_EOF && cut_frames == 5 * 4096 / 4);
    dsd_close(dec);

    /* A truncated DFF also reports its real length at open. */
    write_dff("cut.dff", 2822400, tone, bytes, "DSD ");
    f = fopen("cut.dff", "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    long dff_size = ftell(f);
    fclose(f);
    assert(truncate("cut.dff", dff_size - (long) bytes) == 0); /* keep half the sound data */
    dec = dsd_open_file("cut.dff");
    assert(dec && dsd_get_total_pcm_frame_count(dec) == (uint64_t) bytes / 2 / 4);
    cut_frames = 0;
    while ((r = dsd_read_pcm_frames_s16(dec, 4096, chunk)).frames) cut_frames += r.frames;
    assert(r.status == DECODER_READ_EOF && cut_frames == (uint64_t) bytes / 2 / 4);
    dsd_close(dec);

    /* A data chunk declared shorter than its blocks, followed by metadata:
     * only the rows every channel really has are played, and the metadata
     * is never read as audio. Stereo, 4096-byte blocks, payload 4096 + 100
     * leaves 100 bytes (25 frames) of channel 1. */
    write_dsf("meta.dsf", 2822400, tone, bytes, true, 4096, 1);
    f = fopen("meta.dsf", "r+b");
    assert(f && fseek(f, 28 + 52 + 4, SEEK_SET) == 0);
    put_le(f, 12 + 4096 + 100, 8);
    fclose(f);
    dec = dsd_open_file("meta.dsf");
    assert(dec && dsd_get_total_pcm_frame_count(dec) == 25);
    cut_frames = 0;
    while ((r = dsd_read_pcm_frames_s16(dec, 4096, chunk)).frames) cut_frames += r.frames;
    assert(r.status == DECODER_READ_EOF && cut_frames == 25);
    assert(memcmp(chunk, lsb, 25 * 4) == 0);
    dsd_close(dec);

    /* 64-bit offsets: seek near the end of a sparse file past 4 GiB. */
    uint64_t huge_bytes = (5ull << 30) / 2; /* per channel */
    f = fopen("huge.dsf", "wb");
    fwrite("DSD ", 1, 4, f); put_le(f, 28, 8); put_le(f, 28 + 52 + 12 + huge_bytes * 2, 8); put_le(f, 0, 8);
    fwrite("fmt ", 1, 4, f); put_le(f, 52, 8); put_le(f, 1, 4); put_le(f, 0, 4); put_le(f, 2, 4); put_le(f, 2, 4);
    put_le(f, 2822400, 4); put_le(f, 1, 4); put_le(f, huge_bytes * 8, 8); put_le(f, 4096, 4); put_le(f, 0, 4);
    fwrite("data", 1, 4, f); put_le(f, 12 + huge_bytes * 2, 8);
    fclose(f);
    assert(truncate("huge.dsf", (off_t) (28 + 52 + 12 + huge_bytes * 2)) == 0);
    dec = dsd_open_file("huge.dsf");
    assert(dec && dsd_get_total_pcm_frame_count(dec) == huge_bytes / 4);
    assert(dsd_seek_to_pcm_frame(dec, huge_bytes / 4 - 10));
    r = dsd_read_pcm_frames_s16(dec, 64, chunk);
    assert(r.frames == 10);
    dsd_close(dec);
    unlink("huge.dsf");

    free(lsb);
    free(msb);
    free(dff);
    free(tone);
    printf("DSD decoder tests passed (%s)\n", dir);
    return 0;
}
