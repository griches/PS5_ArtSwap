/*
 * Art Swap helper - snd0.at9: the music a title's tile plays while it is
 * highlighted on the home screen, made on the console itself.
 *
 * The audio is mapped to 48 kHz stereo, brought to the loudness the home
 * screen expects (the value that goes in param.json's pubtools.loudnessSnd0,
 * -28 LUFS by default), held under a true-peak ceiling, faded at both ends so
 * the loop point does not click, and encoded as ATRAC9 at 72-192 kb/s in the
 * RIFF container Sony's at9tool writes, with a loop over the whole track. The
 * console refuses a snd0.at9 larger than 2 MiB.
 *
 * Audio is planar double PCM, full scale 1.0: one array per channel, as
 * numpy's [channels, samples] in the converter this comes from. Every call
 * keeps its own state, so calls on different threads do not meet.
 *
 * The converter's order, which helper/tests/at9cli.c follows: decode the
 * excerpt with up to 0.25 s more on each side, at9_to_stereo_48k, drop
 * rint(margin * 48000) samples from the front and keep rint(seconds * 48000)
 * (at most at9_max_samples), at9_prepare, at9_encode. The margin gives the
 * resampler real audio at the cut.
 *
 * Ported from ps5-at9-converter's ps5at9/atrac9_encoder.py, atrac9.py,
 * at9file.py and dsp.py (Copyright (C) 2026 BlackBearReloaded,
 * GPL-3.0-or-later). The codec's constant tables (at9_tables.c) are
 * LibAtrac9's, Copyright (c) 2018 Alex Barney, under the MIT License.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

#define AT9_RATE 48000
#define AT9_SIZE_LIMIT (2 * 1024 * 1024) /* the largest snd0.at9 the console plays */
#define AT9_ENCODER_DELAY 256            /* samples the decoder puts before the music */
#define AT9_DEFAULT_BITRATE 192
#define AT9_DEFAULT_LUFS (-28.0)
#define AT9_DEFAULT_TRUE_PEAK (-2.0)
#define AT9_DEFAULT_FADE 0.01            /* seconds, at each end */

/* Mono, stereo or 5.1 (FL FR FC LFE BL BR, downmixed as ITU-R BS.775) audio at
 * any sample rate, as 48 kHz stereo; other channel counts keep their first two.
 * left and right are new arrays of out_frames samples, for the caller to free().
 * False (err says why) for empty audio, a rate it cannot convert, or no memory. */
bool at9_to_stereo_48k(const double *const *in, int channels, size_t frames, int rate, double **left,
                       double **right, size_t *out_frames, char *err, size_t errlen);

/* Integrated loudness of 48 kHz stereo audio in LUFS (ITU-R BS.1770-4: K-weighting,
 * 400 ms blocks every 100 ms, the absolute and relative gates); -INFINITY for
 * silence, NAN when there is no memory to measure with. */
double at9_integrated_loudness(const double *l, const double *r, size_t n);

/* Readies 48 kHz stereo audio for at9_encode, in place: normalised to target_lufs
 * (skipped for silence, or when target_lufs is not finite, which keeps the
 * source level), peaks limited to ceiling_dbtp (true peak, 4x oversampled),
 * then raised-cosine fades in and out. source_lufs and final_lufs (either may be
 * NULL) get the loudness before and after; final_lufs is the value for
 * pubtools.loudnessSnd0. Needs one more mono array of n doubles while it limits
 * (32 MiB for the longest snd0) and 2.5 MiB to measure. False only when memory
 * runs out. */
bool at9_prepare(double *l, double *r, size_t n, double target_lufs, double ceiling_dbtp, double fade_in_s,
                 double fade_out_s, double *source_lufs, double *final_lufs);

/* The most samples whose snd0.at9 (with its loop) fits the 2 MiB limit at a bit
 * rate (72, 96, 120, 144, 168 or 192 kb/s); 0 for another rate */
size_t at9_max_samples(int bitrate_kbps);
/* The size of the snd0.at9 for that many samples; 0 for a bit rate it does not make */
size_t at9_file_size(size_t samples, int bitrate_kbps);

/* The complete snd0.at9 for 48 kHz stereo audio (full scale 1.0, as at9_prepare
 * leaves it) at bitrate_kbps, looping the whole track, into file (its contents are
 * replaced). Refuses a file over 2 MiB, empty audio and samples that are not
 * finite. Single-threaded; the working memory is about half a megabyte beyond
 * the file, whatever the length. */
bool at9_encode(const double *l, const double *r, size_t n, int bitrate_kbps, buf_t *file, char *err,
                size_t errlen);

/* ---- the codec's constant tables (at9_tables.c), for at9.c ---- */
extern const uint8_t at9_sf_unsigned_bits[7][64];  /* by word length 1..6 */
extern const uint8_t at9_sf_unsigned_codes[7][64];
extern const uint8_t at9_sf_signed_bits[6][32];    /* by word length 2..5; 0: no code */
extern const uint8_t at9_sf_signed_codes[6][32];
extern const uint8_t at9_spectrum_bits[2][8][4][256]; /* by codebook set, word length 2..7, codebook */
extern const uint16_t at9_spectrum_codes[2][8][4][256];
extern const uint8_t at9_spectrum_vcp[8][4];       /* values per symbol (log2), by word length, codebook */
extern const uint8_t at9_sf_weights[8][32];
extern const uint8_t at9_gradient_main[48];
