/*
 * Art Swap helper - snd0.at9 (at9.h): readying the audio and encoding it as
 * ATRAC9, on the console.
 *
 * The encoder works as the converter's does (its docs/encoder.md says more). A
 * decoder derives most of a frame from a few transmitted numbers: the scale
 * factors and the gradient parameters give every quantisation unit its
 * precision, and the scale factors choose the Huffman codebooks. The encoder
 * recomputes all of that with the decoder's own integer rules, so it knows the
 * exact size and the exact quantisation noise of every choice before it writes
 * anything. For each 256-sample frame it quantises each unit at every
 * precision, then tries about 1,300 gradient settings with every boundary and
 * keeps, for each frame size in bytes, the one with the least noise relative to
 * a masking threshold. The four frames of a superframe share its bytes, and a
 * small dynamic programme spends them where they lower the noise most. No
 * superframe refers to another (band parameters and scale-factor predictions
 * stay inside it), so each decodes on its own, and the loop back to the start
 * is clean.
 *
 * The numbers are the converter's, so the files decode the same way, but this
 * is not always bit for bit its output: numpy sums and transforms in other
 * orders, which moves the last bits of the noise figures and now and then
 * tips a choice between two options that are equally good. On music most
 * superframes come out byte-identical (helper/tests/test_at9.py counts them).
 *
 * Memory: the encoder analyses each superframe twice (once to choose the coded
 * bandwidth, which is smoothed across neighbours, once to encode), rather than
 * keeping a spectrum of the whole clip. Loudness filters in blocks, and the
 * limiter needs one more mono array the length of the clip.
 *
 * Ported from ps5-at9-converter's ps5at9/atrac9_encoder.py, atrac9.py,
 * at9file.py and dsp.py (Copyright (C) 2026 BlackBearReloaded,
 * GPL-3.0-or-later). The decoder rules follow LibAtrac9 by Alex Barney (MIT
 * License), whose tables are in at9_tables.c with its notice.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "at9.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The payload SDK's math.h classifies numbers through a _Generic that its clang warns about
 * (-Wunreachable-code-generic-assoc); the compiler's own builtins answer the same question inline */
#define is_finite(x) __builtin_isfinite(x)
#define is_nan(x) __builtin_isnan(x)

#define N 256                /* samples per frame, and MDCT coefficients */
#define UNITS 30             /* quantisation units per frame */
#define FRAMES 4             /* frames per superframe */
#define SUPERFRAME_SAMPLES (N * FRAMES)
#define HEADER_SIZE 168      /* RIFF, fmt, fact, smpl and the data chunk's own header */
#define D_MIN (-40)          /* the range of scale factor minus gradient the encoder models */
#define D_MAX 50
#define D_SPAN (D_MAX - D_MIN + 1)
#define P_CAP 17             /* precisions above 15 need fine bits, which are never chosen */
#define CELLS (P_CAP + 2)    /* precision 0..18: room to look one above P_CAP */
#define BAD_BITS (1 << 20)   /* the cost of a precision that cannot be coded: no option fits */

static const int QIDX[UNITS + 1] = {0,  2,  4,  6,  8,  10, 12, 14, 16,  20,  24,  28,  32,  40,  48, 56,
                                    64, 72, 80, 88, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256};
static const int QCNT[UNITS] = {2, 2, 2, 2, 2, 2, 2, 2, 4,  4,  4,  4,  8,  8,  8,
                                8, 8, 8, 8, 8, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
static const int CBI[UNITS] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
static const int BAND_UNITS[19] = {0, 4, 8, 10, 12, 13, 14, 15, 16, 18, 20, 21, 22, 23, 24, 25, 26, 28, 30};
static const uint8_t ATRAC9_GUID[16] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                                        0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};

/* ---- FFT (radix 2, in place) ---- */

typedef struct {
    double re, im;
} cplx_t;

typedef struct {
    int n;
    cplx_t *tw;    /* exp(-2 pi i k / n) for k < n / 2 */
    uint32_t *rev; /* bit-reversed index */
} fft_t;

static void fft_free(fft_t *f)
{
    free(f->tw);
    free(f->rev);
    memset(f, 0, sizeof(*f));
}

static bool fft_init(fft_t *f, int n)
{
    memset(f, 0, sizeof(*f));
    f->n = n;
    f->tw = malloc(sizeof(cplx_t) * (size_t)(n / 2));
    f->rev = malloc(sizeof(uint32_t) * (size_t)n);
    if (f->tw == NULL || f->rev == NULL) {
        fft_free(f);
        return false;
    }
    int bits = 0;
    while ((1 << bits) < n)
        bits++;
    /* Each twiddle straight from cos and sin, not by recurrence, so the large transforms keep full precision */
    for (int k = 0; k < n / 2; k++) {
        const double angle = -2.0 * M_PI * (double)k / (double)n;
        f->tw[k] = (cplx_t){cos(angle), sin(angle)};
    }
    for (int i = 0; i < n; i++) {
        uint32_t r = 0;
        for (int b = 0; b < bits; b++)
            if ((i >> b) & 1)
                r |= 1u << (bits - 1 - b);
        f->rev[i] = r;
    }
    return true;
}

/* The inverse is not scaled by 1/n */
static void fft_run(const fft_t *f, cplx_t *x, bool inverse)
{
    const int n = f->n;
    for (int i = 0; i < n; i++) {
        const int j = (int)f->rev[i];
        if (j > i) {
            const cplx_t t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }
    const double sign = inverse ? -1.0 : 1.0;
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; j++) {
                const double wr = f->tw[j * step].re, wi = sign * f->tw[j * step].im;
                cplx_t *a = &x[i + j], *b = &x[i + j + half];
                const double tr = b->re * wr - b->im * wi;
                const double ti = b->re * wi + b->im * wr;
                b->re = a->re - tr;
                b->im = a->im - ti;
                a->re += tr;
                a->im += ti;
            }
        }
    }
}

/* ---- windowed-sinc filters (resampling and true peak) ---- */

/* The modified Bessel function I0 by its power series: every term is positive,
 * so it converges without cancellation for the Kaiser window's arguments */
static double bessel_i0(double x)
{
    const double q = x * x / 4.0;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k < 1000; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term <= sum * 1e-17)
            break;
    }
    return sum;
}

typedef struct {
    int up, down, half; /* taps = 2 * half, at offsets -half + 1 .. half */
    double *h;          /* [up][2 * half]: one row per output phase */
} poly_t;

/* dsp.py's _kaiser_lowpass_phases: a Kaiser-windowed low pass for converting
 * rate_in by up/down, one normalised row of taps per fractional output position */
static bool kaiser_phases(poly_t *ph, int up, int down, double rate_in, double pass_hz, double stop_hz,
                          double atten_db, size_t max_bytes)
{
    memset(ph, 0, sizeof(*ph));
    const double beta = 0.1102 * (atten_db - 8.7);
    const double width = (stop_hz - pass_hz) / rate_in;
    const int taps = (int)ceil((atten_db - 8.0) / (2.285 * 2.0 * M_PI * width));
    int half = taps / 2 + 1;
    if (half < 8)
        half = 8;
    const size_t count = 2 * (size_t)half;
    if ((size_t)up * count * sizeof(double) > max_bytes)
        return false;
    ph->h = malloc((size_t)up * count * sizeof(double));
    if (ph->h == NULL)
        return false;
    ph->up = up;
    ph->down = down;
    ph->half = half;
    const double cutoff = (pass_hz + stop_hz) / 2.0 / rate_in;
    const double i0_beta = bessel_i0(beta);
    for (int p = 0; p < up; p++) {
        double *row = ph->h + (size_t)p * count;
        const double phase = (double)p / (double)up;
        double sum = 0.0;
        for (size_t j = 0; j < count; j++) {
            const double t = (double)((int)j - half + 1) - phase;
            const double x = 2.0 * cutoff * t;
            const double sinc = x == 0.0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            const double tt = t / half;
            double v = 1.0 - tt * tt;
            v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
            row[j] = 2.0 * cutoff * sinc * (bessel_i0(beta * sqrt(v)) / i0_beta);
            sum += row[j];
        }
        for (size_t j = 0; j < count; j++)
            row[j] /= sum;
    }
    return true;
}

/* ---- channels and resampling ---- */

static int gcd(int a, int b)
{
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* out[o] is the filtered input at o * down / up input samples, zero outside the input */
static void resample_channel(const double *x, size_t n_in, const poly_t *ph, double *out, size_t n_out)
{
    const int taps = 2 * ph->half;
    for (size_t o = 0; o < n_out; o++) {
        const uint64_t pos = (uint64_t)o * (uint64_t)ph->down;
        const int64_t base = (int64_t)(pos / (uint64_t)ph->up);
        const double *h = ph->h + (size_t)(pos % (uint64_t)ph->up) * (size_t)taps;
        const int64_t first = base - ph->half + 1;
        double acc = 0.0;
        if (first >= 0 && first + taps <= (int64_t)n_in) {
            const double *in = x + first;
            for (int j = 0; j < taps; j++)
                acc += in[j] * h[j];
        } else {
            for (int j = 0; j < taps; j++) {
                const int64_t at = first + j;
                if (at >= 0 && at < (int64_t)n_in)
                    acc += x[at] * h[j];
            }
        }
        out[o] = acc;
    }
}

bool at9_to_stereo_48k(const double *const *in, int channels, size_t frames, int rate, double **left,
                       double **right, size_t *out_frames, char *err, size_t errlen)
{
    *left = *right = NULL;
    *out_frames = 0;
    if (in == NULL || channels < 1) {
        snprintf(err, errlen, "the audio has no channels");
        return false;
    }
    if (frames == 0) {
        snprintf(err, errlen, "the audio is empty");
        return false;
    }
    if (rate < 1000 || rate > 768000) {
        snprintf(err, errlen, "a sample rate of %d Hz is not supported", rate);
        return false;
    }
    poly_t ph = {0};
    size_t n_out = frames;
    if (rate != AT9_RATE) {
        const int g = gcd(rate, AT9_RATE);
        const int up = AT9_RATE / g, down = rate / g;
        const double nyquist = (rate < AT9_RATE ? rate : AT9_RATE) / 2.0;
        const double pass_hz = 0.9 * nyquist < 20000.0 ? 0.9 * nyquist : 20000.0;
        /* A rate with little in common with 48 kHz needs a row of taps per output phase; past 64 MiB of them
         * the rate is refused rather than held up for */
        if (!kaiser_phases(&ph, up, down, rate, pass_hz, nyquist, 100.0, 64u << 20)) {
            snprintf(err, errlen, "a sample rate of %d Hz cannot be converted (no memory)", rate);
            return false;
        }
        n_out = (size_t)(((uint64_t)frames * (uint64_t)up + (uint64_t)down - 1) / (uint64_t)down);
    }
    double *l = malloc(n_out * sizeof(double));
    double *r = malloc(n_out * sizeof(double));
    double *mix = NULL;
    if (channels == 6 && rate != AT9_RATE)
        mix = malloc(frames * sizeof(double));
    if (l == NULL || r == NULL || (channels == 6 && rate != AT9_RATE && mix == NULL)) {
        free(l);
        free(r);
        free(mix);
        free(ph.h);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    const double k = 1.0 / sqrt(2.0);
    for (int side = 0; side < 2; side++) {
        double *out = side ? r : l;
        const double *src = channels == 1 ? in[0] : in[side];
        if (channels == 6) {
            /* FL FR FC LFE BL BR: each side takes its own front and back with the centre at -3 dB, the LFE
             * left out, scaled so a signal in all of them cannot grow */
            const double *front = in[side], *centre = in[2], *back = in[4 + side];
            double *dst = mix != NULL ? mix : out;
            for (size_t i = 0; i < frames; i++)
                dst[i] = (front[i] + k * centre[i] + k * back[i]) / (1.0 + 2.0 * k);
            src = dst;
        }
        if (rate == AT9_RATE) {
            if (src != out)
                memcpy(out, src, frames * sizeof(double));
        } else if (channels == 1 && side == 1) {
            memcpy(r, l, n_out * sizeof(double));
        } else {
            resample_channel(src, frames, &ph, out, n_out);
        }
    }
    free(mix);
    free(ph.h);
    *left = l;
    *right = r;
    *out_frames = n_out;
    return true;
}

/* ---- loudness (ITU-R BS.1770-4, as dsp.py) ---- */

#define K_TAPS 16384 /* the K-weighting filter's impulse response, cut where it is 200 dB down */

/* The 48 kHz K-weighting impulse response: the shelf's, then the high pass run
 * over it, which is the convolution dsp.py takes of the two responses */
static void k_impulse(double *h)
{
    static const double sb[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285};
    static const double sa[3] = {1.0, -1.69065929318241, 0.73248077421585};
    static const double hb[3] = {1.0, -2.0, 1.0};
    static const double ha[3] = {1.0, -1.99004745483398, 0.99007225036621};
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (int n = 0; n < K_TAPS; n++) {
        const double x0 = n == 0 ? 1.0 : 0.0;
        const double y0 = sb[0] * x0 + sb[1] * x1 + sb[2] * x2 - sa[1] * y1 - sa[2] * y2;
        h[n] = y0;
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
    }
    x1 = x2 = y1 = y2 = 0.0;
    for (int n = 0; n < K_TAPS; n++) {
        const double x0 = h[n];
        const double y0 = hb[0] * x0 + hb[1] * x1 + hb[2] * x2 - ha[1] * y1 - ha[2] * y2;
        h[n] = y0;
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
    }
}

static double lufs(double power)
{
    return -0.691 + 10.0 * log10(power);
}

double at9_integrated_loudness(const double *l, const double *r, size_t n)
{
    const size_t block = 19200, step = 4800; /* 400 ms blocks every 100 ms */
    if (n == 0)
        return -INFINITY;
    /* Overlap-add with both channels in one complex transform (the filter is real, so left stays in the real
     * part and right in the imaginary): one block of output is final per transform, the tail carries on */
    int fft_n = 1 << 15;
    while ((size_t)fft_n < n + K_TAPS - 1 && fft_n < (1 << 16))
        fft_n <<= 1;
    const size_t hop = (size_t)fft_n - K_TAPS + 1 < n ? (size_t)fft_n - K_TAPS : n;
    const size_t tail_n = (size_t)fft_n - hop;
    const size_t steps = (n + step - 1) / step;
    fft_t fft = {0};
    cplx_t *kh = malloc(sizeof(cplx_t) * (size_t)fft_n);
    cplx_t *z = malloc(sizeof(cplx_t) * (size_t)fft_n);
    cplx_t *tail = calloc(tail_n, sizeof(cplx_t));
    double *energy = calloc(2 * steps, sizeof(double)); /* squared K-weighted samples per 100 ms, per channel */
    double *taps = malloc(sizeof(double) * K_TAPS);
    double *zb = NULL;
    double result = NAN;
    if (kh == NULL || z == NULL || tail == NULL || energy == NULL || taps == NULL || !fft_init(&fft, fft_n))
        goto done;
    k_impulse(taps);
    for (int i = 0; i < fft_n; i++)
        kh[i] = (cplx_t){i < K_TAPS ? taps[i] : 0.0, 0.0};
    fft_run(&fft, kh, false);
    const double inv_n = 1.0 / fft_n;
    for (size_t start = 0; start < n; start += hop) {
        const size_t m = n - start < hop ? n - start : hop;
        for (size_t i = 0; i < (size_t)fft_n; i++)
            z[i] = i < m ? (cplx_t){l[start + i], r[start + i]} : (cplx_t){0.0, 0.0};
        fft_run(&fft, z, false);
        for (int i = 0; i < fft_n; i++) {
            const double re = z[i].re * kh[i].re - z[i].im * kh[i].im;
            const double im = z[i].re * kh[i].im + z[i].im * kh[i].re;
            z[i] = (cplx_t){re, im};
        }
        fft_run(&fft, z, true);
        for (size_t i = 0; i < (size_t)fft_n; i++) {
            z[i].re *= inv_n;
            z[i].im *= inv_n;
            if (i < tail_n) {
                z[i].re += tail[i].re;
                z[i].im += tail[i].im;
            }
        }
        for (size_t i = 0; i < m; i++) {
            const size_t s = (start + i) / step;
            energy[2 * s] += z[i].re * z[i].re;
            energy[2 * s + 1] += z[i].im * z[i].im;
        }
        for (size_t i = 0; i < tail_n; i++)
            tail[i] = z[hop + i];
    }
    if (n < block) {
        /* Shorter than one block: the mean square of what there is */
        double sl = 0.0, sr = 0.0;
        for (size_t s = 0; s < steps; s++) {
            sl += energy[2 * s];
            sr += energy[2 * s + 1];
        }
        const double power = sl / (double)n + sr / (double)n;
        result = power > 0.0 ? lufs(power) : -INFINITY;
        goto done;
    }
    const size_t blocks = (n - block) / step + 1;
    zb = malloc(blocks * sizeof(double));
    if (zb == NULL)
        goto done;
    double sum = 0.0;
    size_t kept = 0;
    for (size_t b = 0; b < blocks; b++) {
        double el = 0.0, er = 0.0;
        for (int q = 0; q < 4; q++) {
            el += energy[2 * (b + q)];
            er += energy[2 * (b + q) + 1];
        }
        zb[b] = el / (double)block + er / (double)block;
        if (lufs(zb[b]) > -70.0) {
            sum += zb[b];
            kept++;
        }
    }
    if (kept == 0) {
        result = -INFINITY;
        goto done;
    }
    const double relative = lufs(sum / (double)kept) - 10.0;
    sum = 0.0;
    kept = 0;
    for (size_t b = 0; b < blocks; b++) {
        const double lk = lufs(zb[b]);
        if (lk > -70.0 && lk > relative) {
            sum += zb[b];
            kept++;
        }
    }
    result = lufs(sum / (double)kept);
done:
    fft_free(&fft);
    free(kh);
    free(z);
    free(tail);
    free(energy);
    free(taps);
    free(zb);
    return result;
}

/* ---- true peak and the limiter ---- */

/* The true peak of each sample: the largest magnitude of the four points a 4x
 * oversampling filter puts at and after it, the louder channel's */
static void peak_envelope(const double *l, const double *r, size_t n, const poly_t *ph, double *env)
{
    const int taps = 2 * ph->half;
    double ht[64][4]; /* the taps across the four phases, so one input sample feeds all four at once */
    for (int j = 0; j < taps; j++)
        for (int p = 0; p < 4; p++)
            ht[j][p] = ph->h[(size_t)p * (size_t)taps + (size_t)j];
    for (size_t i = 0; i < n; i++) {
        const int64_t first = (int64_t)i - ph->half + 1;
        double peak = 0.0;
        for (int c = 0; c < 2; c++) {
            const double *x = c ? r : l;
            double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
            if (first >= 0 && first + taps <= (int64_t)n) {
                const double *in = x + first;
                for (int j = 0; j < taps; j++) {
                    a0 += in[j] * ht[j][0];
                    a1 += in[j] * ht[j][1];
                    a2 += in[j] * ht[j][2];
                    a3 += in[j] * ht[j][3];
                }
            } else {
                for (int j = 0; j < taps; j++) {
                    const int64_t at = first + j;
                    if (at < 0 || at >= (int64_t)n)
                        continue;
                    a0 += x[at] * ht[j][0];
                    a1 += x[at] * ht[j][1];
                    a2 += x[at] * ht[j][2];
                    a3 += x[at] * ht[j][3];
                }
            }
            peak = fmax(peak, fmax(fmax(fabs(a0), fabs(a1)), fmax(fabs(a2), fabs(a3))));
        }
        env[i] = peak;
    }
}

static double array_max(const double *x, size_t n)
{
    double m = 0.0;
    for (size_t i = 0; i < n; i++)
        if (x[i] > m)
            m = x[i];
    return m;
}

/* In place: the minimum over [i - width, i + width], the ends repeated beyond the
 * array. A queue of rising minima holds what the window still needs, so values
 * are written over as soon as nothing ahead reads them. */
static bool sliding_min(double *x, size_t n, int width)
{
    if (width <= 0 || n == 0)
        return true;
    const size_t w = 2 * (size_t)width + 1, cap = w + 1;
    double *qv = malloc(cap * sizeof(double));
    size_t *qi = malloc(cap * sizeof(size_t));
    if (qv == NULL || qi == NULL) {
        free(qv);
        free(qi);
        return false;
    }
    const double first = x[0], last = x[n - 1];
    size_t head = 0, count = 0;
    for (size_t j = 0; j < n + 2 * (size_t)width; j++) {
        const double v = j < (size_t)width ? first : j - (size_t)width < n ? x[j - (size_t)width] : last;
        while (count > 0 && qv[(head + count - 1) % cap] >= v)
            count--;
        qv[(head + count) % cap] = v;
        qi[(head + count) % cap] = j;
        count++;
        if (j >= 2 * (size_t)width) {
            const size_t i = j - 2 * (size_t)width;
            while (qi[head] < i) {
                head = (head + 1) % cap;
                count--;
            }
            x[i] = qv[head];
        }
    }
    free(qv);
    free(qi);
    return true;
}

/* In place: the mean over [i - width, i + width], the ends repeated, from a
 * running sum as numpy's cumsum makes it (only the last 2 * width + 2 sums are kept) */
static bool moving_average(double *x, size_t n, int width)
{
    if (width <= 0 || n == 0)
        return true;
    const size_t w = 2 * (size_t)width + 1, ring_n = w + 1;
    double *ring = malloc(ring_n * sizeof(double));
    if (ring == NULL)
        return false;
    const double first = x[0], last = x[n - 1];
    double sum = 0.0;
    size_t m = 0; /* sums made: ring holds c[m - w .. m], c[k] = the first k padded values added up */
    ring[0] = 0.0;
    for (size_t i = 0; i < n; i++) {
        while (m < i + w) {
            const size_t j = m; /* padded index of the value c[m + 1] adds */
            const double v = j < (size_t)width ? first : j - (size_t)width < n ? x[j - (size_t)width] : last;
            sum += v;
            m++;
            ring[m % ring_n] = sum;
        }
        x[i] = (ring[(i + w) % ring_n] - ring[i % ring_n]) / (double)w;
    }
    free(ring);
    return true;
}

/* dsp.py's limit_true_peak: a smooth look-ahead gain (2 ms attack, 50 ms release)
 * that brings every true peak under the ceiling, tightened by 0.05 dB a round if
 * the smoothing lets one through */
static bool limit_true_peak(double *l, double *r, size_t n, double ceiling_db)
{
    poly_t ph;
    if (!kaiser_phases(&ph, 4, 1, 48000.0, 18000.0, 24000.0, 60.0, 1u << 20))
        return false;
    double *env = malloc(n * sizeof(double));
    if (env == NULL) {
        free(ph.h);
        return false;
    }
    bool ok = true;
    peak_envelope(l, r, n, &ph, env);
    const double ceiling = pow(10.0, ceiling_db / 20.0);
    if (array_max(env, n) > ceiling) {
        double target = ceiling_db;
        int attack = (int)(0.002 * AT9_RATE), release = (int)(0.05 * AT9_RATE);
        attack = attack < 1 ? 1 : attack;
        release = release < 1 ? 1 : release;
        for (int round = 0; round < 8 && ok; round++) {
            const double want = pow(10.0, target / 20.0);
            for (size_t i = 0; i < n; i++) {
                const double need = want / (env[i] > 1e-12 ? env[i] : 1e-12);
                env[i] = need < 1.0 ? need : 1.0;
            }
            ok = sliding_min(env, n, attack) && moving_average(env, n, attack / 2) &&
                 sliding_min(env, n, release) && moving_average(env, n, release / 2);
            if (!ok)
                break;
            for (size_t i = 0; i < n; i++) {
                l[i] *= env[i];
                r[i] *= env[i];
            }
            peak_envelope(l, r, n, &ph, env);
            if (array_max(env, n) <= ceiling)
                break;
            target -= 0.05;
        }
    }
    free(env);
    free(ph.h);
    return ok;
}

/* Raised-cosine fades, so the loop's end meets its start without a click */
static void apply_fades(double *l, double *r, size_t n, double fade_in, double fade_out)
{
    /* Python's round() rounds halves to even, as rint does */
    const double fin = is_finite(fade_in) && fade_in > 0.0 ? rint(fade_in * AT9_RATE) : 0.0;
    const double fout = is_finite(fade_out) && fade_out > 0.0 ? rint(fade_out * AT9_RATE) : 0.0;
    const size_t fi = fin >= (double)n ? n : (size_t)fin;
    const size_t fo = fout >= (double)n ? n : (size_t)fout;
    for (size_t i = 0; i < fi; i++) {
        const double g = 0.5 - 0.5 * cos(M_PI * ((double)i + 0.5) / (double)fi);
        l[i] *= g;
        r[i] *= g;
    }
    for (size_t i = 0; i < fo; i++) {
        const double g = 0.5 + 0.5 * cos(M_PI * ((double)i + 0.5) / (double)fo);
        l[n - fo + i] *= g;
        r[n - fo + i] *= g;
    }
}

bool at9_prepare(double *l, double *r, size_t n, double target_lufs, double ceiling_dbtp, double fade_in_s,
                 double fade_out_s, double *source_lufs, double *final_lufs)
{
    const double source = at9_integrated_loudness(l, r, n);
    if (source_lufs != NULL)
        *source_lufs = source;
    if (is_nan(source))
        return false;
    if (is_finite(source) && is_finite(target_lufs)) {
        const double gain = pow(10.0, (target_lufs - source) / 20.0);
        for (size_t i = 0; i < n; i++) {
            l[i] *= gain;
            r[i] *= gain;
        }
    }
    if (n > 0 && !limit_true_peak(l, r, n, ceiling_dbtp))
        return false;
    apply_fades(l, r, n, fade_in_s, fade_out_s);
    const double final = at9_integrated_loudness(l, r, n);
    if (final_lufs != NULL)
        *final_lufs = final;
    return !is_nan(final);
}

/* ---- sizes ---- */

/* Bytes per 256-sample frame at a stereo bit rate; 0 for one the format does not have */
static int frame_bytes_for(int bitrate_kbps)
{
    switch (bitrate_kbps) {
    case 72:
    case 96:
    case 120:
    case 144:
    case 168:
    case 192:
        return bitrate_kbps * 1000 * N / (8 * AT9_RATE);
    default:
        return 0;
    }
}

size_t at9_max_samples(int bitrate_kbps)
{
    const int frame_bytes = frame_bytes_for(bitrate_kbps);
    if (frame_bytes == 0)
        return 0;
    const size_t superframes = (AT9_SIZE_LIMIT - HEADER_SIZE) / ((size_t)frame_bytes * FRAMES);
    return superframes * SUPERFRAME_SAMPLES - AT9_ENCODER_DELAY;
}

size_t at9_file_size(size_t samples, int bitrate_kbps)
{
    const int frame_bytes = frame_bytes_for(bitrate_kbps);
    if (frame_bytes == 0)
        return 0;
    const size_t superframes = (samples + AT9_ENCODER_DELAY + SUPERFRAME_SAMPLES - 1) / SUPERFRAME_SAMPLES;
    return HEADER_SIZE + superframes * (size_t)frame_bytes * FRAMES;
}

/* ---- the decoder's integer rules (atrac9.py) ---- */

/* The 31-entry allocation gradient, exactly as the decoder derives it */
static void gradient_curve(int start_unit, int end_unit, int start_value, int end_value, int unit_count,
                           int grad[31])
{
    for (int i = 0; i < 31; i++)
        grad[i] = start_value;
    for (int i = end_unit; i <= (unit_count < 30 ? unit_count : 30); i++)
        grad[i] = end_value;
    const int units = end_unit - start_unit, values = end_value - start_value;
    if (units > 0 && values != 0) {
        /* The curve for a span of n units samples the main curve at i * 48 / n; the products truncate as
         * Python's int() does, with the same double arithmetic, so the gradient is the decoder's own */
        const double scale = (values < 0 ? -values - 1 : values - 1) / 31.0;
        for (int i = start_unit; i < end_unit; i++) {
            const int curve = at9_gradient_main[(i - start_unit) * 48 / units];
            const int step = (int)(curve * scale);
            grad[i] = values < 0 ? start_value - 1 - step : start_value + 1 + step;
        }
    }
}

static void precision_mask(const uint8_t *sf, int units, uint8_t mask[UNITS])
{
    memset(mask, 0, UNITS);
    for (int i = 1; i < units; i++) {
        const int delta = sf[i] - sf[i - 1];
        if (delta > 1)
            mask[i] += (uint8_t)(delta - 1 < 5 ? delta - 1 : 5);
        else if (delta < -1)
            mask[i - 1] += (uint8_t)(-delta - 1 < 5 ? -delta - 1 : 5);
    }
}

/* Each unit's coarse precision, and the fine precision above 15 (which the encoder must never need) */
static void precisions(const uint8_t *sf, const int *grad, int mode, int boundary, int units, int coarse[UNITS],
                       int fine[UNITS])
{
    memset(coarse, 0, sizeof(int) * UNITS);
    memset(fine, 0, sizeof(int) * UNITS);
    if (mode != 0) {
        uint8_t mask[UNITS];
        precision_mask(sf, units, mask);
        for (int i = 0; i < units; i++) {
            int p = sf[i] + mask[i] - grad[i];
            if (p > 0)
                p = mode == 1 ? p / 2 : mode == 2 ? 3 * p / 8 : p / 4;
            coarse[i] = p;
        }
    } else {
        for (int i = 0; i < units; i++)
            coarse[i] = sf[i] - grad[i];
    }
    for (int i = 0; i < units; i++) {
        if (coarse[i] < 1)
            coarse[i] = 1;
        if (i < boundary)
            coarse[i]++;
        if (coarse[i] > 15) {
            fine[i] = coarse[i] - 15;
            coarse[i] = 15;
        }
    }
}

/* Which of the two spectrum codebook sets each unit uses, from the scale factors */
static void codebook_sets(const uint8_t *sf, int units, uint8_t sets[UNITS])
{
    memset(sets, 0, UNITS);
    if (units <= 1)
        return;
    int s[UNITS + 1];
    for (int i = 0; i < units; i++)
        s[i] = sf[i];
    s[units] = sf[units - 1];
    int avg = 0;
    if (units > 12) {
        for (int i = 0; i < 12; i++)
            avg += s[i];
        avg = (avg + 6) / 12;
    }
    for (int i = 8; i < units; i++) {
        const int lo = s[i - 1] < s[i + 1] ? s[i - 1] : s[i + 1];
        if (s[i] - lo >= 3 || 2 * s[i] - s[i - 1] - s[i + 1] >= 3)
            sets[i] = 1;
    }
    for (int i = 12; i < units; i++) {
        if (sets[i])
            continue;
        const int lo = s[i - 1] < s[i + 1] ? s[i - 1] : s[i + 1];
        if (s[i] - lo >= 2 && s[i] >= avg - (QCNT[i] == 16 ? 1 : 0))
            sets[i] = 1;
    }
}

/* ---- the encoder ---- */

/* The psychoacoustic model's constants (atrac9_encoder.py's _Psycho) */
#define FULL_SCALE_SPL 110.0 /* playback level of a full-scale sine, dB SPL (loud) */
#define SLOPE_BELOW 27.0     /* masking slope towards lower frequencies, dB/Bark */
#define SLOPE_ABOVE 12.0     /* masking slope towards higher frequencies, dB/Bark */
#define NOISE_OFFSET 5.5     /* threshold below a noise-like masker, dB */
#define TONE_OFFSET 14.5     /* below a tonal masker at 0 Bark, rising 1 dB/Bark ... */
#define TONE_OFFSET_MAX 25.0 /* ... up to this, dB */
#define FLATNESS_TONAL (-25.0) /* spectral flatness (dB) taken as fully tonal */

/* Scale factor codings (the decoder's modes as written per channel) */
enum { SF_CLC, SF_DELTA, SF_DISTANCE, SF_OFFSET };

typedef struct {
    int bits; /* including the two mode bits; INT_MAX: this coding cannot be used */
    int mode, kind, length, base, weights, count;
    const uint8_t *baseline; /* SF_DISTANCE and SF_OFFSET: the scale factors predicted from */
} sfc_t;

typedef struct {
    int32_t bits; /* Huffman or fixed-length cost; BAD_BITS when the precision cannot be coded */
    double noise; /* quantisation noise over the masking threshold */
} cell_t;

/* One channel of one frame, quantised at every precision */
typedef struct {
    uint8_t sf[UNITS]; /* zero from the coded units up */
    uint8_t sets[UNITS];
    uint8_t mask[UNITS];
    uint8_t base[2][UNITS]; /* what the gradient is taken from: sf (mode 0), sf + mask (modes 1-3) */
    cell_t cell[UNITS][CELLS];
    int16_t q[16][N]; /* q[p]: the coefficients quantised at precision p (1..15) */
    sfc_t coding;
} chan_t;

/* One superframe's analysis */
typedef struct {
    double spec[2][FRAMES][N];
    double energy[2][FRAMES][UNITS];
    double mask[2][FRAMES][UNITS];
    uint8_t sf[2][FRAMES][UNITS];
} analysis_t;

typedef struct {
    uint8_t m, s, a, header_bits;
} cand_t;

typedef struct {
    int count;
    cand_t *list;
} cands_t;

/* A frame's best option for each size that lowers the noise (bytes rising, noise falling) */
typedef struct {
    int n, kmax;
    int *bytes;
    double *dist;
    int *flat; /* candidate * (kmax + 1) + boundary */
} opts_t;

typedef struct {
    int bitrate, frame_bytes, superframe_bytes, max_band;
    double window[2 * N]; /* ATRAC9's analysis window, mirrored */
    double hann[2 * N];
    cplx_t pre[N / 2], post[N / 2]; /* the MDCT's twiddles around its 128-point FFT */
    fft_t fft128, fft512;
    double spread[UNITS][UNITS]; /* [maskee][masker] */
    double tone_offset[UNITS], ath[UNITS];
    int bin_lo[UNITS], bin_hi[UNITS]; /* FFT bins each unit's tonality is measured over */
    double step[16], inv_step[16];
    int8_t grad[31][31][UNITS]; /* by start unit and start value; the end unit and value are always 31 */
    uint8_t pod[4][D_SPAN];     /* precision by mode and scale factor minus gradient, capped at P_CAP */
    cands_t cands[UNITS + 1];   /* by coded units, made when first needed */
    analysis_t an;
    double block[2][2 * N]; /* analyse()'s scratch, here rather than on a thread's stack */
    cplx_t fft_buf[2 * N];
    double power[2][N + 1];
    chan_t ch[FRAMES][2];
    opts_t opts[FRAMES];
    double *best_d, *dp, *dp_new;
    int *best_flat, *arg[FRAMES];
} enc_t;

static double bark(double f)
{
    return 13.0 * atan(0.00076 * f) + 3.5 * atan((f / 7500.0) * (f / 7500.0));
}

static double ath_db(double f)
{
    const double k = (f > 20.0 ? f : 20.0) / 1000.0;
    return 3.64 * pow(k, -0.8) - 6.5 * exp(-0.6 * (k - 3.3) * (k - 3.3)) + 1e-3 * pow(k, 4.0);
}

static void enc_free(enc_t *e)
{
    if (e == NULL)
        return;
    fft_free(&e->fft128);
    fft_free(&e->fft512);
    for (int u = 0; u <= UNITS; u++)
        free(e->cands[u].list);
    for (int f = 0; f < FRAMES; f++) {
        free(e->opts[f].bytes);
        free(e->opts[f].dist);
        free(e->opts[f].flat);
        free(e->arg[f]);
    }
    free(e->best_d);
    free(e->best_flat);
    free(e->dp);
    free(e->dp_new);
    free(e);
}

static enc_t *enc_new(int bitrate)
{
    enc_t *e = calloc(1, sizeof(enc_t));
    if (e == NULL)
        return NULL;
    e->bitrate = bitrate;
    e->frame_bytes = frame_bytes_for(bitrate);
    e->superframe_bytes = e->frame_bytes * FRAMES;
    /* The highest band coded at each per-channel rate; band 16 is 18 kHz */
    const int per_channel = bitrate / 2;
    e->max_band = per_channel >= 84 ? 16 : per_channel >= 72 ? 15 : per_channel >= 60 ? 14 : per_channel >= 48 ? 13 : 12;
    const size_t slots = (size_t)e->superframe_bytes + 1;
    bool ok = fft_init(&e->fft128, N / 2) && fft_init(&e->fft512, 2 * N);
    for (int f = 0; f < FRAMES && ok; f++) {
        e->opts[f].bytes = malloc(slots * sizeof(int));
        e->opts[f].dist = malloc(slots * sizeof(double));
        e->opts[f].flat = malloc(slots * sizeof(int));
        e->arg[f] = malloc(slots * sizeof(int));
        ok = e->opts[f].bytes && e->opts[f].dist && e->opts[f].flat && e->arg[f];
    }
    e->best_d = malloc(slots * sizeof(double));
    e->best_flat = malloc(slots * sizeof(int));
    e->dp = malloc(slots * sizeof(double));
    e->dp_new = malloc(slots * sizeof(double));
    if (!ok || !e->best_d || !e->best_flat || !e->dp || !e->dp_new) {
        enc_free(e);
        return NULL;
    }
    for (int n = 0; n < N; n++) {
        const double w = (sin(((n + 0.5) / N - 0.5) * M_PI) + 1.0) * 0.5;
        e->window[n] = w;
        e->window[2 * N - 1 - n] = w;
    }
    /* numpy's np.hanning(512), symmetric */
    for (int n = 0; n < 2 * N; n++)
        e->hann[n] = 0.5 + 0.5 * cos(M_PI * (double)(1 - 2 * N + 2 * n) / (double)(2 * N - 1));
    /* MDCT scaled by 2/N, through a DCT-IV of the folded block, through a 128-point FFT */
    for (int k = 0; k < N / 2; k++) {
        e->pre[k] = (cplx_t){cos(-M_PI * k / N), sin(-M_PI * k / N)};
        const double a = -M_PI * (k + 0.25) / N;
        e->post[k] = (cplx_t){cos(a) * (2.0 / N), sin(a) * (2.0 / N)};
    }
    const double bin_hz = AT9_RATE / 2.0 / N;
    double z[UNITS];
    for (int u = 0; u < UNITS; u++) {
        const double lo = QIDX[u] * bin_hz, hi = QIDX[u + 1] * bin_hz;
        z[u] = bark((lo + hi) / 2.0);
        double ath = INFINITY;
        const double step = (hi - lo) / 8.0;
        for (int t = 0; t <= 8; t++) {
            const double v = ath_db(t == 8 ? hi : t * step + lo);
            ath = v < ath ? v : ath;
        }
        e->ath[u] = 32768.0 * 32768.0 * pow(10.0, (ath - FULL_SCALE_SPL) / 10.0);
        e->tone_offset[u] = TONE_OFFSET + z[u] < TONE_OFFSET_MAX ? TONE_OFFSET + z[u] : TONE_OFFSET_MAX;
        e->bin_lo[u] = QIDX[u] - 1 > 0 ? QIDX[u] - 1 : 0;
        e->bin_hi[u] = QIDX[u + 1] + 2 < N + 1 ? QIDX[u + 1] + 2 : N + 1;
    }
    for (int i = 0; i < UNITS; i++) {
        for (int j = 0; j < UNITS; j++) {
            const double dz = z[i] - z[j];
            const double atten = dz < 0 ? SLOPE_BELOW * -dz : SLOPE_ABOVE * dz;
            e->spread[i][j] = pow(10.0, -atten / 10.0);
        }
    }
    for (int i = 0; i < 16; i++) {
        e->step[i] = 2.0 / (double)((1 << (i + 1)) - 1);
        e->inv_step[i] = 1.0 / e->step[i];
    }
    for (int s = 1; s <= 30; s++) {
        for (int a = 0; a <= 30; a++) {
            int g[31];
            gradient_curve(s, 31, a, 31, 30, g);
            for (int u = 0; u < UNITS; u++)
                e->grad[s][a][u] = (int8_t)g[u];
        }
    }
    for (int m = 0; m < 4; m++) {
        for (int d = D_MIN; d <= D_MAX; d++) {
            int p = d;
            if (p > 0)
                p = m == 1 ? p / 2 : m == 2 ? 3 * p / 8 : m == 3 ? p / 4 : p;
            p = p < 1 ? 1 : p > P_CAP ? P_CAP : p;
            e->pod[m][d - D_MIN] = (uint8_t)p;
        }
    }
    return e;
}

/* The gradient settings searched for a number of coded units: modes 1-3 with
 * start units around the bandwidth, mode 1 from the first unit, and mode 0 at
 * three start units, in the converter's (sorted) order */
static const cands_t *get_cands(enc_t *e, int units)
{
    cands_t *c = &e->cands[units];
    if (c->list != NULL)
        return c;
    const int lo = units - 12 > 1 ? units - 12 : 1, hi = units + 2 < 30 ? units + 2 : 30;
    const int s0[3] = {units - 8 > 1 ? units - 8 : 1, units - 4 > 1 ? units - 4 : 1, units < 30 ? units : 30};
    c->list = malloc(sizeof(cand_t) * 4 * 30 * 31);
    if (c->list == NULL)
        return NULL;
    c->count = 0;
    for (int m = 0; m < 4; m++) {
        for (int s = 1; s <= 30; s++) {
            for (int a = 0; a <= 30; a++) {
                const bool in = m == 0 ? (s == s0[0] || s == s0[1] || s == s0[2])
                                       : (s >= lo && s <= hi && a <= 26) || (m == 1 && s == 1);
                if (in)
                    c->list[c->count++] = (cand_t){(uint8_t)m, (uint8_t)s, (uint8_t)a, (uint8_t)(m ? 16 : 28)};
            }
        }
    }
    return c;
}

static void mdct(enc_t *e, const double *x, double *out)
{
    const double *w = e->window;
    double u[N];
    for (int i = 0; i < N / 2; i++) {
        u[i] = -x[383 - i] * w[383 - i] - x[384 + i] * w[384 + i];
        u[N / 2 + i] = x[i] * w[i] - x[255 - i] * w[255 - i];
    }
    cplx_t t[N / 2];
    for (int m = 0; m < N / 2; m++) {
        const double re = u[2 * m], im = u[N - 1 - 2 * m];
        t[m] = (cplx_t){re * e->pre[m].re - im * e->pre[m].im, re * e->pre[m].im + im * e->pre[m].re};
    }
    fft_run(&e->fft128, t, false);
    for (int k = 0; k < N / 2; k++) {
        out[2 * k] = t[k].re * e->post[k].re - t[k].im * e->post[k].im;
        out[N - 1 - 2 * k] = -(t[k].re * e->post[k].im + t[k].im * e->post[k].re);
    }
}

/* Per-unit masking thresholds (in the MDCT coefficients' energy) from the unit
 * energies, spread across the Bark scale and lowered by an offset that grows with
 * tonality, measured by the spectral flatness of the Hann-windowed FFT */
static void thresholds(const enc_t *e, const double *energy, const double *power, double *mask)
{
    double logp[N + 1];
    for (int i = 0; i <= N; i++)
        logp[i] = log(power[i] + 1e-12);
    for (int u = 0; u < UNITS; u++) {
        const int lo = e->bin_lo[u], hi = e->bin_hi[u];
        double sum_log = 0.0, sum_power = 0.0;
        for (int i = lo; i < hi; i++) {
            sum_log += logp[i];
            sum_power += power[i];
        }
        const double gm = sum_log / (hi - lo);
        const double am = log(sum_power / (hi - lo) + 1e-12);
        const double sfm_db = 10.0 / log(10.0) * (gm - am);
        double tonal = sfm_db / FLATNESS_TONAL;
        tonal = tonal < 0.0 ? 0.0 : tonal > 1.0 ? 1.0 : tonal;
        const double offset = tonal * e->tone_offset[u] + (1.0 - tonal) * NOISE_OFFSET;
        double masked = 0.0;
        for (int j = 0; j < UNITS; j++)
            masked += energy[j] * e->spread[u][j];
        masked *= pow(10.0, -offset / 10.0);
        mask[u] = masked > e->ath[u] ? masked : e->ath[u];
    }
}

/* The smallest scale factor whose range 2^(sf - 15) covers the peak, exactly (frexp, not a rounded log2) */
static uint8_t scale_factor(double peak)
{
    if (!(peak > 0.0))
        return 0;
    int e;
    const double m = frexp(peak, &e);
    const int sf = (m == 0.5 ? e - 1 : e) + 15;
    return (uint8_t)(sf < 0 ? 0 : sf > 31 ? 31 : sf);
}

/* Spectra, energies, masking thresholds and scale factors of one superframe's
 * frames; frame f's block is the 512 samples from (f - 1) * 256, zero outside the clip */
static void analyse(enc_t *e, const double *l, const double *r, size_t n, size_t superframe)
{
    analysis_t *a = &e->an;
    for (int f = 0; f < FRAMES; f++) {
        const int64_t start = ((int64_t)superframe * FRAMES + f - 1) * N;
        double(*block)[2 * N] = e->block;
        for (int c = 0; c < 2; c++) {
            const double *x = c ? r : l;
            for (int i = 0; i < 2 * N; i++) {
                const int64_t t = start + i;
                block[c][i] = t >= 0 && t < (int64_t)n ? x[t] * 32768.0 : 0.0;
            }
            mdct(e, block[c], a->spec[c][f]);
        }
        /* Both channels' power spectra from one complex FFT (left real, right imaginary) */
        cplx_t *z = e->fft_buf;
        for (int i = 0; i < 2 * N; i++)
            z[i] = (cplx_t){block[0][i] * e->hann[i], block[1][i] * e->hann[i]};
        fft_run(&e->fft512, z, false);
        double(*power)[N + 1] = e->power;
        for (int k = 0; k <= N; k++) {
            const cplx_t p = z[k], q = z[(2 * N - k) % (2 * N)];
            const double lr = p.re + q.re, li = p.im - q.im; /* 2 * left */
            const double rr = p.im + q.im, ri = q.re - p.re; /* 2 * right */
            power[0][k] = (lr * lr + li * li) * 0.25;
            power[1][k] = (rr * rr + ri * ri) * 0.25;
        }
        for (int c = 0; c < 2; c++) {
            const double *spec = a->spec[c][f];
            for (int u = 0; u < UNITS; u++) {
                double energy = 0.0, peak = 0.0;
                for (int i = QIDX[u]; i < QIDX[u + 1]; i++) {
                    energy += spec[i] * spec[i];
                    peak = fmax(peak, fabs(spec[i]));
                }
                a->energy[c][f][u] = energy;
                a->sf[c][f][u] = scale_factor(peak);
            }
            thresholds(e, a->energy[c][f], power[c], a->mask[c][f]);
        }
    }
}

/* The highest unit whose energy is above its threshold in any frame or channel, plus one */
static int audible_top(const analysis_t *a)
{
    int top = 0;
    for (int c = 0; c < 2; c++)
        for (int f = 0; f < FRAMES; f++)
            for (int u = top; u < UNITS; u++)
                if (a->energy[c][f][u] > a->mask[c][f][u])
                    top = u + 1;
    return top;
}

/* One channel of a frame at every precision: the quantised values, their exact
 * cost with the codebooks the decoder will pick, and their weighted noise */
static void prepare_channel(enc_t *e, chan_t *ch, const double *x, const uint8_t *sf_in, const double *mask_in,
                            int units)
{
    memset(ch->sf, 0, UNITS);
    memcpy(ch->sf, sf_in, (size_t)units);
    codebook_sets(ch->sf, units, ch->sets);
    precision_mask(ch->sf, units, ch->mask);
    for (int u = 0; u < UNITS; u++) {
        ch->base[0][u] = ch->sf[u];
        ch->base[1][u] = (uint8_t)(ch->sf[u] + ch->mask[u]);
    }
    const int coefs = QIDX[units];
    double xn[N], scale[N];
    for (int u = 0; u < units; u++) {
        const double s = ldexp(1.0, ch->sf[u] - 15);
        for (int i = QIDX[u]; i < QIDX[u + 1]; i++) {
            scale[i] = s;
            xn[i] = x[i] / s;
        }
    }
    for (int p = 1; p <= 15; p++) {
        const double inv = e->inv_step[p], maxq = (double)((1 << p) - 1);
        for (int i = 0; i < coefs; i++) {
            double v = rint(xn[i] * inv);
            v = v > maxq ? maxq : v < -maxq ? -maxq : v;
            ch->q[p][i] = (int16_t)v;
        }
    }
    /* The codebook rules (docs/atrac9-profile.md): in units 0-7 at word lengths 2-4 the larger of each pair
     * needs a magnitude of at least 2^(word length - 2), and in units 8-11 at word length 2 a group of four
     * cannot be all zero. Where quantising leaves less, the coefficient with the larger magnitude gets the
     * smallest value allowed, with its own sign. */
    for (int p = 1; p <= 3; p++) {
        const int need = 1 << (p - 1);
        for (int u = 0; u < 8 && u < units; u++) {
            int16_t *q = &ch->q[p][2 * u];
            if (abs(q[0]) >= need || abs(q[1]) >= need)
                continue;
            const int j = fabs(xn[2 * u + 1]) > fabs(xn[2 * u]) ? 1 : 0;
            q[j] = (int16_t)(xn[2 * u + j] >= 0 ? need : -need);
        }
    }
    for (int u = 8; u < 12 && u < units; u++) {
        int16_t *q = &ch->q[1][16 + 4 * (u - 8)];
        if (q[0] || q[1] || q[2] || q[3])
            continue;
        const double *xs = &xn[16 + 4 * (u - 8)];
        int j = 0;
        for (int k = 1; k < 4; k++)
            if (fabs(xs[k]) > fabs(xs[j]))
                j = k;
        q[j] = (int16_t)(xs[j] >= 0 ? 1 : -1);
    }
    for (int u = 0; u < UNITS; u++)
        for (int p = 0; p < CELLS; p++)
            ch->cell[u][p] = (cell_t){BAD_BITS, 0.0};
    for (int u = 0; u < units; u++) {
        const double weight = 1.0 / mask_in[u];
        for (int p = 1; p <= 15; p++) {
            double noise = 0.0;
            for (int i = QIDX[u]; i < QIDX[u + 1]; i++) {
                const double err = (xn[i] - ch->q[p][i] * e->step[p]) * scale[i];
                noise += err * err;
            }
            ch->cell[u][p].noise = noise * weight;
        }
        for (int p = 1; p <= 15; p++) {
            const int wl = p + 1;
            if (wl > 7) {
                ch->cell[u][p].bits = QCNT[u] * wl;
                continue;
            }
            const int cbi = CBI[u], vc = 1 << at9_spectrum_vcp[wl][cbi];
            const unsigned value_mask = (1u << wl) - 1;
            const uint8_t *book = at9_spectrum_bits[ch->sets[u]][wl][cbi];
            int bits = 0;
            for (int g = QIDX[u]; g < QIDX[u + 1]; g += vc) {
                unsigned sym = 0;
                for (int j = 0; j < vc; j++)
                    sym |= ((unsigned)ch->q[p][g + j] & value_mask) << (j * wl);
                if (book[sym] == 0) {
                    /* A value with no code: the rules above keep this from happening, and if it did the
                     * precision would simply not be chosen */
                    bits = BAD_BITS;
                    break;
                }
                bits += book[sym];
            }
            ch->cell[u][p].bits = bits;
        }
    }
}

/* ---- scale factor coding: whichever of the decoder's modes is cheapest ---- */

static const sfc_t SFC_NONE = {INT_MAX, 0, 0, 0, 0, 0, 0, NULL};

static sfc_t sf_clc(const uint8_t *sf, int units)
{
    int lo = sf[0], hi = sf[0];
    for (int u = 1; u < units; u++) {
        lo = sf[u] < lo ? sf[u] : lo;
        hi = sf[u] > hi ? sf[u] : hi;
    }
    for (int length = 2; length <= 4; length++)
        if (hi - lo < (1 << length))
            return (sfc_t){2 + 2 + 5 + units * length, 1, SF_CLC, length, lo, 0, 0, NULL};
    return (sfc_t){2 + 2 + units * 5, 1, SF_CLC, 5, 0, 0, 0, NULL};
}

static sfc_t sf_delta_offset(const uint8_t *sf, int units, int mode)
{
    int t[8][UNITS], base[8], tmax[8];
    for (int w = 0; w < 8; w++) {
        int lo = INT_MAX, hi = INT_MIN;
        for (int u = 0; u < units; u++) {
            t[w][u] = sf[u] + at9_sf_weights[w][u];
            lo = t[w][u] < lo ? t[w][u] : lo;
            hi = t[w][u] > hi ? t[w][u] : hi;
        }
        base[w] = lo < 0 ? 0 : lo > 31 ? 31 : lo;
        tmax[w] = hi;
        /* lo - base is never negative here: sf and the weights are */
        if (lo - base[w] < 0)
            tmax[w] = INT_MAX;
    }
    sfc_t best = SFC_NONE;
    for (int length = 3; length <= 6; length++) {
        const int mask = (1 << length) - 1;
        int best_w = -1, best_bits = INT_MAX;
        for (int w = 0; w < 8; w++) {
            if (tmax[w] == INT_MAX || tmax[w] - base[w] >= (1 << length))
                continue;
            int bits = 2 + 3 + 5 + 2 + length;
            for (int u = 1; u < units; u++)
                bits += at9_sf_unsigned_bits[length][(t[w][u] - t[w][u - 1]) & mask];
            if (bits < best_bits) {
                best_bits = bits;
                best_w = w;
            }
        }
        if (best_w >= 0 && best_bits < best.bits)
            best = (sfc_t){best_bits, mode, SF_DELTA, length, base[best_w], best_w, 0, NULL};
    }
    return best;
}

static sfc_t sf_distance(const uint8_t *sf, int units, const uint8_t *baseline, int baseline_len, int mode)
{
    const int count = units < baseline_len ? units : baseline_len;
    int d[UNITS], lo = 0, hi = 0;
    for (int u = 0; u < count; u++) {
        d[u] = sf[u] - baseline[u];
        lo = u == 0 || d[u] < lo ? d[u] : lo;
        hi = u == 0 || d[u] > hi ? d[u] : hi;
    }
    sfc_t best = SFC_NONE;
    for (int length = 2; length <= 5; length++) {
        const int half = 1 << (length - 1), mask = (1 << length) - 1;
        if (lo < -half || hi >= half)
            continue;
        int bits = 2 + 2 + (units - count) * 5;
        bool ok = true;
        for (int u = 0; u < count && ok; u++) {
            const int b = at9_sf_signed_bits[length][d[u] & mask];
            ok = b != 0;
            bits += b;
        }
        if (ok && bits < best.bits)
            best = (sfc_t){bits, mode, SF_DISTANCE, length, 0, 0, count, baseline};
    }
    return best;
}

static sfc_t sf_delta_baseline(const uint8_t *sf, int units, const uint8_t *baseline, int baseline_len, int mode)
{
    const int count = units < baseline_len ? units : baseline_len;
    if (count < 1)
        return SFC_NONE;
    int rd[UNITS], lo = INT_MAX, hi = INT_MIN;
    for (int u = 0; u < count; u++) {
        rd[u] = sf[u] - baseline[u];
        lo = rd[u] < lo ? rd[u] : lo;
    }
    const int base = lo < -16 ? -16 : lo > 15 ? 15 : lo;
    for (int u = 0; u < count; u++) {
        rd[u] -= base;
        hi = rd[u] > hi ? rd[u] : hi;
    }
    if (lo - base < 0)
        return SFC_NONE;
    sfc_t best = SFC_NONE;
    for (int length = 1; length <= 4; length++) {
        if (hi >= (1 << length))
            continue;
        const int mask = (1 << length) - 1;
        int bits = 2 + 5 + 2 + length + (units - count) * 5;
        for (int u = 1; u < count; u++)
            bits += at9_sf_unsigned_bits[length][(rd[u] - rd[u - 1]) & mask];
        if (bits < best.bits)
            best = (sfc_t){bits, mode, SF_OFFSET, length, base, 0, count, baseline};
    }
    return best;
}

static void consider(sfc_t *best, sfc_t option)
{
    if (option.bits < best->bits)
        *best = option;
}

/* The decoder's mode numbers differ by channel: channel 1 can also predict from channel 0 */
static sfc_t choose_sf_coding(const uint8_t *sf, int units, int channel, bool first, const uint8_t *prev,
                              const uint8_t *ch0)
{
    sfc_t best = SFC_NONE;
    if (channel == 0) {
        consider(&best, sf_delta_offset(sf, units, 0));
        consider(&best, sf_clc(sf, units));
        if (!first) {
            consider(&best, sf_distance(sf, units, prev, units, 2));
            consider(&best, sf_delta_baseline(sf, units, prev, units, 3));
        }
    } else {
        consider(&best, sf_delta_offset(sf, units, 0));
        consider(&best, sf_distance(sf, units, ch0, units, 1));
        consider(&best, sf_delta_baseline(sf, units, ch0, units, 2));
        if (!first)
            consider(&best, sf_distance(sf, units, prev, units, 3));
    }
    return best;
}

/* ---- bit allocation ---- */

/* Every candidate gradient with every boundary: for each frame size in bytes the
 * option with the least weighted noise (the first in candidate order on a tie),
 * then only the sizes that lower the noise. False when nothing fits. */
static bool frame_options(enc_t *e, int f, const cands_t *cands, int fixed_bits, int units)
{
    const int kmax = units < 15 ? units : 15;
    const int cap = e->superframe_bytes, limit = 8 * cap;
    for (int i = 0; i <= cap; i++) {
        e->best_d[i] = INFINITY;
        e->best_flat[i] = -1;
    }
    for (int ci = 0; ci < cands->count; ci++) {
        const cand_t cd = cands->list[ci];
        const int8_t *g = e->grad[cd.s][cd.a];
        const uint8_t *pod = e->pod[cd.m] - D_MIN;
        int bits0 = fixed_bits + cd.header_bits;
        int dbits[16] = {0};    /* added bits of a boundary above each unit, both channels */
        double d0[2] = {0.0, 0.0}, dn[2][16];
        bool bad = false;
        for (int c = 0; c < 2 && !bad; c++) {
            const chan_t *ch = &e->ch[f][c];
            const uint8_t *base = ch->base[cd.m != 0];
            for (int u = 0; u < units; u++) {
                const cell_t *cell = &ch->cell[u][pod[base[u] - g[u]]];
                if (cell->bits >= BAD_BITS) {
                    bad = true;
                    break;
                }
                bits0 += cell->bits;
                d0[c] += cell->noise;
                if (u < kmax) {
                    /* One step up (the boundary's extra bit); BAD_BITS there rules out the boundaries past u */
                    dbits[u] += cell[1].bits - cell->bits;
                    dn[c][u] = cell[1].noise - cell->noise;
                }
            }
        }
        if (bad)
            continue;
        int bits = bits0;
        double cum0 = 0.0, cum1 = 0.0;
        for (int k = 0; k <= kmax; k++) {
            if (k > 0) {
                bits += dbits[k - 1];
                cum0 += dn[0][k - 1];
                cum1 += dn[1][k - 1];
            }
            if (bits > limit)
                continue;
            const double dist = (cum0 + d0[0]) + (cum1 + d0[1]);
            const int nb = (bits + 7) >> 3;
            if (dist < e->best_d[nb]) {
                e->best_d[nb] = dist;
                e->best_flat[nb] = ci * (kmax + 1) + k;
            }
        }
    }
    opts_t *o = &e->opts[f];
    o->n = 0;
    o->kmax = kmax;
    double low = INFINITY;
    for (int nb = 0; nb <= cap; nb++) {
        if (e->best_flat[nb] < 0 || !(o->n == 0 || e->best_d[nb] < low))
            continue;
        o->bytes[o->n] = nb;
        o->dist[o->n] = e->best_d[nb];
        o->flat[o->n] = e->best_flat[nb];
        o->n++;
        low = e->best_d[nb];
    }
    return o->n > 0;
}

/* One option per frame, the least total noise within the superframe's bytes (the
 * smallest total size on a tie). False when the four cannot fit together. */
static bool allocate(enc_t *e, int choice[FRAMES])
{
    const int cap = e->superframe_bytes;
    double *dp = e->dp, *next = e->dp_new;
    for (int t = 0; t <= cap; t++)
        dp[t] = INFINITY;
    dp[0] = 0.0;
    for (int f = 0; f < FRAMES; f++) {
        const opts_t *o = &e->opts[f];
        int *arg = e->arg[f];
        for (int t = 0; t <= cap; t++) {
            next[t] = INFINITY;
            arg[t] = -1;
        }
        for (int i = 0; i < o->n; i++) {
            const int size = o->bytes[i];
            const double d = o->dist[i];
            for (int t = size; t <= cap; t++) {
                const double v = dp[t - size] + d;
                if (v < next[t]) {
                    next[t] = v;
                    arg[t] = i;
                }
            }
        }
        double *swap = dp;
        dp = next;
        next = swap;
    }
    int pos = 0;
    for (int t = 1; t <= cap; t++)
        if (dp[t] < dp[pos])
            pos = t;
    if (!is_finite(dp[pos]))
        return false;
    for (int f = FRAMES - 1; f >= 0; f--) {
        const int i = e->arg[f][pos];
        choice[f] = i;
        pos -= e->opts[f].bytes[i];
    }
    return true;
}

/* ---- the bitstream ---- */

typedef struct {
    uint8_t *data; /* zeroed beforehand */
    size_t pos, limit; /* bits */
} bitw_t;

static void put(bitw_t *w, uint32_t value, int count)
{
    for (int i = count - 1; i >= 0; i--, w->pos++)
        if (w->pos < w->limit && ((value >> i) & 1))
            w->data[w->pos >> 3] |= (uint8_t)(0x80u >> (w->pos & 7));
}

static void write_sf(bitw_t *w, const uint8_t *sf, int units, const sfc_t *c)
{
    put(w, (uint32_t)c->mode, 2);
    const int length = c->length, mask = (1 << length) - 1;
    switch (c->kind) {
    case SF_CLC:
        put(w, (uint32_t)(length - 2), 2);
        if (length < 5)
            put(w, (uint32_t)c->base, 5);
        for (int u = 0; u < units; u++)
            put(w, (uint32_t)(sf[u] - c->base), length);
        break;
    case SF_DELTA: {
        const uint8_t *weights = at9_sf_weights[c->weights];
        put(w, (uint32_t)c->weights, 3);
        put(w, (uint32_t)c->base, 5);
        put(w, (uint32_t)(length - 3), 2);
        put(w, (uint32_t)(sf[0] + weights[0] - c->base), length);
        for (int u = 1; u < units; u++) {
            const int d = ((sf[u] + weights[u]) - (sf[u - 1] + weights[u - 1])) & mask;
            put(w, at9_sf_unsigned_codes[length][d], at9_sf_unsigned_bits[length][d]);
        }
        break;
    }
    case SF_DISTANCE:
        put(w, (uint32_t)(length - 2), 2);
        for (int u = 0; u < c->count; u++) {
            const int sym = (sf[u] - c->baseline[u]) & mask;
            put(w, at9_sf_signed_codes[length][sym], at9_sf_signed_bits[length][sym]);
        }
        for (int u = c->count; u < units; u++)
            put(w, sf[u], 5);
        break;
    default: /* SF_OFFSET */
        put(w, (uint32_t)(c->base + 16), 5);
        put(w, (uint32_t)(length - 1), 2);
        put(w, (uint32_t)(sf[0] - c->baseline[0] - c->base), length);
        for (int u = 1; u < c->count; u++) {
            const int d = ((sf[u] - c->baseline[u]) - (sf[u - 1] - c->baseline[u - 1])) & mask;
            put(w, at9_sf_unsigned_codes[length][d], at9_sf_unsigned_bits[length][d]);
        }
        for (int u = c->count; u < units; u++)
            put(w, sf[u], 5);
        break;
    }
}

/* Writes frame f with option `index` at out; returns its bytes, or -1 (err says why) */
static int write_frame(enc_t *e, int f, int index, int band, int units, const cands_t *cands, uint8_t *out,
                       int room, char *err, size_t errlen)
{
    const opts_t *o = &e->opts[f];
    const int ci = o->flat[index] / (o->kmax + 1), boundary = o->flat[index] % (o->kmax + 1);
    const cand_t cd = cands->list[ci];
    const bool first = f == 0;
    bitw_t w = {out, 0, (size_t)room * 8};
    put(&w, first ? 0 : 1, 1); /* first frame of the superframe: not */
    put(&w, first ? 0 : 1, 1); /* band parameters reused from the first frame */
    if (first) {
        put(&w, (uint32_t)(band - 3), 4);
        put(&w, (uint32_t)(band - 3), 4); /* stereo band = band: no intensity stereo */
        put(&w, 0, 1);                    /* no band extension */
    }
    put(&w, cd.m, 2);
    if (cd.m != 0) {
        put(&w, cd.s, 5);
        put(&w, cd.a, 5);
    } else {
        put(&w, cd.s, 6);
        put(&w, 31 - 1, 6); /* end unit */
        put(&w, cd.a, 5);
        put(&w, 31, 5); /* end value */
    }
    put(&w, (uint32_t)boundary, 4);
    put(&w, 0, 1); /* primary channel 0 */
    put(&w, 0, 1); /* no intensity-stereo signs */
    put(&w, 0, 1); /* no band-extension data */
    int grad[31];
    gradient_curve(cd.s, 31, cd.a, 31, units, grad);
    for (int c = 0; c < 2; c++) {
        const chan_t *ch = &e->ch[f][c];
        int coarse[UNITS], fine[UNITS];
        precisions(ch->sf, grad, cd.m, boundary, units, coarse, fine);
        for (int u = 0; u < units; u++) {
            if (fine[u]) {
                snprintf(err, errlen, "internal error: fine precision selected");
                return -1;
            }
        }
        write_sf(&w, ch->sf, units, &ch->coding);
        for (int u = 0; u < units; u++) {
            const int p = coarse[u], wl = p + 1;
            const int16_t *q = ch->q[p];
            if (wl <= 7) {
                const int cbi = CBI[u], vc = 1 << at9_spectrum_vcp[wl][cbi];
                const unsigned value_mask = (1u << wl) - 1;
                const uint8_t *bits = at9_spectrum_bits[ch->sets[u]][wl][cbi];
                const uint16_t *codes = at9_spectrum_codes[ch->sets[u]][wl][cbi];
                for (int g = QIDX[u]; g < QIDX[u + 1]; g += vc) {
                    unsigned sym = 0;
                    for (int j = 0; j < vc; j++)
                        sym |= ((unsigned)q[g + j] & value_mask) << (j * wl);
                    if (bits[sym] == 0) {
                        snprintf(err, errlen, "internal error: invalid spectrum symbol");
                        return -1;
                    }
                    put(&w, codes[sym], bits[sym]);
                }
            } else {
                for (int i = QIDX[u]; i < QIDX[u + 1]; i++)
                    put(&w, (uint32_t)(int32_t)q[i], wl);
            }
        }
    }
    const int bytes = (int)((w.pos + 7) / 8);
    if (w.pos > w.limit || bytes != o->bytes[index]) {
        snprintf(err, errlen, "internal error: frame size mismatch (%d != %d)", bytes, o->bytes[index]);
        return -1;
    }
    return bytes;
}

/* One superframe at a coded bandwidth: 0, 1 when it does not fit (narrow the band), -1 on an error */
static int encode_superframe(enc_t *e, int band, uint8_t *out, char *err, size_t errlen)
{
    const int units = BAND_UNITS[band];
    const cands_t *cands = get_cands(e, units);
    if (cands == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    const analysis_t *a = &e->an;
    for (int f = 0; f < FRAMES; f++) {
        const bool first = f == 0;
        /* Flags, band parameters on the first frame, stereo flags and the band-extension flag; the gradient's
         * own bits are each candidate's header_bits */
        int fixed = 2 + (first ? 9 : 0) + 2 + 1;
        for (int c = 0; c < 2; c++) {
            chan_t *ch = &e->ch[f][c];
            prepare_channel(e, ch, a->spec[c][f], a->sf[c][f], a->mask[c][f], units);
            ch->coding = choose_sf_coding(ch->sf, units, c, first, first ? NULL : e->ch[f - 1][c].sf,
                                          c ? e->ch[f][0].sf : NULL);
            fixed += ch->coding.bits;
        }
        if (!frame_options(e, f, cands, fixed, units))
            return 1;
    }
    int choice[FRAMES];
    if (!allocate(e, choice))
        return 1;
    const int cap = e->superframe_bytes;
    memset(out, 0, (size_t)cap);
    int pos = 0;
    for (int f = 0; f < FRAMES; f++) {
        const int bytes = write_frame(e, f, choice[f], band, units, cands, out + pos, cap - pos, err, errlen);
        if (bytes < 0)
            return -1;
        pos += bytes;
    }
    /* Sony's decoder rejects a superframe whose unused tail is not 0x01 bytes */
    memset(out + pos, 0x01, (size_t)(cap - pos));
    return 0;
}

/* ---- the container ---- */

static void le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, v);
    le16(p + 2, v >> 16);
}

/* RIFF/WAVE as at9tool -wholeloop writes it: fmt (WAVE_FORMAT_EXTENSIBLE with the
 * ATRAC9 configuration), fact (samples, then the overlap and encoder delays), smpl
 * (one forward loop from 256 to samples + 255, played forever) and data's header */
static void write_header(uint8_t *h, int frame_bytes, uint32_t samples, uint32_t data_len)
{
    const uint32_t superframe_bytes = (uint32_t)frame_bytes * FRAMES;
    memcpy(h, "RIFF", 4);
    le32(h + 4, HEADER_SIZE - 8 + data_len);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    le32(h + 16, 52);
    le16(h + 20, 0xFFFE);
    le16(h + 22, 2);
    le32(h + 24, AT9_RATE);
    le32(h + 28, superframe_bytes * AT9_RATE / SUPERFRAME_SAMPLES);
    le16(h + 32, superframe_bytes); /* block align */
    le16(h + 34, 0);                /* bits per sample */
    le16(h + 36, 34);               /* extension size */
    le16(h + 38, SUPERFRAME_SAMPLES);
    le32(h + 40, 0x3);              /* speakers: front left and right */
    memcpy(h + 44, ATRAC9_GUID, 16);
    le32(h + 60, 1); /* version */
    /* The configuration word, big-endian: 0xFE, 48 kHz (index 7), one stereo block (2), frame bytes - 1,
     * four frames per superframe (2) */
    const uint32_t config = 0xFEu << 24 | 7u << 20 | 2u << 17 | (uint32_t)(frame_bytes - 1) << 5 | 2u << 3;
    h[64] = (uint8_t)(config >> 24);
    h[65] = (uint8_t)(config >> 16);
    h[66] = (uint8_t)(config >> 8);
    h[67] = (uint8_t)config;
    le32(h + 68, 0);
    memcpy(h + 72, "fact", 4);
    le32(h + 76, 12);
    le32(h + 80, samples);
    le32(h + 84, AT9_ENCODER_DELAY);
    le32(h + 88, AT9_ENCODER_DELAY);
    memcpy(h + 92, "smpl", 4);
    le32(h + 96, 60);
    /* manufacturer, product, sample period (ns, rounded), MIDI unity note, pitch fraction, SMPTE format and
     * offset, one loop, sampler data size 24 as Sony writes */
    const uint32_t smpl[9] = {0, 0, 20833, 60, 0, 0, 0, 1, 24};
    for (int i = 0; i < 9; i++)
        le32(h + 100 + 4 * i, smpl[i]);
    const uint32_t loop[6] = {0, 0, AT9_ENCODER_DELAY, samples + AT9_ENCODER_DELAY - 1, 0, 0};
    for (int i = 0; i < 6; i++)
        le32(h + 136 + 4 * i, loop[i]);
    memcpy(h + 160, "data", 4);
    le32(h + 164, data_len);
}

bool at9_encode(const double *l, const double *r, size_t n, int bitrate_kbps, buf_t *file, char *err,
                size_t errlen)
{
    file->len = 0;
    if (frame_bytes_for(bitrate_kbps) == 0) {
        snprintf(err, errlen, "%d kb/s is not an ATRAC9 stereo bit rate (72, 96, 120, 144, 168 or 192)",
                 bitrate_kbps);
        return false;
    }
    if (n == 0) {
        snprintf(err, errlen, "the audio is empty");
        return false;
    }
    const size_t size = at9_file_size(n, bitrate_kbps);
    if (size > AT9_SIZE_LIMIT) {
        snprintf(err, errlen,
                 "%.3f s at %d kb/s makes a %zu-byte file, above the 2 MiB PS5 limit; use at most %.3f s or a lower "
                 "bit rate",
                 (double)n / AT9_RATE, bitrate_kbps, size, (double)at9_max_samples(bitrate_kbps) / AT9_RATE);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!is_finite(l[i]) || !is_finite(r[i])) {
            snprintf(err, errlen, "the audio has a sample that is not a number, at %zu", i);
            return false;
        }
    }
    const size_t superframes = (n + AT9_ENCODER_DELAY + SUPERFRAME_SAMPLES - 1) / SUPERFRAME_SAMPLES;
    enc_t *e = enc_new(bitrate_kbps);
    uint8_t *need = malloc(superframes);
    if (e == NULL || need == NULL || !buf_reserve(file, size)) {
        enc_free(e);
        free(need);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    /* The coded bandwidth of each superframe: the lowest band that holds every unit above its threshold, then
     * the widest within two superframes either side, so the bandwidth does not flicker */
    for (size_t s = 0; s < superframes; s++) {
        analyse(e, l, r, n, s);
        const int top = audible_top(&e->an);
        int band = 3;
        while (band < 18 && BAND_UNITS[band] < top)
            band++;
        need[s] = (uint8_t)band;
    }
    write_header(file->data, e->frame_bytes, (uint32_t)n, (uint32_t)(size - HEADER_SIZE));
    file->len = HEADER_SIZE;
    bool ok = true;
    for (size_t s = 0; s < superframes && ok; s++) {
        int band = need[s];
        for (size_t t = s >= 2 ? s - 2 : 0; t <= s + 2 && t < superframes; t++)
            band = need[t] > band ? need[t] : band;
        band = band > e->max_band ? e->max_band : band < 3 ? 3 : band;
        analyse(e, l, r, n, s);
        for (;;) {
            const int got = encode_superframe(e, band, file->data + file->len, err, errlen);
            if (got == 0)
                break;
            if (got < 0 || band <= 3) {
                if (got > 0)
                    snprintf(err, errlen, "superframe %zu cannot be encoded within its size", s);
                ok = false;
                break;
            }
            band--; /* narrow the coded bandwidth until the superframe fits (low bit rates only) */
        }
        file->len += (size_t)e->superframe_bytes;
    }
    enc_free(e);
    free(need);
    if (ok && file->len != size) {
        snprintf(err, errlen, "internal error: unexpected file size");
        ok = false;
    }
    if (!ok)
        file->len = 0;
    return ok;
}
