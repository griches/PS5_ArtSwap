/*
 * Art Swap helper (artswapd) - the shared declarations.
 *
 * The helper is a payload: it runs outside any title's sandbox, so it can read
 * and write the home screen's art (/user/appmeta) and the homebrew folders,
 * and it talks to SteamGridDB. The Art Swap title starts it through the ELF
 * loader and asks it for everything over a localhost socket (protocol.md).
 *
 * The same source builds for macOS and Linux (ARTSWAP_HOST) with libcurl, the
 * console's paths under $ARTSWAP_ROOT, for tests run without a console.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTSWAP_VERSION "0.1.0"
#define ARTSWAP_PORT 9117

/* ---- growable byte buffer ---- */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} buf_t;

void buf_free(buf_t *b);
bool buf_reserve(buf_t *b, size_t extra);
bool buf_append(buf_t *b, const void *data, size_t len);
bool buf_printf(buf_t *b, const char *format, ...) __attribute__((format(printf, 2, 3)));
/* Appends value with tabs and line breaks turned into spaces (a TSV field) */
bool buf_field(buf_t *b, const char *value);

/* ---- log: stdout (the kernel log, for a payload) and /data/artswap/helper.log ---- */
void log_open(void);
void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* ---- paths ---- */
/* A console path as this process opens it: $ARTSWAP_ROOT + path on a host build */
const char *cpath(const char *path, char *out, size_t size);
/* The helper's own folder (/data/artswap) and a file in it */
const char *data_path(const char *name, char *out, size_t size);

/* ---- files (paths already through cpath) ---- */
bool file_exists(const char *path);
bool dir_exists(const char *path);
bool file_read(const char *path, buf_t *out);
/* Writes path.new, flushes it, renames it over path; keeps mode and owner of
 * like (when not NULL) */
bool file_write_replace(const char *path, const void *data, size_t len, const char *like);
bool file_copy(const char *from, const char *to);
bool mkdirs(const char *path);

/* ---- small helpers ---- */
uint64_t fnv1a64(const char *text);
uint64_t now_ms(void);
void trim(char *text);
/* Copies src into dst (size bytes), always terminated */
void copy_str(char *dst, size_t size, const char *src);

/* ---- HTTP (http_ps5.c on the console, http_host.c with libcurl) ---- */
bool http_init(char *err, size_t errlen);
const char *http_backend(void);
/* GET url, with "Authorization: Bearer <bearer>" when bearer is not NULL.
 * Returns false on a transport error (err says why); status holds the HTTP
 * status otherwise, and out the body. */
bool http_get(const char *url, const char *bearer, buf_t *out, int *status, char *err, size_t errlen);

/* ---- configuration: the API key ---- */
void config_load(void);
/* The person's own key (/data/artswap/apikey.txt); false when they have not given one */
bool config_key(char *out, size_t size);
const char *config_key_source(void); /* "user" or "none" */
bool config_set_key(const char *key);

/* ---- SteamGridDB (sgdb.c) ---- */
/* Each writes TSV lines into out (protocol.md) */
bool sgdb_search(const char *term, buf_t *out, char *err, size_t errlen);
bool sgdb_list(long game, const char *kind, int page, buf_t *out, char *err, size_t errlen);
bool sgdb_check_key(const char *key, char *err, size_t errlen);
/* A picture from SteamGridDB's servers, through the download cache */
bool sgdb_fetch(const char *url, buf_t *out, char *err, size_t errlen);
void sgdb_cache_trim(void);
