/*
 * Art Swap helper - the person's own pictures (pictures.h).
 *
 * Pictures the person sends from a phone or a computer (phone.c), or copies
 * to /data/artswap/pictures, are offered first in the picker, for an icon, a
 * background or a logo. The title names one as mine:<name>, which sgdb_fetch
 * reads from the folder (never through the download cache), so THUMB,
 * PREVIEW, APPLY and LOGO take them as they take SteamGridDB's.
 *
 * The list reads only each file's head: a PNG's IHDR (and whether a tRNS
 * chunk comes before its pictures), a JPEG's frame header.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pictures.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIST_MAX 512

static const char *kKinds[] = {".png", ".jpg", ".jpeg"};

const char *pictures_folder(char *out, size_t size)
{
    data_path("pictures", out, size);
    mkdirs(out);
    return out;
}

bool pictures_name_ok(const char *name)
{
    const size_t len = strlen(name);
    return len > 0 && len <= 200 && name[0] != '.' && strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

/* ---- the files' heads ---- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool probe_png(FILE *f, int *w, int *h, bool *alpha)
{
    uint8_t head[33];
    if (fread(head, 1, sizeof(head), f) != sizeof(head) || memcmp(head, "\x89PNG\r\n\x1a\n", 8) != 0 ||
        memcmp(head + 12, "IHDR", 4) != 0)
        return false;
    *w = (int)be32(head + 16);
    *h = (int)be32(head + 20);
    const uint8_t colour = head[25];
    *alpha = colour == 4 || colour == 6;
    /* a tRNS chunk, which makes a colour (or a palette entry) see-through, comes before the first IDAT */
    for (int chunks = 0; !*alpha && chunks < 64; chunks++) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), f) != sizeof(chunk))
            break;
        if (memcmp(chunk + 4, "tRNS", 4) == 0)
            *alpha = true;
        if (memcmp(chunk + 4, "IDAT", 4) == 0 || memcmp(chunk + 4, "IEND", 4) == 0)
            break;
        if (fseek(f, (long)be32(chunk) + 4, SEEK_CUR) != 0) /* the data and its CRC */
            break;
    }
    return *w > 0 && *h > 0;
}

static bool probe_jpeg(FILE *f, int *w, int *h)
{
    uint8_t soi[2];
    if (fread(soi, 1, 2, f) != 2 || soi[0] != 0xff || soi[1] != 0xd8)
        return false;
    /* segments until a frame header (SOF0-15, but DHT, JPG and DAC), which holds the size */
    for (int segments = 0; segments < 256; segments++) {
        int c = fgetc(f);
        if (c != 0xff)
            return false;
        while ((c = fgetc(f)) == 0xff)
            ;
        if (c == EOF || c == 0xd9 || c == 0xda) /* the end, or the pictures' data, with no frame header */
            return false;
        if (c == 0x01 || (c >= 0xd0 && c <= 0xd7))
            continue; /* no length */
        uint8_t len[2];
        if (fread(len, 1, 2, f) != 2)
            return false;
        const int length = len[0] << 8 | len[1];
        if (length < 2)
            return false;
        if (c >= 0xc0 && c <= 0xcf && c != 0xc4 && c != 0xc8 && c != 0xcc) {
            uint8_t frame[5];
            if (fread(frame, 1, 5, f) != 5)
                return false;
            *h = frame[1] << 8 | frame[2];
            *w = frame[3] << 8 | frame[4];
            return *w > 0 && *h > 0;
        }
        if (fseek(f, length - 2, SEEK_CUR) != 0)
            return false;
    }
    return false;
}

bool pictures_probe(const char *path, int *w, int *h, bool *alpha)
{
    *w = *h = 0;
    *alpha = false;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    bool ok = probe_png(f, w, h, alpha);
    if (!ok) {
        rewind(f);
        *alpha = false;
        ok = probe_jpeg(f, w, h);
    }
    fclose(f);
    return ok;
}

/* ---- the folder ---- */

typedef struct {
    char *name;
    long long bytes;
    time_t when;
    int w, h;
    bool alpha;
} entry_t;

/* newest first; the same moment A to Z */
static int by_age(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->when != y->when)
        return x->when > y->when ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

void pictures_list(buf_t *out)
{
    char folder[512];
    DIR *dir = opendir(pictures_folder(folder, sizeof(folder)));
    if (dir == NULL)
        return;
    entry_t *entries = calloc(LIST_MAX, sizeof(*entries));
    int n = 0;
    struct dirent *e;
    while (entries != NULL && (e = readdir(dir)) != NULL && n < LIST_MAX) {
        /* hidden files are left out, uploads on their way among them (.name.partial) */
        if (!pictures_name_ok(e->d_name))
            continue;
        char path[800];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", folder, e->d_name);
        entry_t *entry = &entries[n];
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > PICTURE_MAX_BYTES ||
            !pictures_probe(path, &entry->w, &entry->h, &entry->alpha))
            continue;
        entry->name = strdup(e->d_name);
        entry->bytes = (long long)st.st_size;
        entry->when = st.st_mtime;
        n += entry->name != NULL;
    }
    closedir(dir);
    if (entries == NULL)
        return;
    qsort(entries, (size_t)n, sizeof(*entries), by_age);
    for (int i = 0; i < n; i++) {
        buf_printf(out, "F\t");
        buf_field(out, entries[i].name);
        buf_printf(out, "\t%lld\t%d\t%d\t%d\n", entries[i].bytes, entries[i].w, entries[i].h, entries[i].alpha);
        free(entries[i].name);
    }
    free(entries);
}

/* The picture's path, after checking it is one of the folder's own files */
static bool picture_path(const char *name, char *path, size_t size, char *err, size_t errlen)
{
    if (!pictures_name_ok(name)) {
        snprintf(err, errlen, "That is not the name of a picture in Art Swap's pictures folder");
        return false;
    }
    char folder[512];
    snprintf(path, size, "%s/%s", pictures_folder(folder, sizeof(folder)), name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, errlen, "%s is no longer in Art Swap's pictures folder", name);
        return false;
    }
    if ((size_t)st.st_size > PICTURE_MAX_BYTES) {
        snprintf(err, errlen, "%s is too big for Art Swap (40 MB at most)", name);
        return false;
    }
    return true;
}

bool pictures_read(const char *name, buf_t *out, char *err, size_t errlen)
{
    out->len = 0;
    char path[800];
    if (!picture_path(name, path, sizeof(path), err, errlen))
        return false;
    if (!file_read(path, out) || out->len == 0) {
        snprintf(err, errlen, "%s could not be read", name);
        return false;
    }
    return true;
}

bool pictures_delete(const char *name, char *err, size_t errlen)
{
    char path[800];
    if (!picture_path(name, path, sizeof(path), err, errlen))
        return false;
    if (unlink(path) != 0) {
        snprintf(err, errlen, "%s could not be deleted", name);
        return false;
    }
    log_line("pictures: deleted %s", name);
    return true;
}

static bool is_picture(const char *name)
{
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot && i < sizeof(kKinds) / sizeof(kKinds[0]); i++)
        if (strcasecmp(dot, kKinds[i]) == 0)
            return true;
    return false;
}

bool pictures_upload_name(const char *wanted, char *out, size_t size)
{
    /* the last part of what the phone called it, with only plain characters (as music_upload_name) */
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
    return pictures_name_ok(out) && strstr(out, "..") == NULL && is_picture(out);
}
