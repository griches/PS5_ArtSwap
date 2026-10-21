/*
 * Art Swap helper - buffers, the log, paths and files.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "artswap.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- buffers ---- */

void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

bool buf_reserve(buf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra + 1)
        cap *= 2;
    uint8_t *data = realloc(b->data, cap);
    if (data == NULL)
        return false;
    b->data = data;
    b->cap = cap;
    return true;
}

bool buf_append(buf_t *b, const void *data, size_t len)
{
    if (!buf_reserve(b, len))
        return false;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0; /* text stays terminated */
    return true;
}

bool buf_printf(buf_t *b, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list again;
    va_copy(again, args);
    const int need = vsnprintf(NULL, 0, format, args);
    va_end(args);
    bool ok = need >= 0 && buf_reserve(b, (size_t)need);
    if (ok) {
        vsnprintf((char *)b->data + b->len, (size_t)need + 1, format, again);
        b->len += (size_t)need;
    }
    va_end(again);
    return ok;
}

bool buf_field(buf_t *b, const char *value)
{
    const size_t len = value ? strlen(value) : 0;
    if (!buf_reserve(b, len))
        return false;
    for (size_t i = 0; i < len; i++) {
        const char c = value[i];
        b->data[b->len++] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    b->data[b->len] = 0;
    return true;
}

/* ---- the log ---- */

static FILE *g_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_open(void)
{
    char path[512];
    data_path("helper.log", path, sizeof(path));
    g_log = fopen(path, "w");
}

void log_line(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    pthread_mutex_lock(&g_log_lock);
    printf("[artswapd] %s\n", line);
    fflush(stdout);
    if (g_log != NULL) {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        fprintf(g_log, "%02d:%02d:%02d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_log_lock);
}

/* ---- paths ---- */

const char *cpath(const char *path, char *out, size_t size)
{
#ifdef ARTSWAP_HOST
    const char *root = getenv("ARTSWAP_ROOT");
    if (root != NULL && path[0] == '/') {
        snprintf(out, size, "%s%s", root, path);
        return out;
    }
#endif
    copy_str(out, size, path);
    return out;
}

const char *data_path(const char *name, char *out, size_t size)
{
    char folder[512];
    snprintf(folder, sizeof(folder), "/data/artswap/%s", name);
    return cpath(folder, out, size);
}

/* ---- files ---- */

bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool file_read(const char *path, buf_t *out)
{
    out->len = 0;
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0)
        buf_reserve(out, (size_t)st.st_size);
    for (;;) {
        if (!buf_reserve(out, 65536)) {
            close(fd);
            return false;
        }
        const ssize_t got = read(fd, out->data + out->len, 65536);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return false;
        }
        if (got == 0)
            break;
        out->len += (size_t)got;
    }
    close(fd);
    if (out->data != NULL)
        out->data[out->len] = 0;
    return true;
}

static bool write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        const ssize_t put = write(fd, p, len);
        if (put < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += put;
        len -= (size_t)put;
    }
    return true;
}

bool file_write_replace(const char *path, const void *data, size_t len, const char *like)
{
    char temp[600];
    snprintf(temp, sizeof(temp), "%s.new", path);
    mode_t mode = 0644;
    struct stat st;
    const bool have_like = like != NULL && stat(like, &st) == 0;
    if (have_like)
        mode = st.st_mode & 07777;
    const int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        log_line("write %s: %s", temp, strerror(errno));
        return false;
    }
    bool ok = write_all(fd, data, len) && fsync(fd) == 0;
    close(fd);
    if (ok && have_like) {
        if (chmod(temp, mode) != 0 || chown(temp, st.st_uid, st.st_gid) != 0)
            log_line("%s: mode or owner not kept: %s", path, strerror(errno));
    }
    if (ok && rename(temp, path) != 0) {
        log_line("rename %s: %s", path, strerror(errno));
        ok = false;
    }
    if (!ok)
        unlink(temp);
    return ok;
}

bool file_copy(const char *from, const char *to)
{
    buf_t data = {0};
    const bool ok = file_read(from, &data) && file_write_replace(to, data.data, data.len, from);
    buf_free(&data);
    return ok;
}

bool mkdirs(const char *path)
{
    char temp[512];
    copy_str(temp, sizeof(temp), path);
    for (char *p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(temp, 0777);
            *p = '/';
        }
    }
    return mkdir(temp, 0777) == 0 || errno == EEXIST;
}

/* ---- small helpers ---- */

uint64_t fnv1a64(const char *text)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (; *text; text++) {
        hash ^= (uint8_t)*text;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void trim(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == '\t'))
        text[--len] = 0;
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t')
        start++;
    if (start > 0)
        memmove(text, text + start, len - start + 1);
}

void copy_str(char *dst, size_t size, const char *src)
{
    if (size == 0)
        return;
    size_t i = 0;
    for (; src != NULL && src[i] && i + 1 < size; i++)
        dst[i] = src[i];
    dst[i] = 0;
}
