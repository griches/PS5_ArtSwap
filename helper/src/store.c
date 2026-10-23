/*
 * Art Swap helper - a store game's own logo, from PlayStation Store's public catalog (store.h).
 *
 * A game bought from PlayStation Store shows its logo on its home-screen page;
 * the console fetches it from PlayStation Network, which a jailbroken console
 * does not reach. The store's catalog answers anyone, with no account, by the
 * game's concept (from app.db: scan.c). It is asked only when the person has
 * turned PlayStation Store logos on (config.c), as a web browser asks
 * (http_get_plain): no key, no cookie, nothing that says Art Swap or PS5. Sony
 * still sees the console's address and which game. Each game is asked about
 * once: the answer is kept in /data/artswap/store/<concept>.txt (a game the
 * store had no logo for is asked about again after a week).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "store.h"

#include "../third_party/cJSON.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define CATALOG "https://m.np.playstation.com/api/catalog/v2/concepts/%ld?country=%s&language=%s&age=99"
#define IMAGES "https://image.api.playstation.com/"
#define ASK_AGAIN_S (7 * 24 * 3600)

bool store_picture(const char *url)
{
    return strncmp(url, IMAGES, strlen(IMAGES)) == 0;
}

/* The LOGO picture in a concept's answer, on Sony's image server only */
static bool logo_of(const buf_t *json, char *url, size_t size)
{
    cJSON *root = cJSON_ParseWithLength((const char *)json->data, json->len);
    const cJSON *media = root ? cJSON_GetObjectItemCaseSensitive(root, "media") : NULL;
    const cJSON *images = media ? cJSON_GetObjectItemCaseSensitive(media, "images") : NULL;
    const cJSON *image;
    bool found = false;
    cJSON_ArrayForEach(image, images)
    {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(image, "type");
        const cJSON *address = cJSON_GetObjectItemCaseSensitive(image, "url");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "LOGO") == 0 && cJSON_IsString(address) &&
            store_picture(address->valuestring)) {
            copy_str(url, size, address->valuestring);
            found = true;
            break;
        }
    }
    cJSON_Delete(root);
    return found;
}

/* The concept's logo: as the last answer said, or asked of the US, UK, then Japanese store; "" for none */
static bool find_logo(long concept, char *url, size_t size, char *err, size_t errlen)
{
    char name[64], path[512];
    snprintf(name, sizeof(name), "store/%ld.txt", concept);
    data_path(name, path, sizeof(path));
    buf_t kept = {0};
    struct stat st;
    if (file_read(path, &kept) && kept.len > 0) {
        copy_str(url, size, (const char *)kept.data);
        url[strcspn(url, "\r\n")] = 0;
        buf_free(&kept);
        const bool none = strcmp(url, "none") == 0;
        if (!none)
            return true;
        if (stat(path, &st) == 0 && time(NULL) - st.st_mtime < ASK_AGAIN_S) {
            url[0] = 0;
            return true;
        }
    }
    buf_free(&kept);
    static const char *stores[][2] = {{"US", "en-US"}, {"GB", "en-GB"}, {"JP", "ja-JP"}};
    url[0] = 0;
    for (size_t i = 0; i < sizeof(stores) / sizeof(stores[0]) && !url[0]; i++) {
        char address[256];
        snprintf(address, sizeof(address), CATALOG, concept, stores[i][0], stores[i][1]);
        buf_t body = {0};
        int status = 0;
        if (!http_get_plain(address, &body, &status, err, errlen)) {
            /* refused at once, most often: jailbroken consoles are kept from Sony's servers, to stop updates */
            char why[160];
            copy_str(why, sizeof(why), err);
            snprintf(err, errlen, "This console cannot reach PlayStation Store: its servers are blocked here, as on "
                     "most jailbroken consoles (%s)", why);
            buf_free(&body);
            return false;
        }
        if (status == 200)
            logo_of(&body, url, size);
        buf_free(&body);
        if (status != 200 && status != 404) {
            snprintf(err, errlen, "PlayStation Store answered %d", status);
            return false;
        }
    }
    char folder[512];
    mkdirs(data_path("store", folder, sizeof(folder)));
    const char *line = url[0] ? url : "none";
    file_write_replace(path, line, strlen(line), NULL);
    log_line("store: concept %ld %s", concept, url[0] ? "has a logo" : "has no logo");
    return true;
}

bool store_logo(const app_t *app, buf_t *out, char *err, size_t errlen)
{
    if (!config_store_logos()) {
        snprintf(err, errlen, "PlayStation Store logos are off: turn them on in Settings, Games");
        return false;
    }
    if (app->concept <= 0) {
        snprintf(err, errlen, "%s is not a PlayStation Store game", app->name);
        return false;
    }
    char url[512];
    if (!find_logo(app->concept, url, sizeof(url), err, errlen))
        return false;
    if (!url[0]) {
        buf_printf(out, "P\t0\t0\t0\n");
        return true;
    }
    buf_t png = {0};
    art_format_t fmt;
    const bool ok = sgdb_fetch(url, &png, err, errlen) && image_probe(png.data, png.len, &fmt);
    buf_free(&png);
    if (!ok) {
        if (!err[0])
            snprintf(err, errlen, "PlayStation Store's logo is not a picture");
        return false;
    }
    buf_printf(out, "P\t0\t1\t1\nI\t0\t");
    buf_field(out, url);
    buf_printf(out, "\t");
    buf_field(out, url);
    buf_printf(out, "\t%d\t%d\tofficial\tPlayStation Store\timage/png\n", fmt.w, fmt.h);
    return true;
}
