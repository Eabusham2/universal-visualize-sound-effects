/* Host-side unit test for fft.c: compares the radix-2 FFT against a naive
 * O(n^2) DFT on random data and checks a few analytic spectra. Runs on any
 * OS (no Win32) so CI can verify the DSP core natively.
 *
 *   make test
 */
#include "fft.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;

#define CHECK(cond, ...) do { \
        if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                       printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static void naive_dft(const float *re, const float *im, size_t n,
                      double *out_re, double *out_im) {
    for (size_t k = 0; k < n; ++k) {
        double sr = 0.0, si = 0.0;
        for (size_t t = 0; t < n; ++t) {
            double a = -2.0 * M_PI * (double)k * (double)t / (double)n;
            double c = cos(a), s = sin(a);
            sr += re[t] * c - im[t] * s;
            si += re[t] * s + im[t] * c;
        }
        out_re[k] = sr;
        out_im[k] = si;
    }
}

static void test_against_dft(size_t n) {
    FFT f;
    CHECK(fft_init(&f, n) == 0, "fft_init(%zu)", n);

    float  *re = malloc(n * sizeof(float)), *im = malloc(n * sizeof(float));
    float  *re0 = malloc(n * sizeof(float)), *im0 = malloc(n * sizeof(float));
    double *dre = malloc(n * sizeof(double)), *dim = malloc(n * sizeof(double));
    srand(12345u + (unsigned)n);
    for (size_t i = 0; i < n; ++i) {
        re0[i] = re[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
        im0[i] = im[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    }
    naive_dft(re0, im0, n, dre, dim);
    fft_forward(&f, re, im);

    double max_err = 0.0, max_mag = 0.0;
    for (size_t k = 0; k < n; ++k) {
        double e = hypot(re[k] - dre[k], im[k] - dim[k]);
        if (e > max_err) max_err = e;
        double m = hypot(dre[k], dim[k]);
        if (m > max_mag) max_mag = m;
    }
    /* float32 butterflies accumulate ~1e-5 relative error per stage */
    CHECK(max_err <= 1e-4 * (max_mag > 1.0 ? max_mag : 1.0),
          "n=%zu max abs error %g (max magnitude %g)", n, max_err, max_mag);

    free(re); free(im); free(re0); free(im0); free(dre); free(dim);
    fft_free(&f);
}

static void test_pure_tone(void) {
    const size_t n = 2048;
    const size_t bin = 100;            /* 100 * 48000/2048 = 2343.75 Hz */
    FFT f;
    CHECK(fft_init(&f, n) == 0, "fft_init");
    float *in = malloc(n * sizeof(float));
    float *sre = malloc(n * sizeof(float)), *sim = malloc(n * sizeof(float));
    float *mag = malloc((n / 2 + 1) * sizeof(float));
    for (size_t i = 0; i < n; ++i)
        in[i] = (float)cos(2.0 * M_PI * (double)bin * (double)i / (double)n);

    fft_magnitude_real(&f, in, sre, sim, mag);

    /* A unit cosine exactly on a bin has magnitude n/2 there and ~0 elsewhere. */
    CHECK(fabs(mag[bin] - n / 2.0) < 1e-2 * n, "tone bin magnitude %g", mag[bin]);
    size_t peak = 0;
    for (size_t k = 1; k <= n / 2; ++k) if (mag[k] > mag[peak]) peak = k;
    CHECK(peak == bin, "peak bin %zu, expected %zu", peak, bin);
    double leak = 0.0;
    for (size_t k = 0; k <= n / 2; ++k) if (k != bin) leak += mag[k];
    CHECK(leak < 1e-3 * n, "leakage outside the tone bin: %g", leak);

    free(in); free(sre); free(sim); free(mag);
    fft_free(&f);
}

static void test_dc_and_invalid(void) {
    FFT f;
    CHECK(fft_init(&f, 3) != 0, "non power of two must be rejected");
    CHECK(fft_init(&f, 1) != 0, "n=1 must be rejected");
    CHECK(fft_init(NULL, 8) != 0, "NULL fft must be rejected");

    CHECK(fft_init(&f, 8) == 0, "fft_init(8)");
    float re[8], im[8];
    for (int i = 0; i < 8; ++i) { re[i] = 1.0f; im[i] = 0.0f; }
    fft_forward(&f, re, im);
    CHECK(fabs(re[0] - 8.0) < 1e-5 && fabs(im[0]) < 1e-5, "DC bin of ones: %g,%g", re[0], im[0]);
    for (int k = 1; k < 8; ++k)
        CHECK(fabs(re[k]) < 1e-5 && fabs(im[k]) < 1e-5, "bin %d of DC input: %g,%g", k, re[k], im[k]);
    fft_free(&f);
    CHECK(f.n == 0 && f.bitrev == NULL, "fft_free must reset the struct");
    fft_free(&f);   /* double free must be safe */
}

int main(void) {
    test_dc_and_invalid();
    test_pure_tone();
    test_against_dft(2);
    test_against_dft(16);
    test_against_dft(256);
    test_against_dft(2048);
    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("fft_test: all checks passed\n");
    return 0;
}
