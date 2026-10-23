/*
 * Art Swap helper - a title's home-screen music (music.h).
 *
 * The home screen plays a title's sce_sys/snd0.at9 while its tile is
 * selected: ATRAC9 at 48 kHz in a RIFF file with a whole-track loop, at most
 * 2 MiB, and as loud as -28 LUFS. The person's own music (MP3, FLAC, WAV or
 * AIFF, sent from a phone or copied to /data/artswap/music) is decoded here
 * (minimp3, dr_flac, dr_wav), a clip of it prepared and encoded as
 * ps5-at9-converter does (at9.c), and written as the art is: over each
 * snd0.at9 the title has, its original kept once as snd0.at9.old. A title
 * with none has nothing for the home screen to play until an install
 * registers one (ShadowMountPlus writes snd0Info when it installs a title
 * whose sce_sys has a snd0.at9), so the file is added to its folder and Apply
 * changes installs it again, as for a background it never had.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "music.h"

#include "art.h"
#include "at9.h"

#include "../third_party/cJSON.h"
#include "../third_party/dr_flac.h"
#include "../third_party/dr_wav.h"
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#include "../third_party/minimp3_ex.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define BITRATE 192
#define LOUDNESS -28.0
#define TRUE_PEAK -2.0
#define FADE_S 0.01
#define MARGIN_S 0.25 /* decoded either side of the clip, so the resampling can be cut cleanly (audio_io.load) */
#define LEVELS 200

static const char *kKinds[] = {".mp3", ".flac", ".wav", ".aif", ".aiff"};

const char *music_folder(char *out, size_t size)
{
    data_path("music", out, size);
    mkdirs(out);
    return out;
}

double music_max_seconds(void)
{
    return (double)at9_max_samples(BITRATE) / 48000.0;
}

static bool is_music(const char *name)
{
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot && name[0] != '.' && i < sizeof(kKinds) / sizeof(kKinds[0]); i++)
        if (strcasecmp(dot, kKinds[i]) == 0)
            return true;
    return false;
}

/* A name the music folder holds: no folders, no hidden files, one of the kinds read here */
static bool name_ok(const char *name)
{
    return name[0] && strchr(name, '/') == NULL && strstr(name, "..") == NULL && strlen(name) < 200 && is_music(name);
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

void music_list(buf_t *out)
{
    char folder[512];
    DIR *dir = opendir(music_folder(folder, sizeof(folder)));
    if (dir == NULL)
        return;
    char *names[512];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < 512)
        if (name_ok(e->d_name))
            names[n++] = strdup(e->d_name);
    closedir(dir);
    qsort(names, (size_t)n, sizeof(names[0]), by_name);
    for (int i = 0; i < n; i++) {
        char path[800];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", folder, names[i]);
        if (names[i] && stat(path, &st) == 0) {
            buf_printf(out, "S\t");
            buf_field(out, names[i]);
            buf_printf(out, "\t%lld\n", (long long)st.st_size);
        }
        free(names[i]);
    }
}

bool music_upload_name(const char *wanted, char *out, size_t size)
{
    /* the last part of what the phone called it, with only plain characters */
    const char *base = strrchr(wanted, '/');
    base = base ? base + 1 : wanted;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)base; *p && o + 1 < size && o < 120; p++)
        out[o++] = isalnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == ' ' || *p == '(' || *p == ')'
                       ? (char)*p
                       : '_';
    out[o] = 0;
    while (out[0] == '.' || out[0] == ' ')
        memmove(out, out + 1, strlen(out));
    return name_ok(out);
}

/* ---- reading a file ---- */

typedef enum { SONG_WAV, SONG_FLAC, SONG_MP3 } song_kind_t;

typedef struct {
    song_kind_t kind;
    drwav wav;
    drflac *flac;
    mp3dec_ex_t mp3;
    buf_t mp3_data; /* minimp3 reads from memory */
    int rate, channels;
    uint64_t frames;
} song_t;

static bool song_open(const char *name, song_t *song, char *err, size_t errlen)
{
    memset(song, 0, sizeof(*song));
    char folder[512], path[800];
    if (!name_ok(name)) {
        snprintf(err, errlen, "%s is not a music file Art Swap reads (MP3, FLAC, WAV or AIFF)", name);
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s", music_folder(folder, sizeof(folder)), name);
    const char *dot = strrchr(name, '.');
    bool ok = false;
    if (strcasecmp(dot, ".mp3") == 0) {
        song->kind = SONG_MP3;
        ok = file_read(path, &song->mp3_data) &&
             mp3dec_ex_open_buf(&song->mp3, song->mp3_data.data, song->mp3_data.len, MP3D_SEEK_TO_SAMPLE) == 0 &&
             song->mp3.info.channels > 0 && song->mp3.info.hz > 0;
        if (ok) {
            song->channels = song->mp3.info.channels;
            song->rate = song->mp3.info.hz;
            song->frames = song->mp3.samples / (uint64_t)song->channels;
        } else
            buf_free(&song->mp3_data);
    } else if (strcasecmp(dot, ".flac") == 0) {
        song->kind = SONG_FLAC;
        song->flac = drflac_open_file(path, NULL);
        ok = song->flac != NULL;
        if (ok) {
            song->channels = song->flac->channels;
            song->rate = (int)song->flac->sampleRate;
            song->frames = song->flac->totalPCMFrameCount;
        }
    } else {
        song->kind = SONG_WAV;
        ok = drwav_init_file(&song->wav, path, NULL);
        if (ok) {
            song->channels = song->wav.channels;
            song->rate = (int)song->wav.sampleRate;
            song->frames = song->wav.totalPCMFrameCount;
        }
    }
    if (ok && (song->channels < 1 || song->channels > 8 || song->rate < 8000 || song->rate > 192000 ||
               song->frames == 0)) {
        ok = false;
        if (song->kind == SONG_FLAC)
            drflac_close(song->flac);
        else if (song->kind == SONG_WAV)
            drwav_uninit(&song->wav);
        else {
            mp3dec_ex_close(&song->mp3);
            buf_free(&song->mp3_data);
        }
    }
    if (!ok)
        snprintf(err, errlen, "%s could not be read as music", name);
    return ok;
}

/* Up to frames frames from where the song is, interleaved; how many */
static size_t song_read(song_t *song, float *out, size_t frames)
{
    if (song->kind == SONG_FLAC)
        return (size_t)drflac_read_pcm_frames_f32(song->flac, frames, out);
    if (song->kind == SONG_WAV)
        return (size_t)drwav_read_pcm_frames_f32(&song->wav, frames, out);
    return mp3dec_ex_read(&song->mp3, out, frames * (size_t)song->channels) / (size_t)song->channels;
}

static bool song_seek(song_t *song, uint64_t frame)
{
    if (song->kind == SONG_FLAC)
        return drflac_seek_to_pcm_frame(song->flac, frame);
    if (song->kind == SONG_WAV)
        return drwav_seek_to_pcm_frame(&song->wav, frame);
    return mp3dec_ex_seek(&song->mp3, frame * (uint64_t)song->channels) == 0;
}

static void song_close(song_t *song)
{
    if (song->kind == SONG_FLAC)
        drflac_close(song->flac);
    else if (song->kind == SONG_WAV)
        drwav_uninit(&song->wav);
    else {
        mp3dec_ex_close(&song->mp3);
        buf_free(&song->mp3_data);
    }
}

bool music_info(const char *name, buf_t *out, char *err, size_t errlen)
{
    song_t song;
    if (!song_open(name, &song, err, errlen))
        return false;
    /* the loudest sample of each of LEVELS stretches of it */
    float peaks[LEVELS] = {0};
    const size_t chunk = 4096;
    float *pcm = malloc(chunk * (size_t)song.channels * sizeof(float));
    uint64_t at = 0;
    size_t got;
    while (pcm != NULL && (got = song_read(&song, pcm, chunk)) > 0) {
        for (size_t i = 0; i < got; i++, at++) {
            const int level = (int)(at * LEVELS / (song.frames ? song.frames : 1));
            float peak = 0.0f;
            for (int c = 0; c < song.channels; c++)
                peak = fmaxf(peak, fabsf(pcm[i * (size_t)song.channels + (size_t)c]));
            if (level < LEVELS && peak > peaks[level])
                peaks[level] = peak;
        }
    }
    free(pcm);
    /* a decoder's count can be an estimate (an MP3 without an index): what was read is the length */
    const double seconds = (double)(at ? at : song.frames) / song.rate;
    buf_printf(out, "song\t");
    buf_field(out, name);
    buf_printf(out, "\t%.3f\t%d\t%d\nW", seconds, song.rate, song.channels);
    for (int i = 0; i < LEVELS; i++)
        buf_printf(out, "\t%d", (int)lrintf(fminf(1.0f, peaks[i]) * 99.0f));
    buf_printf(out, "\n");
    song_close(&song);
    return true;
}

/* The clip, prepared as ps5-at9-converter's cli.py prepares it: decoded from a little before start, made stereo
 * at 48 kHz, cut, brought to -28 LUFS, its true peak held under -2 dBTP, and faded at both ends for the loop */
static bool prepare(const char *name, double start, double seconds, double **left, double **right, size_t *count,
                    double *source_lufs, double *final_lufs, char *err, size_t errlen)
{
    *left = *right = NULL;
    if (!(seconds > 0.0) || start < 0.0) {
        snprintf(err, errlen, "The clip is empty");
        return false;
    }
    seconds = fmin(seconds, music_max_seconds());
    song_t song;
    if (!song_open(name, &song, err, errlen))
        return false;
    const double begin = fmax(0.0, start - MARGIN_S), offset = start - begin;
    const uint64_t first = (uint64_t)floor(begin * song.rate);
    const size_t want = (size_t)ceil((seconds + offset + MARGIN_S) * song.rate);
    bool ok = first < song.frames && song_seek(&song, first);
    if (!ok)
        snprintf(err, errlen, "The clip starts after the end of %s", name);
    float *pcm = ok ? malloc(want * (size_t)song.channels * sizeof(float)) : NULL;
    size_t got = 0;
    if (ok && pcm == NULL) {
        snprintf(err, errlen, "out of memory");
        ok = false;
    }
    while (ok && got < want) {
        const size_t read = song_read(&song, pcm + got * (size_t)song.channels, want - got);
        if (read == 0)
            break;
        got += read;
    }
    const int channels = song.channels, rate = song.rate;
    song_close(&song);
    if (ok && got == 0) {
        snprintf(err, errlen, "The clip starts after the end of %s", name);
        ok = false;
    }
    /* planar doubles, as the converter holds them */
    double *planes[8] = {0};
    for (int c = 0; ok && c < channels; c++) {
        planes[c] = malloc(got * sizeof(double));
        ok = planes[c] != NULL;
        for (size_t i = 0; ok && i < got; i++)
            planes[c][i] = pcm[i * (size_t)channels + (size_t)c];
    }
    free(pcm);
    double *l = NULL, *r = NULL;
    size_t frames = 0;
    if (ok)
        ok = at9_to_stereo_48k((const double *const *)planes, channels, got, rate, &l, &r, &frames, err, errlen);
    for (int c = 0; c < channels; c++)
        free(planes[c]);
    /* as cli.py cuts it: Python's round() takes a half to the even side, as rint does */
    const size_t skip = (size_t)rint(offset * 48000.0);
    const size_t n = ok && frames > skip ? (size_t)fmin((double)(frames - skip), rint(seconds * 48000.0)) : 0;
    if (ok && n == 0) {
        snprintf(err, errlen, "The clip is empty");
        ok = false;
    }
    if (ok) {
        memmove(l, l + skip, n * sizeof(double));
        memmove(r, r + skip, n * sizeof(double));
        ok = at9_prepare(l, r, n, LOUDNESS, TRUE_PEAK, FADE_S, FADE_S, source_lufs, final_lufs);
        if (!ok)
            snprintf(err, errlen, "The clip could not be prepared");
    }
    if (!ok) {
        free(l);
        free(r);
        return false;
    }
    *left = l;
    *right = r;
    *count = n;
    return true;
}

bool music_clip(const char *name, double start, double seconds, buf_t *out, char *err, size_t errlen)
{
    double *l, *r, source, final;
    size_t n;
    const uint64_t began = now_ms();
    if (!prepare(name, start, seconds, &l, &r, &n, &source, &final, err, errlen))
        return false;
    buf_printf(out, "clip\t%zu\t%.2f\t%.2f\n", n, source, final);
    bool ok = buf_reserve(out, n * 4);
    for (size_t i = 0; ok && i < n; i++) {
        const long a = lrint(fmax(-32768.0, fmin(32767.0, l[i] * 32768.0)));
        const long b = lrint(fmax(-32768.0, fmin(32767.0, r[i] * 32768.0)));
        const uint8_t s[4] = {(uint8_t)(a & 255), (uint8_t)((a >> 8) & 255), (uint8_t)(b & 255),
                              (uint8_t)((b >> 8) & 255)};
        memcpy(out->data + out->len, s, 4);
        out->len += 4;
    }
    free(l);
    free(r);
    if (!ok)
        snprintf(err, errlen, "out of memory");
    log_line("music: clip of %s from %.2f s, %.2f s, %.1f LUFS to %.1f, in %llu ms", name, start, (double)n / 48000.0,
             source, final, (unsigned long long)(now_ms() - began));
    return ok;
}

/* pubtools.loudnessSnd0 in each param.json beside the title's art, as the converter writes it: the home screen's
 * level for the music. The rest of each file stays as it was. */
static void set_loudness(const app_t *app, double lufs)
{
    char done[8][420];
    int n = 0;
    for (int i = 0; i < app->nfiles && n < 8; i++) {
        char path[420];
        const char *slash = strrchr(app->files[i].path, '/');
        if (slash == NULL)
            continue;
        snprintf(path, sizeof(path), "%.*s/param.json", (int)(slash - app->files[i].path), app->files[i].path);
        bool seen = false;
        for (int j = 0; j < n && !seen; j++)
            seen = strcmp(done[j], path) == 0;
        if (seen)
            continue;
        copy_str(done[n++], sizeof(done[0]), path);
        char real[512];
        buf_t text = {0};
        if (!file_read(cpath(path, real, sizeof(real)), &text) || text.len == 0) {
            buf_free(&text);
            continue;
        }
        cJSON *root = cJSON_ParseWithLength((const char *)text.data, text.len);
        buf_free(&text);
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            continue;
        }
        cJSON *pubtools = cJSON_GetObjectItemCaseSensitive(root, "pubtools");
        if (!cJSON_IsObject(pubtools)) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "pubtools");
            pubtools = cJSON_AddObjectToObject(root, "pubtools");
        }
        char value[16];
        snprintf(value, sizeof(value), "%.2f", lufs);
        cJSON_DeleteItemFromObjectCaseSensitive(pubtools, "loudnessSnd0");
        cJSON_AddStringToObject(pubtools, "loudnessSnd0", value);
        char *printed = cJSON_Print(root);
        if (printed != NULL && !file_write_replace(real, printed, strlen(printed), real))
            log_line("music: %s could not be written", path);
        cJSON_free(printed);
        cJSON_Delete(root);
    }
}

bool music_apply(const app_t *app, const char *folder, const char *name, double start, double seconds, bool *added,
                 buf_t *report, char *err, size_t errlen)
{
    double *l, *r, source, final;
    size_t n;
    const uint64_t began = now_ms();
    if (!prepare(name, start, seconds, &l, &r, &n, &source, &final, err, errlen))
        return false;
    buf_t at9 = {0};
    bool ok = at9_encode(l, r, n, BITRATE, &at9, err, errlen);
    free(l);
    free(r);
    int targets = 0;
    for (int i = 0; i < app->nfiles; i++)
        targets += strcmp(app->files[i].slot, "snd0") == 0;
    *added = targets == 0;
    /* music Art Swap added before has no original: its copies are listed with it */
    const bool was_added = art_added(app->id, "snd0");
    int written = 0;
    for (int i = 0; ok && i < app->nfiles; i++) {
        const art_file_t *file = &app->files[i];
        if (strcmp(file->slot, "snd0") != 0)
            continue;
        char real[512], old[420], real_old[512];
        cpath(file->path, real, sizeof(real));
        snprintf(old, sizeof(old), "%s.old", file->path);
        cpath(old, real_old, sizeof(real_old));
        if (was_added)
            art_added_note(app->id, file->path);
        else if (!file_exists(real_old)) {
            ok = file_copy(real, real_old);
            if (ok)
                buf_printf(report, "O\t%s\n", old);
            else
                snprintf(err, errlen, "the original could not be kept as %s", old);
        }
        if (ok && !file_write_replace(real, at9.data, at9.len, real)) {
            snprintf(err, errlen, "%s could not be written", file->path);
            ok = false;
        }
        if (ok) {
            buf_printf(report, "W\t%s\tat9 %.1fs\n", file->path, (double)n / 48000.0);
            written++;
        }
    }
    if (ok && *added) {
        if (folder == NULL || !folder[0]) {
            snprintf(err, errlen, "%s has no music, and Art Swap adds music only to homebrew ShadowMountPlus "
                                  "installed from a folder",
                     app->name);
            ok = false;
        } else {
            char dest[440], real[512], icon[440], like[512];
            snprintf(dest, sizeof(dest), "%s/sce_sys/snd0.at9", folder);
            snprintf(icon, sizeof(icon), "%s/sce_sys/icon0.png", folder);
            cpath(icon, like, sizeof(like));
            ok = file_write_replace(cpath(dest, real, sizeof(real)), at9.data, at9.len,
                                    file_exists(like) ? like : NULL);
            if (ok) {
                art_added_note(app->id, dest);
                buf_printf(report, "W\t%s\tat9 %.1fs\n", dest, (double)n / 48000.0);
                written++;
            } else
                snprintf(err, errlen, "%s could not be written", dest);
        }
    }
    if (ok)
        set_loudness(app, final);
    buf_free(&at9);
    log_line("music %s: %s from %.2f s, %.2f s, %d files%s in %llu ms%s%s", app->id, name, start, (double)n / 48000.0,
             written, *added ? " (added)" : "", (unsigned long long)(now_ms() - began), ok ? "" : ": ", ok ? "" : err);
    return ok;
}
