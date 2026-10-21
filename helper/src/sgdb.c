/*
 * Art Swap helper - SteamGridDB (https://www.steamgriddb.com/api/v2).
 *
 * The API's answers are turned into the TSV lines of protocol.md here, so the
 * title never parses JSON. Pictures are only ever fetched from SteamGridDB's
 * own hosts, and kept in /data/artswap/cache (trimmed at start-up).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "artswap.h"

#include "../third_party/cJSON.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define API "https://www.steamgriddb.com/api/v2"

static void url_escape(const char *text, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && o + 4 < size; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
            *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = 0;
}

/* GET an API path; on success the parsed answer, whose "success" was true */
static cJSON *api_get(const char *path, const char *key, char *err, size_t errlen)
{
    char url[1024];
    snprintf(url, sizeof(url), "%s%s", API, path);
    char own_key[80];
    if (key == NULL) {
        if (!config_key(own_key, sizeof(own_key))) {
            snprintf(err, errlen, "No SteamGridDB key yet: add yours in Art Swap's settings (OPTIONS)");
            return NULL;
        }
        key = own_key;
    }
    buf_t body = {0};
    int status = 0;
    if (!http_get(url, key, &body, &status, err, errlen)) {
        buf_free(&body);
        return NULL;
    }
    cJSON *json = body.len ? cJSON_ParseWithLength((const char *)body.data, body.len) : NULL;
    buf_free(&body);
    const cJSON *success = json ? cJSON_GetObjectItemCaseSensitive(json, "success") : NULL;
    if (status == 200 && cJSON_IsTrue(success))
        return json;
    /* why not */
    const cJSON *errors = json ? cJSON_GetObjectItemCaseSensitive(json, "errors") : NULL;
    const cJSON *first = cJSON_IsArray(errors) ? cJSON_GetArrayItem(errors, 0) : NULL;
    if (status == 401)
        snprintf(err, errlen, "SteamGridDB refused the API key (401)");
    else if (status == 404)
        snprintf(err, errlen, "SteamGridDB has nothing here (404)");
    else if (status == 429)
        snprintf(err, errlen, "SteamGridDB is busy: too many requests, try again in a minute");
    else if (cJSON_IsString(first))
        snprintf(err, errlen, "SteamGridDB: %s (%d)", first->valuestring, status);
    else
        snprintf(err, errlen, "SteamGridDB answered %d", status);
    cJSON_Delete(json);
    return NULL;
}

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static double json_number(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valuedouble : 0.0;
}

bool sgdb_search(const char *term, buf_t *out, char *err, size_t errlen)
{
    char escaped[512], path[600];
    url_escape(term, escaped, sizeof(escaped));
    snprintf(path, sizeof(path), "/search/autocomplete/%s", escaped);
    cJSON *json = api_get(path, NULL, err, errlen);
    if (json == NULL)
        return false;
    const cJSON *game;
    cJSON_ArrayForEach(game, cJSON_GetObjectItemCaseSensitive(json, "data"))
    {
        const double released = json_number(game, "release_date");
        int year = 0;
        if (released > 0) {
            const time_t t = (time_t)released;
            struct tm tm;
            gmtime_r(&t, &tm);
            year = tm.tm_year + 1900;
        }
        buf_printf(out, "G\t%.0f\t", json_number(game, "id"));
        buf_field(out, json_string(game, "name"));
        buf_printf(out, "\t%d\t%d\n", year, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(game, "verified")));
    }
    cJSON_Delete(json);
    return true;
}

bool sgdb_list(long game, const char *kind, int page, buf_t *out, char *err, size_t errlen)
{
    static const struct {
        const char *kind, *endpoint, *query;
        int min_side;
    } kinds[] = {
        {"square", "grids", "dimensions=512x512,1024x1024&mimes=image/png,image/jpeg", 0},
        {"covers", "grids", "dimensions=600x900,660x930,342x482&mimes=image/png,image/jpeg", 0},
        {"wide", "grids", "dimensions=920x430,460x215&mimes=image/png,image/jpeg", 0},
        /* the sharpest first: 3840x1240, then 1920x620 and 1600x650 (a background is 3840x2160) */
        {"heroes", "heroes", "dimensions=3840x1240&mimes=image/png,image/jpeg", 0},
        {"heroes", "heroes", "dimensions=1920x620,1600x650&mimes=image/png,image/jpeg", 0},
        {"icons", "icons", "mimes=image/png", 192},
        {"logos", "logos", "mimes=image/png", 0},
    };
    /* a kind may be several queries, listed one after the other on each page */
    cJSON *answers[2] = {NULL, NULL};
    int queries = 0, from = -1;
    double total = 0, limit = 0;
    for (int i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])) && queries < 2; i++) {
        if (strcmp(kind, kinds[i].kind) != 0)
            continue;
        if (from < 0)
            from = i;
        char path[400];
        snprintf(path, sizeof(path), "/%s/game/%ld?%s&types=static&nsfw=false&page=%d", kinds[i].endpoint, game,
                 kinds[i].query, page);
        cJSON *json = api_get(path, NULL, err, errlen);
        if (json == NULL) {
            cJSON_Delete(answers[0]);
            return false;
        }
        total += json_number(json, "total");
        limit += json_number(json, "limit");
        answers[queries++] = json;
    }
    if (from < 0) {
        snprintf(err, errlen, "no kind of art called %s", kind);
        return false;
    }
    const int k = from;
    buf_printf(out, "P\t%d\t%.0f\t%.0f\n", page, total, limit);
    int hidden = 0;
    for (int q = 0; q < queries; q++) {
        const cJSON *image;
        cJSON_ArrayForEach(image, cJSON_GetObjectItemCaseSensitive(answers[q], "data"))
        {
            const int w = (int)json_number(image, "width"), h = (int)json_number(image, "height");
            if (w < kinds[k].min_side || h < kinds[k].min_side) {
                hidden++;
                continue;
            }
            const cJSON *style = cJSON_GetObjectItemCaseSensitive(image, "style");
            const cJSON *author = cJSON_GetObjectItemCaseSensitive(image, "author");
            buf_printf(out, "I\t%.0f\t", json_number(image, "id"));
            buf_field(out, json_string(image, "url"));
            buf_printf(out, "\t");
            buf_field(out, json_string(image, "thumb"));
            buf_printf(out, "\t%d\t%d\t", w, h);
            buf_field(out, cJSON_IsString(style) ? style->valuestring : "");
            buf_printf(out, "\t");
            buf_field(out, author ? json_string(author, "name") : "");
            buf_printf(out, "\t");
            buf_field(out, json_string(image, "mime"));
            buf_printf(out, "\n");
        }
        cJSON_Delete(answers[q]);
    }
    if (hidden)
        buf_printf(out, "H\t%d\n", hidden);
    return true;
}

bool sgdb_check_key(const char *key, char *err, size_t errlen)
{
    cJSON *json = api_get("/search/autocomplete/portal", key, err, errlen);
    cJSON_Delete(json);
    return json != NULL;
}

/* ---- pictures, through the cache ---- */

static bool allowed(const char *url)
{
    static const char *hosts[] = {"https://cdn2.steamgriddb.com/", "https://cdn.steamgriddb.com/",
                                  "https://www.steamgriddb.com/", "https://steamgriddb.com/"};
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
        if (strncmp(url, hosts[i], strlen(hosts[i])) == 0)
            return true;
    return false;
}

bool sgdb_fetch(const char *url, buf_t *out, char *err, size_t errlen)
{
    out->len = 0;
    if (!allowed(url)) {
        snprintf(err, errlen, "only SteamGridDB's pictures are fetched");
        return false;
    }
    char name[64], path[512];
    snprintf(name, sizeof(name), "cache/%016llx.bin", (unsigned long long)fnv1a64(url));
    data_path(name, path, sizeof(path));
    if (file_read(path, out) && out->len > 0)
        return true;
    int status = 0;
    if (!http_get(url, NULL, out, &status, err, errlen))
        return false;
    if (status != 200 || out->len == 0) {
        snprintf(err, errlen, "the picture did not download (%d)", status);
        return false;
    }
    if (!file_write_replace(path, out->data, out->len, NULL))
        log_line("could not keep %s in the cache", url);
    return true;
}

/* Oldest first, down to 192 MiB, when the cache is over 256 MiB */
void sgdb_cache_trim(void)
{
    char folder[512];
    data_path("cache", folder, sizeof(folder));
    DIR *dir = opendir(folder);
    if (dir == NULL)
        return;
    typedef struct {
        char name[64];
        time_t when;
        off_t size;
    } entry_t;
    entry_t *entries = NULL;
    size_t count = 0, cap = 0;
    off_t total = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(entries->name))
            continue;
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", folder, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 256;
            entry_t *grown = realloc(entries, cap * sizeof(*entries));
            if (grown == NULL)
                break;
            entries = grown;
        }
        copy_str(entries[count].name, sizeof(entries[count].name), e->d_name);
        entries[count].when = st.st_mtime;
        entries[count].size = st.st_size;
        total += st.st_size;
        count++;
    }
    closedir(dir);
    const off_t high = (off_t)256 << 20, low = (off_t)192 << 20;
    if (total > high) {
        /* the oldest out first (a simple selection: the cache holds hundreds, not millions) */
        while (total > low) {
            size_t oldest = count;
            for (size_t i = 0; i < count; i++)
                if (entries[i].size >= 0 && (oldest == count || entries[i].when < entries[oldest].when))
                    oldest = i;
            if (oldest == count)
                break;
            char path[640];
            snprintf(path, sizeof(path), "%s/%s", folder, entries[oldest].name);
            unlink(path);
            total -= entries[oldest].size;
            entries[oldest].size = -1;
        }
        log_line("cache trimmed to %lld MiB", (long long)(total >> 20));
    }
    free(entries);
}
