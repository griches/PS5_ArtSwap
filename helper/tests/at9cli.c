/*
 * Art Swap helper - the snd0.at9 encoder (src/at9.c) on a Mac or Linux host,
 * for tests: reads a WAV, cuts the excerpt the way ps5-at9-converter's command
 * line does (its audio_io.load and cli.run), readies it, writes the .at9, and
 * prints the loudness and the time each step took as "key value" lines.
 *
 *   cc -std=gnu11 -O2 -DARTSWAP_HOST -Ihelper/src helper/tests/at9cli.c helper/src/at9.c \
 *       helper/src/at9_tables.c helper/src/util.c -lm -lpthread -o at9cli
 *   at9cli -i IN.wav -o OUT.at9 [--start TIME] [--duration TIME|max] [--bitrate KBPS]
 *          [--loudness LUFS] [--true-peak DBTP] [--fade-in S] [--fade-out S] [--dump PREPARED.f64] [--ftz]
 *
 * TIME is seconds or [hh:]mm:ss. The WAV may be 8-, 16-, 24- or 32-bit PCM or
 * 32- or 64-bit float, any channel count and rate. --dump writes the readied
 * audio as raw float64, all of the left channel then all of the right. --ftz
 * flushes denormal numbers to zero, as a console process may run (the payload
 * SDK's note on the MXCSR), where the host can do it (macOS on arm64).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "at9.h"

#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int channels, rate;
    size_t frames;
    double **pcm; /* [channels][frames], full scale 1.0 */
} wav_t;

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | rd16(p + 2) << 16;
}

static void wav_free(wav_t *w)
{
    for (int c = 0; w->pcm != NULL && c < w->channels; c++)
        free(w->pcm[c]);
    free(w->pcm);
    memset(w, 0, sizeof(*w));
}

/* As the converter's wav.read_wav: integer PCM left-justified into 32 bits over 2^31, 8-bit unsigned around 128 */
static bool wav_read(const buf_t *raw, wav_t *w, char *err, size_t errlen)
{
    memset(w, 0, sizeof(*w));
    const uint8_t *d = raw->data;
    if (raw->len < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) {
        snprintf(err, errlen, "not a RIFF/WAVE file");
        return false;
    }
    const uint8_t *fmt = NULL, *data = NULL;
    size_t fmt_len = 0, data_len = 0, pos = 12;
    while (pos + 8 <= raw->len) {
        const uint32_t size = rd32(d + pos + 4);
        const size_t avail = raw->len - pos - 8;
        const size_t body = size < avail ? size : avail;
        if (memcmp(d + pos, "fmt ", 4) == 0) {
            fmt = d + pos + 8;
            fmt_len = body;
        } else if (memcmp(d + pos, "data", 4) == 0) {
            data = d + pos + 8;
            data_len = body;
            break;
        }
        pos += 8 + (size_t)size + (size & 1);
    }
    if (fmt == NULL || data == NULL || fmt_len < 16) {
        snprintf(err, errlen, "missing fmt or data chunk");
        return false;
    }
    uint32_t tag = rd16(fmt);
    const int channels = (int)rd16(fmt + 2), bits = (int)rd16(fmt + 14);
    size_t align = rd16(fmt + 12);
    if (tag == 0xFFFE && fmt_len >= 40)
        tag = rd16(fmt + 24);
    if (channels < 1 || bits == 0 || (tag != 1 && tag != 3)) {
        snprintf(err, errlen, "unsupported WAVE format (tag 0x%04x, %d bits)", tag, bits);
        return false;
    }
    const int width = tag == 3 ? bits / 8 : (bits + 7) / 8;
    if ((tag == 3 && width != 4 && width != 8) || (tag == 1 && (width < 1 || width > 4))) {
        snprintf(err, errlen, "unsupported sample width %d", width);
        return false;
    }
    if (align < (size_t)channels * (size_t)width)
        align = (size_t)channels * (size_t)width;
    w->channels = channels;
    w->rate = (int)rd32(fmt + 4);
    w->frames = data_len / align;
    w->pcm = calloc((size_t)channels, sizeof(double *));
    for (int c = 0; w->pcm != NULL && c < channels; c++)
        w->pcm[c] = malloc((w->frames ? w->frames : 1) * sizeof(double));
    for (int c = 0; w->pcm != NULL && c < channels; c++) {
        if (w->pcm[c] == NULL) {
            wav_free(w);
            break;
        }
    }
    if (w->pcm == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (size_t i = 0; i < w->frames; i++) {
        for (int c = 0; c < channels; c++) {
            const uint8_t *p = data + i * align + (size_t)c * (size_t)width;
            double v;
            if (tag == 3 && width == 4) {
                float f;
                memcpy(&f, p, 4);
                v = f;
            } else if (tag == 3) {
                memcpy(&v, p, 8);
            } else if (width == 1) {
                v = ((double)p[0] - 128.0) / 128.0;
            } else {
                uint32_t u = 0;
                for (int b = 0; b < width; b++)
                    u |= (uint32_t)p[b] << (8 * (4 - width + b));
                v = (double)(int32_t)u / 2147483648.0;
            }
            w->pcm[c][i] = v;
        }
    }
    return true;
}

/* Seconds from "90", "90.5", "1:30" or "1:02:03.5" */
static bool parse_time(const char *text, double *out)
{
    double value = 0.0;
    int parts = 0;
    const char *p = text;
    for (;;) {
        char *end;
        const double v = strtod(p, &end);
        if (end == p)
            return false;
        value = value * 60.0 + v;
        parts++;
        if (*end == ':' && parts < 3) {
            p = end + 1;
            continue;
        }
        if (*end != 0)
            return false;
        break;
    }
    *out = value;
    return value >= 0 && isfinite(value);
}

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int usage(void)
{
    fprintf(stderr, "usage: at9cli -i IN.wav -o OUT.at9 [--start TIME] [--duration TIME|max] [--bitrate KBPS]\n"
                    "              [--loudness LUFS] [--true-peak DBTP] [--fade-in S] [--fade-out S]"
                    " [--dump PREPARED.f64] [--ftz]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL, *dump = NULL;
    double start = 0.0, duration = 15.0, target = AT9_DEFAULT_LUFS, ceiling = AT9_DEFAULT_TRUE_PEAK;
    double fade_in = AT9_DEFAULT_FADE, fade_out = AT9_DEFAULT_FADE;
    bool max_duration = false;
    int bitrate = AT9_DEFAULT_BITRATE;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--ftz") == 0) {
#ifdef FE_DFL_DISABLE_DENORMS_ENV
            fesetenv(FE_DFL_DISABLE_DENORMS_ENV);
            continue;
#else
            fprintf(stderr, "error: --ftz is not available on this host\n");
            return 2;
#endif
        }
        if (v == NULL)
            return usage();
        i++;
        if (strcmp(a, "-i") == 0)
            input = v;
        else if (strcmp(a, "-o") == 0)
            output = v;
        else if (strcmp(a, "--dump") == 0)
            dump = v;
        else if (strcmp(a, "--start") == 0) {
            if (!parse_time(v, &start))
                return usage();
        } else if (strcmp(a, "--duration") == 0) {
            if (strcmp(v, "max") == 0)
                max_duration = true;
            else if (!parse_time(v, &duration) || duration <= 0)
                return usage();
        } else if (strcmp(a, "--bitrate") == 0)
            bitrate = atoi(v);
        else if (strcmp(a, "--loudness") == 0)
            target = strtod(v, NULL);
        else if (strcmp(a, "--true-peak") == 0)
            ceiling = strtod(v, NULL);
        else if (strcmp(a, "--fade-in") == 0)
            fade_in = strtod(v, NULL);
        else if (strcmp(a, "--fade-out") == 0)
            fade_out = strtod(v, NULL);
        else
            return usage();
    }
    if (input == NULL || output == NULL)
        return usage();
    const size_t max_samples = at9_max_samples(bitrate);
    if (max_samples == 0) {
        fprintf(stderr, "error: %d kb/s is not a stereo ATRAC9 bit rate\n", bitrate);
        return 1;
    }
    char err[256];
    buf_t raw = {0};
    wav_t wav;
    if (!file_read(input, &raw)) {
        fprintf(stderr, "error: cannot read %s\n", input);
        return 1;
    }
    if (!wav_read(&raw, &wav, err, sizeof(err))) {
        fprintf(stderr, "error: %s: %s\n", input, err);
        return 1;
    }
    buf_free(&raw);
    printf("input %d %d %zu\n", wav.rate, wav.channels, wav.frames);

    /* The excerpt with up to 0.25 s either side, so the resampler has real audio at the cut */
    const double t0 = seconds();
    const double margin = 0.25, begin = start - margin > 0.0 ? start - margin : 0.0, offset = start - begin;
    const size_t a = (size_t)floor(begin * wav.rate);
    size_t b = wav.frames;
    if (!max_duration) {
        const size_t want = a + (size_t)ceil((duration + offset + margin) * wav.rate);
        b = want < b ? want : b;
    }
    if (a >= b) {
        fprintf(stderr, "error: --start is beyond the end of the input\n");
        return 1;
    }
    const double **in = calloc((size_t)wav.channels, sizeof(double *));
    for (int c = 0; c < wav.channels; c++)
        in[c] = wav.pcm[c] + a;
    double *l, *r;
    size_t frames;
    if (!at9_to_stereo_48k(in, wav.channels, b - a, wav.rate, &l, &r, &frames, err, sizeof(err))) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    free(in);
    wav_free(&wav);
    const size_t skip = (size_t)rint(offset * AT9_RATE);
    const size_t available = frames > skip ? frames - skip : 0;
    size_t count = available;
    if (max_duration)
        count = count < max_samples ? count : max_samples;
    else {
        const size_t want = (size_t)rint(duration * AT9_RATE);
        count = count < want ? count : want;
    }
    if (count == 0) {
        fprintf(stderr, "error: the selected excerpt is empty\n");
        return 1;
    }
    memmove(l, l + skip, count * sizeof(double));
    memmove(r, r + skip, count * sizeof(double));
    const double t1 = seconds();
    printf("count %zu\n", count);

    double source, final;
    if (!at9_prepare(l, r, count, target, ceiling, fade_in, fade_out, &source, &final)) {
        fprintf(stderr, "error: out of memory readying the audio\n");
        return 1;
    }
    const double t2 = seconds();
    printf("source_lufs %.9f\nfinal_lufs %.9f\n", source, final);
    if (dump != NULL) {
        FILE *f = fopen(dump, "wb");
        if (f == NULL || fwrite(l, sizeof(double), count, f) != count || fwrite(r, sizeof(double), count, f) != count) {
            fprintf(stderr, "error: cannot write %s\n", dump);
            return 1;
        }
        fclose(f);
    }

    buf_t file = {0};
    if (!at9_encode(l, r, count, bitrate, &file, err, sizeof(err))) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    const double t3 = seconds();
    if (!file_write_replace(output, file.data, file.len, NULL)) {
        fprintf(stderr, "error: cannot write %s\n", output);
        return 1;
    }
    printf("bytes %zu\ntime_stereo %.3f\ntime_prepare %.3f\ntime_encode %.3f\n", file.len, t1 - t0, t2 - t1, t3 - t2);
    buf_free(&file);
    free(l);
    free(r);
    return 0;
}
