/*
 * Art Swap helper - HTTP on a Mac or Linux host (tests), through libcurl.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "artswap.h"

#include <curl/curl.h>
#include <stdio.h>

bool http_init(char *err, size_t errlen)
{
    const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        snprintf(err, errlen, "curl: %s", curl_easy_strerror(rc));
        return false;
    }
    return true;
}

const char *http_backend(void)
{
    return "curl";
}

static size_t collect(char *data, size_t size, size_t count, void *context)
{
    return buf_append(context, data, size * count) ? size * count : 0;
}

static bool get_as(const char *url, const char *bearer, const char *agent, buf_t *out, int *status, char *err,
                   size_t errlen)
{
    out->len = 0;
    *status = 0;
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        snprintf(err, errlen, "curl: no handle");
        return false;
    }
    struct curl_slist *headers = NULL;
    char auth[160];
    if (bearer != NULL) {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
        headers = curl_slist_append(headers, auth);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        snprintf(err, errlen, "%s", curl_easy_strerror(rc));
        return false;
    }
    *status = (int)code;
    return true;
}

bool http_get(const char *url, const char *bearer, buf_t *out, int *status, char *err, size_t errlen)
{
    return get_as(url, bearer, "ArtSwap/" ARTSWAP_VERSION " (PS5 homebrew)", out, status, err, errlen);
}

bool http_get_plain(const char *url, buf_t *out, int *status, char *err, size_t errlen)
{
    return get_as(url, NULL, "Mozilla/5.0", out, status, err, errlen);
}
