/*
 * Art Swap helper - a title's home-screen music (snd0.at9), made on the console from a file of the person's.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "scan.h"

/* The longest clip a snd0.at9 holds (2 MiB at 192 kb/s), in seconds */
double music_max_seconds(void);
/* "S name bytes" a line: the music in /data/artswap/music (MP3, FLAC, WAV, AIFF), A to Z */
void music_list(buf_t *out);
/* "song name seconds rate channels", then "W" and 200 levels (0-99): how loud the file is along its length */
bool music_info(const char *name, buf_t *out, char *err, size_t errlen);
/* The clip as the home screen will play it: "clip frames source_lufs final_lufs" and a line break, then the
 * frames as interleaved stereo 16-bit samples at 48 kHz */
bool music_clip(const char *name, double start, double seconds, buf_t *out, char *err, size_t errlen);
/* snd0.at9 made from the clip, written over each of the title's (its original kept once), or, when it has
 * none, added to its folder's sce_sys (folder: as ShadowMountPlus names it), for an install to take in.
 * *added says which. report gets "O" and "W" lines, as APPLY's. Called with g_apply_lock held. */
bool music_apply(const app_t *app, const char *folder, const char *name, double start, double seconds, bool *added,
                 buf_t *report, char *err, size_t errlen);
/* A file name for an upload, safe to keep in the music folder, or false */
bool music_upload_name(const char *wanted, char *out, size_t size);
/* Where an upload goes: the music folder, made if need be */
const char *music_folder(char *out, size_t size);
