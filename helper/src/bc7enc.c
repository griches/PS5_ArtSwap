/*
 * Art Swap helper - BC7 for the home screen's DDS backgrounds.
 *
 * Mode 6 only (one subset, RGBA endpoints of 7 bits plus a p-bit, 4-bit
 * indices): principal-axis endpoints, the indices they give, one
 * least-squares refit, both p-bit pairs tried. It is the encoder of PS5
 * RetroArch's tools/make-title-art.py (Copyright (C) 2026 Mihawk-99,
 * GPL-3.0-or-later), which our titles' pic0.dds files are made with, in C
 * with alpha, spread over threads by rows of blocks.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "image.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

static const int kWeights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

/* 7 stored bits for a channel value with this p-bit */
static int quantise(float value, int p)
{
    const int q = (int)lroundf((value - (float)p) * 0.5f);
    return q < 0 ? 0 : q > 127 ? 127 : q;
}

typedef struct {
    int q[2][4]; /* endpoints, 7 bits a channel */
    int p[2];    /* p-bits */
    int idx[16];
    long err;
} block_fit_t;

/* The best index of each pixel for the endpoints in fit, and the error */
static void choose(const int px[16][4], block_fit_t *fit)
{
    int e0[4], e1[4], pal[16][4];
    for (int c = 0; c < 4; c++) {
        e0[c] = fit->q[0][c] * 2 + fit->p[0];
        e1[c] = fit->q[1][c] * 2 + fit->p[1];
    }
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++)
            pal[i][c] = ((64 - kWeights[i]) * e0[c] + kWeights[i] * e1[c] + 32) >> 6;
    fit->err = 0;
    for (int k = 0; k < 16; k++) {
        long best = 0x7fffffff;
        int best_i = 0;
        for (int i = 0; i < 16; i++) {
            long d = 0;
            for (int c = 0; c < 4; c++) {
                const int diff = px[k][c] - pal[i][c];
                d += diff * diff;
            }
            if (d < best) {
                best = d;
                best_i = i;
            }
        }
        fit->idx[k] = best_i;
        fit->err += best;
    }
}

static void set_endpoints(block_fit_t *fit, const float e0[4], const float e1[4], int p0, int p1)
{
    fit->p[0] = p0;
    fit->p[1] = p1;
    for (int c = 0; c < 4; c++) {
        fit->q[0][c] = quantise(e0[c], p0);
        fit->q[1][c] = quantise(e1[c], p1);
    }
}

static void encode_block(const int px[16][4], uint8_t out[16])
{
    float mean[4] = {0, 0, 0, 0};
    for (int k = 0; k < 16; k++)
        for (int c = 0; c < 4; c++)
            mean[c] += (float)px[k][c];
    for (int c = 0; c < 4; c++)
        mean[c] /= 16.0f;
    float cov[4][4] = {{0}};
    for (int k = 0; k < 16; k++)
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                cov[i][j] += ((float)px[k][i] - mean[i]) * ((float)px[k][j] - mean[j]);
    float axis[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    for (int iter = 0; iter < 8; iter++) {
        float next[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                next[i] += cov[i][j] * axis[j];
        const float norm = sqrtf(next[0] * next[0] + next[1] * next[1] + next[2] * next[2] + next[3] * next[3]);
        if (norm < 1e-9f)
            break;
        for (int i = 0; i < 4; i++)
            axis[i] = next[i] / norm;
    }
    float lo = 1e30f, hi = -1e30f;
    for (int k = 0; k < 16; k++) {
        float t = 0.0f;
        for (int c = 0; c < 4; c++)
            t += ((float)px[k][c] - mean[c]) * axis[c];
        lo = fminf(lo, t);
        hi = fmaxf(hi, t);
    }
    float e0[4], e1[4];
    for (int c = 0; c < 4; c++) {
        e0[c] = mean[c] + lo * axis[c];
        e1[c] = mean[c] + hi * axis[c];
    }

    block_fit_t best;
    memset(&best, 0, sizeof(best));
    best.err = 0x7fffffff;
    static const int pbits[2][2] = {{1, 1}, {0, 0}};
    for (int pb = 0; pb < 2; pb++) {
        block_fit_t fit;
        set_endpoints(&fit, e0, e1, pbits[pb][0], pbits[pb][1]);
        choose(px, &fit);
        if (fit.err < best.err)
            best = fit;
        /* one least-squares refit of the endpoints to these indices */
        float a = 0, b = 0, cc = 0, d0[4] = {0}, d1[4] = {0};
        for (int k = 0; k < 16; k++) {
            const float w = (float)kWeights[fit.idx[k]] / 64.0f;
            a += (1 - w) * (1 - w);
            b += (1 - w) * w;
            cc += w * w;
            for (int c = 0; c < 4; c++) {
                d0[c] += (1 - w) * (float)px[k][c];
                d1[c] += w * (float)px[k][c];
            }
        }
        const float det = a * cc - b * b;
        if (fabsf(det) > 1e-6f) {
            float r0[4], r1[4];
            for (int c = 0; c < 4; c++) {
                r0[c] = (cc * d0[c] - b * d1[c]) / det;
                r1[c] = (a * d1[c] - b * d0[c]) / det;
            }
            block_fit_t refit;
            set_endpoints(&refit, r0, r1, pbits[pb][0], pbits[pb][1]);
            choose(px, &refit);
            if (refit.err < best.err)
                best = refit;
        }
    }

    /* the first index is stored in 3 bits: swap the ends when it needs 4 */
    if (best.idx[0] >= 8) {
        for (int c = 0; c < 4; c++) {
            const int t = best.q[0][c];
            best.q[0][c] = best.q[1][c];
            best.q[1][c] = t;
        }
        const int t = best.p[0];
        best.p[0] = best.p[1];
        best.p[1] = t;
        for (int k = 0; k < 16; k++)
            best.idx[k] = 15 - best.idx[k];
    }

    uint64_t lo_bits = 1ull << 6, hi_bits = 0; /* mode 6 */
    int pos = 7;
#define PUT(value, bits)                                                                          \
    do {                                                                                          \
        for (int bit = 0; bit < (bits); bit++, pos++) {                                           \
            const uint64_t v = (uint64_t)(((value) >> bit) & 1);                                  \
            if (pos < 64)                                                                         \
                lo_bits |= v << pos;                                                              \
            else                                                                                  \
                hi_bits |= v << (pos - 64);                                                       \
        }                                                                                         \
    } while (0)
    for (int c = 0; c < 4; c++) {
        PUT(best.q[0][c], 7);
        PUT(best.q[1][c], 7);
    }
    PUT(best.p[0], 1);
    PUT(best.p[1], 1);
    PUT(best.idx[0], 3);
    for (int k = 1; k < 16; k++)
        PUT(best.idx[k], 4);
#undef PUT
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(lo_bits >> (i * 8));
        out[8 + i] = (uint8_t)(hi_bits >> (i * 8));
    }
}

typedef struct {
    const image_t *img;
    uint8_t *blocks;
    int first_row, rows;
} job_t;

static void *encode_rows(void *argument)
{
    const job_t *job = argument;
    const int bw = job->img->w / 4;
    int px[16][4];
    for (int by = job->first_row; by < job->first_row + job->rows; by++)
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    const uint8_t *p = job->img->rgba + ((size_t)(by * 4 + y) * job->img->w + bx * 4 + x) * 4;
                    for (int c = 0; c < 4; c++)
                        px[y * 4 + x][c] = p[c];
                }
            encode_block(px, job->blocks + ((size_t)by * bw + bx) * 16);
        }
    return NULL;
}

void bc7_encode_image(const image_t *img, uint8_t *blocks)
{
    enum { kThreads = 8 };
    const int bh = img->h / 4;
    job_t jobs[kThreads];
    pthread_t threads[kThreads];
    bool started[kThreads];
    const int per = (bh + kThreads - 1) / kThreads;
    for (int t = 0; t < kThreads; t++) {
        const int first = t * per;
        jobs[t] = (job_t){img, blocks, first, first >= bh ? 0 : (first + per > bh ? bh - first : per)};
        started[t] = jobs[t].rows > 0 && pthread_create(&threads[t], NULL, encode_rows, &jobs[t]) == 0;
        if (!started[t] && jobs[t].rows > 0)
            encode_rows(&jobs[t]); /* no thread: do it here */
    }
    for (int t = 0; t < kThreads; t++)
        if (started[t])
            pthread_join(threads[t], NULL);
}
