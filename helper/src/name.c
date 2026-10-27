/*
 * Art Swap helper - a title's name, in its param.json.
 *
 * The home screen takes a title's name from its param.json when the title is
 * installed (localizedParameters, each language's titleName, into app.db), and
 * shows the names app.db holds. So Apply changes writes a new name into app.db
 * too (appdb.c), with no install, which would delete the title's download data.
 * ShadowMountPlus reads the folder's sce_sys/param.json at any later install;
 * the home screen's copy in /user/appmeta/<id> is what the scan reads names
 * from. Both are changed together, and each original is kept once as
 * param.json.old, as with the art.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "name.h"

#include "../third_party/cJSON.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define NAME_MAX_PATHS 4

static void add_path(char paths[NAME_MAX_PATHS][420], int *n, const char *path)
{
    bool seen = false;
    for (int j = 0; j < *n && !seen; j++)
        seen = strcmp(paths[j], path) == 0;
    char real[512];
    if (!seen && *n < NAME_MAX_PATHS && file_exists(cpath(path, real, sizeof(real))))
        copy_str(paths[(*n)++], sizeof(paths[0]), path);
}

/* The param.json files beside the title's art: one in each folder that holds its art, and the install's copy
 * the home screen's entry names (/user/app/<id>/sce_sys), whether or not a scan has listed art there */
static int name_paths(const app_t *app, char paths[NAME_MAX_PATHS][420])
{
    int n = 0;
    for (int i = 0; i < app->nfiles && n < NAME_MAX_PATHS; i++) {
        const char *slash = strrchr(app->files[i].path, '/');
        if (slash == NULL)
            continue;
        char path[420];
        snprintf(path, sizeof(path), "%.*s/param.json", (int)(slash - app->files[i].path), app->files[i].path);
        add_path(paths, &n, path);
    }
    char installed[420];
    snprintf(installed, sizeof(installed), "/user/app/%s/sce_sys/param.json", app->id);
    add_path(paths, &n, installed);
    return n;
}

/* 1 to 127 bytes of UTF-8, no control characters */
static bool name_valid(const char *name, char *err, size_t errlen)
{
    const size_t len = strlen(name);
    if (len == 0 || len > 127) {
        snprintf(err, errlen, "A name is 1 to 127 bytes long");
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p;) {
        int more = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;
        if (more < 0 || *p < 0x20 || *p == 0x7f) {
            snprintf(err, errlen, "That name has a character a title name cannot hold");
            return false;
        }
        p++;
        for (; more > 0; more--, p++)
            if ((*p & 0xc0) != 0x80) {
                snprintf(err, errlen, "That name has a character a title name cannot hold");
                return false;
            }
    }
    return true;
}

/* param.json's text with every language's titleName set to name; NULL when it has none */
static char *renamed(const buf_t *json, const char *name)
{
    cJSON *root = cJSON_ParseWithLength((const char *)json->data, json->len);
    cJSON *languages = root ? cJSON_GetObjectItemCaseSensitive(root, "localizedParameters") : NULL;
    int set = 0;
    const cJSON *language;
    cJSON_ArrayForEach(language, languages)
    {
        if (!cJSON_IsObject(language) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(language, "titleName")))
            continue;
        cJSON_ReplaceItemInObjectCaseSensitive((cJSON *)language, "titleName", cJSON_CreateString(name));
        set++;
    }
    char *text = set > 0 ? cJSON_Print(root) : NULL;
    cJSON_Delete(root);
    return text;
}

bool name_change(const app_t *app, const char *name, buf_t *report, char *err, size_t errlen)
{
    if (!name_valid(name, err, errlen))
        return false;
    char paths[NAME_MAX_PATHS][420];
    const int n = name_paths(app, paths);
    if (n == 0) {
        snprintf(err, errlen, "%s has no param.json, where a PS5 title keeps its name", app->name);
        return false;
    }
    /* every copy is made before any is written: all of them change, or none */
    buf_t originals[NAME_MAX_PATHS] = {{0}};
    char *texts[NAME_MAX_PATHS] = {0};
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        char real[512];
        ok = file_read(cpath(paths[i], real, sizeof(real)), &originals[i]) &&
             (texts[i] = renamed(&originals[i], name)) != NULL;
        if (!ok)
            snprintf(err, errlen, "%s has no name Art Swap can change", paths[i]);
    }
    for (int i = 0; i < n && ok; i++) {
        char real[512], old[430], real_old[512];
        cpath(paths[i], real, sizeof(real));
        snprintf(old, sizeof(old), "%s.old", paths[i]);
        cpath(old, real_old, sizeof(real_old));
        if (!file_exists(real_old)) {
            ok = file_write_replace(real_old, originals[i].data, originals[i].len, real);
            if (ok)
                buf_printf(report, "O\t%s\n", old);
            else
                snprintf(err, errlen, "the original could not be kept as %s", old);
        }
        if (ok) {
            ok = file_write_replace(real, texts[i], strlen(texts[i]), real);
            if (ok)
                buf_printf(report, "W\t%s\n", paths[i]);
            else
                snprintf(err, errlen, "%s could not be written", paths[i]);
        }
    }
    for (int i = 0; i < n; i++) {
        buf_free(&originals[i]);
        cJSON_free(texts[i]);
    }
    log_line("rename %s: \"%s\" in %d files%s%s", app->id, name, n, ok ? "" : ": ", ok ? "" : err);
    return ok;
}

int name_restore(const app_t *app, buf_t *report)
{
    char paths[NAME_MAX_PATHS][420];
    const int n = name_paths(app, paths);
    int restored = 0;
    bool failed = false;
    for (int i = 0; i < n; i++) {
        char real[512], old[430], real_old[512];
        cpath(paths[i], real, sizeof(real));
        snprintf(old, sizeof(old), "%s.old", paths[i]);
        if (!file_exists(cpath(old, real_old, sizeof(real_old))))
            continue;
        if (file_rename(real_old, real)) {
            buf_printf(report, "R\t%s\n", paths[i]);
            restored++;
        } else {
            failed = true;
        }
    }
    if (restored > 0 || failed)
        log_line("restore %s name: %d files%s", app->id, restored, failed ? ", one failed" : "");
    return failed ? -1 : restored;
}

/* param.json's language tags, in the home screen's numbering */
static const char *const kLanguages[NAME_LANGUAGES] = {
    "ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT", "ru-RU", "ko-KR", "zh-Hant",
    "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO", "pl-PL", "pt-BR", "en-GB", "tr-TR", "es-419", "ar-AE",
    "fr-CA", "cs-CZ", "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID", "uk-UA"};

bool name_read(const app_t *app, names_t *out)
{
    memset(out, 0, sizeof(*out));
    char paths[NAME_MAX_PATHS][420];
    const int n = name_paths(app, paths);
    buf_t json = {0};
    char real[512];
    if (n == 0 || !file_read(cpath(paths[0], real, sizeof(real)), &json)) {
        buf_free(&json);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)json.data, json.len);
    buf_free(&json);
    const cJSON *languages = root ? cJSON_GetObjectItemCaseSensitive(root, "localizedParameters") : NULL;
    const cJSON *def = languages ? cJSON_GetObjectItemCaseSensitive(languages, "defaultLanguage") : NULL;
    const cJSON *language;
    cJSON_ArrayForEach(language, languages)
    {
        const cJSON *title = cJSON_IsObject(language) ? cJSON_GetObjectItemCaseSensitive(language, "titleName") : NULL;
        if (!cJSON_IsString(title) || title->valuestring[0] == 0 || strlen(title->valuestring) >= sizeof(out->def))
            continue;
        if (out->def[0] == 0 || (cJSON_IsString(def) && strcmp(language->string, def->valuestring) == 0))
            copy_str(out->def, sizeof(out->def), title->valuestring);
        for (int i = 0; i < NAME_LANGUAGES; i++)
            if (strcasecmp(language->string, kLanguages[i]) == 0)
                copy_str(out->lang[i], sizeof(out->lang[i]), title->valuestring);
    }
    cJSON_Delete(root);
    return out->def[0] != 0;
}

bool name_kept(const app_t *app)
{
    char paths[NAME_MAX_PATHS][420];
    const int n = name_paths(app, paths);
    for (int i = 0; i < n; i++) {
        char old[430], real_old[512];
        snprintf(old, sizeof(old), "%s.old", paths[i]);
        if (file_exists(cpath(old, real_old, sizeof(real_old))))
            return true;
    }
    return false;
}
