/*
 * Art Swap helper - the titles on the console and where their art is (scan.h).
 *
 * The home screen draws a title's tile and background from its folder in
 * /user/appmeta (icon0, pic0, pic1), or, for one ShadowMountPlus installed,
 * from /user/app/<id>/sce_sys, the copy it made at install. A homebrew title
 * has one more in its own folder (sce_sys/), which ShadowMountPlus copies in
 * when it installs the title again, so all of them are changed together. Names come from the param.json or
 * param.sfo beside the art, then the homebrew folder's param.json, then the
 * system's app.db (a copy, read only), then the title id. Every scan is
 * written to /data/artswap/scan.txt, to read over FTP.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "scan.h"

#include "../third_party/cJSON.h"
#include "../third_party/sqlite3.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *kArtNames[] = {"icon0.png", "icon0.dds", "icon0.jpg", "pic0.png", "pic0.dds",
                                  "pic0.jpg",  "pic1.png",  "pic1.dds",  "pic1.jpg"};

static const char *kHomebrewRoots[] = {
    "/data/homebrew",      "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
    "/mnt/usb3/homebrew",  "/mnt/usb4/homebrew", "/mnt/usb5/homebrew", "/mnt/usb6/homebrew",
    "/mnt/usb7/homebrew",  "/mnt/ext0/homebrew", "/mnt/ext1/homebrew",
};

static bool is_title_id(const char *name)
{
    if (strlen(name) != 9)
        return false;
    for (int i = 0; i < 4; i++)
        if (!isupper((unsigned char)name[i]))
            return false;
    for (int i = 4; i < 9; i++)
        if (!isdigit((unsigned char)name[i]))
            return false;
    return strncmp(name, "NPXS", 4) != 0; /* the system's own apps */
}

bool slot_matches(const art_file_t *file, const char *slot)
{
    if (strcmp(slot, "icon") == 0)
        return strcmp(file->slot, "icon0") == 0;
    if (strcmp(slot, "pic") == 0)
        return strcmp(file->slot, "pic0") == 0 || strcmp(file->slot, "pic1") == 0;
    return strcmp(file->slot, slot) == 0;
}

/* ---- names ---- */

static void clean_name(char *name)
{
    /* the trademark signs and the line breaks some param files carry */
    static const char *drop[] = {"\xe2\x84\xa2", "\xc2\xae", "\xc2\xa9"};
    for (size_t d = 0; d < sizeof(drop) / sizeof(drop[0]); d++) {
        char *at;
        while ((at = strstr(name, drop[d])) != NULL)
            memmove(at, at + strlen(drop[d]), strlen(at + strlen(drop[d])) + 1);
    }
    for (char *p = name; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    trim(name);
}

static bool name_from_param_json(const char *path, char *name, size_t size)
{
    char real[512];
    buf_t text = {0};
    bool found = false;
    if (file_read(cpath(path, real, sizeof(real)), &text) && text.len > 0) {
        cJSON *json = cJSON_ParseWithLength((const char *)text.data, text.len);
        const cJSON *localized = cJSON_GetObjectItemCaseSensitive(json, "localizedParameters");
        if (cJSON_IsObject(localized)) {
            const cJSON *lang = cJSON_GetObjectItemCaseSensitive(localized, "defaultLanguage");
            const cJSON *entry = cJSON_IsString(lang) ? cJSON_GetObjectItemCaseSensitive(localized, lang->valuestring)
                                                      : NULL;
            const cJSON *title = entry ? cJSON_GetObjectItemCaseSensitive(entry, "titleName") : NULL;
            if (!cJSON_IsString(title)) {
                /* any language with a name */
                const cJSON *each;
                cJSON_ArrayForEach(each, localized)
                {
                    title = cJSON_GetObjectItemCaseSensitive(each, "titleName");
                    if (cJSON_IsString(title))
                        break;
                }
            }
            if (cJSON_IsString(title) && title->valuestring[0]) {
                copy_str(name, size, title->valuestring);
                found = true;
            }
        }
        cJSON_Delete(json);
    }
    buf_free(&text);
    return found;
}

static bool title_id_from_param_json(const char *path, char *id, size_t size)
{
    char real[512];
    buf_t text = {0};
    bool found = false;
    if (file_read(cpath(path, real, sizeof(real)), &text) && text.len > 0) {
        cJSON *json = cJSON_ParseWithLength((const char *)text.data, text.len);
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(json, "titleId");
        if (cJSON_IsString(title) && is_title_id(title->valuestring)) {
            copy_str(id, size, title->valuestring);
            found = true;
        }
        cJSON_Delete(json);
    }
    buf_free(&text);
    return found;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A PS4 title's param.sfo: its TITLE */
static bool name_from_param_sfo(const char *path, char *name, size_t size)
{
    char real[512];
    buf_t sfo = {0};
    bool found = false;
    if (file_read(cpath(path, real, sizeof(real)), &sfo) && sfo.len >= 20 &&
        memcmp(sfo.data, "\0PSF", 4) == 0) {
        const uint32_t keys = le32(sfo.data + 8), values = le32(sfo.data + 12), count = le32(sfo.data + 16);
        for (uint32_t i = 0; i < count && 20 + (i + 1) * 16 <= sfo.len; i++) {
            const uint8_t *e = sfo.data + 20 + i * 16;
            const uint32_t key_at = keys + le16(e), len = le32(e + 4), value_at = values + le32(e + 12);
            if (key_at >= sfo.len || value_at + len > sfo.len)
                continue;
            if (strncmp((const char *)sfo.data + key_at, "TITLE", 6) == 0 && len > 1) {
                const size_t n = len < size ? len : size - 1;
                memcpy(name, sfo.data + value_at, n);
                name[n] = 0;
                found = name[0] != 0;
                break;
            }
        }
    }
    buf_free(&sfo);
    return found;
}

/* Names from a copy of the system's app.db: any table with a title id and a title name column */
static void names_from_app_db(app_t *apps, int count)
{
    char from[512], to[512], off[512];
    if (file_exists(data_path("no-appdb", off, sizeof(off)))) {
        log_line("app.db: skipped (/data/artswap/no-appdb)");
        return;
    }
    log_line("app.db: reading a copy for the names param files did not give");
    cpath("/system_data/priv/mms/app.db", from, sizeof(from));
    data_path("app.db.copy", to, sizeof(to));
    if (!file_copy(from, to)) {
        log_line("app.db: no copy (%s)", from);
        return;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(to, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        log_line("app.db: %s", db ? sqlite3_errmsg(db) : "not opened");
        sqlite3_close(db);
        unlink(to);
        return;
    }
    sqlite3_stmt *tables = NULL;
    int named = 0;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1, &tables, NULL) ==
        SQLITE_OK) {
        while (sqlite3_step(tables) == SQLITE_ROW) {
            char table[128];
            copy_str(table, sizeof(table), (const char *)sqlite3_column_text(tables, 0));
            char sql[300];
            snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", table);
            sqlite3_stmt *info = NULL;
            char id_col[64] = "", name_col[64] = "";
            if (sqlite3_prepare_v2(db, sql, -1, &info, NULL) == SQLITE_OK) {
                while (sqlite3_step(info) == SQLITE_ROW) {
                    const char *col = (const char *)sqlite3_column_text(info, 1);
                    if (col == NULL)
                        continue;
                    if (strcasecmp(col, "titleId") == 0)
                        copy_str(id_col, sizeof(id_col), col);
                    else if (strcasecmp(col, "titleName") == 0)
                        copy_str(name_col, sizeof(name_col), col);
                }
            }
            sqlite3_finalize(info);
            if (!id_col[0] || !name_col[0])
                continue;
            snprintf(sql, sizeof(sql), "SELECT \"%s\", \"%s\" FROM \"%s\"", id_col, name_col, table);
            sqlite3_stmt *rows = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &rows, NULL) == SQLITE_OK) {
                while (sqlite3_step(rows) == SQLITE_ROW) {
                    const char *id = (const char *)sqlite3_column_text(rows, 0);
                    const char *name = (const char *)sqlite3_column_text(rows, 1);
                    if (id == NULL || name == NULL || !name[0])
                        continue;
                    for (int i = 0; i < count; i++)
                        if (!apps[i].name[0] && strcmp(apps[i].id, id) == 0) {
                            copy_str(apps[i].name, sizeof(apps[i].name), name);
                            named++;
                        }
                }
            }
            sqlite3_finalize(rows);
        }
    }
    sqlite3_finalize(tables);
    sqlite3_close(db);
    unlink(to);
    log_line("app.db: %d names", named);
}

/* ---- files ---- */

static app_t *find_or_add(app_t **apps, int *count, int *cap, const char *id)
{
    for (int i = 0; i < *count; i++)
        if (strcmp((*apps)[i].id, id) == 0)
            return &(*apps)[i];
    if (*count == *cap) {
        const int grown_cap = *cap ? *cap * 2 : 64;
        app_t *grown = realloc(*apps, (size_t)grown_cap * sizeof(app_t));
        if (grown == NULL)
            return NULL;
        *apps = grown;
        *cap = grown_cap;
    }
    app_t *app = &(*apps)[(*count)++];
    memset(app, 0, sizeof(*app));
    copy_str(app->id, sizeof(app->id), id);
    copy_str(app->kind, sizeof(app->kind), strncmp(id, "CUSA", 4) == 0 ? "ps4" : "ps5");
    return app;
}

/* Reads enough of a file to know its format */
static bool probe_file(const char *path, art_format_t *fmt)
{
    char real[512];
    const int fd = open(cpath(path, real, sizeof(real)), O_RDONLY);
    if (fd < 0)
        return false;
    uint8_t *head = malloc(65536);
    const ssize_t got = head ? read(fd, head, 65536) : -1;
    close(fd);
    const bool ok = got > 0 && image_probe(head, (size_t)got, fmt);
    free(head);
    return ok;
}

/* The art files in folder, added to app */
static void add_art(app_t *app, const char *folder)
{
    char real[512];
    DIR *dir = opendir(cpath(folder, real, sizeof(real)));
    if (dir == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        for (size_t i = 0; i < sizeof(kArtNames) / sizeof(kArtNames[0]); i++) {
            if (strcasecmp(e->d_name, kArtNames[i]) != 0 || app->nfiles >= APP_MAX_FILES)
                continue;
            art_file_t *file = &app->files[app->nfiles];
            memset(file, 0, sizeof(*file));
            snprintf(file->path, sizeof(file->path), "%s/%s", folder, e->d_name);
            copy_str(file->slot, sizeof(file->slot), kArtNames[i]);
            *strchr(file->slot, '.') = 0;
            if (!probe_file(file->path, &file->fmt)) {
                log_line("%s: not a picture this app writes", file->path);
                continue;
            }
            char old[420], real_old[512];
            snprintf(old, sizeof(old), "%s.old", file->path);
            file->has_old = file_exists(cpath(old, real_old, sizeof(real_old)));
            app->nfiles++;
        }
    }
    closedir(dir);
}

/* What a title's appmeta folder holds, every file, to /data/artswap/appmeta-files.txt */
static buf_t g_listing;

static void list_folder(const char *folder)
{
    char real[512];
    DIR *dir = opendir(cpath(folder, real, sizeof(real)));
    if (dir == NULL)
        return;
    buf_printf(&g_listing, "%s\n", folder);
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[800];
        snprintf(path, sizeof(path), "%s/%s", real, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0)
            buf_printf(&g_listing, "\t%s\t%lld%s\n", e->d_name, (long long)st.st_size, S_ISDIR(st.st_mode) ? "\t(folder)" : "");
    }
    closedir(dir);
}

/* A folder of /user/appmeta that is a title's */
static void add_appmeta_title(app_t **apps, int *count, int *cap, const char *folder, const char *id)
{
    app_t *app = find_or_add(apps, count, cap, id);
    if (app == NULL)
        return;
    list_folder(folder);
    add_art(app, folder);
    char param[480];
    if (!app->name[0]) {
        snprintf(param, sizeof(param), "%s/param.json", folder);
        if (!name_from_param_json(param, app->name, sizeof(app->name))) {
            snprintf(param, sizeof(param), "%s/param.sfo", folder);
            name_from_param_sfo(param, app->name, sizeof(app->name));
        }
    }
}

static void scan_appmeta(app_t **apps, int *count, int *cap, const char *root, int depth)
{
    char real[512];
    DIR *dir = opendir(cpath(root, real, sizeof(real)));
    if (dir == NULL) {
        if (depth == 0)
            log_line("%s: cannot be read", root);
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char folder[400], real_folder[512];
        snprintf(folder, sizeof(folder), "%s/%s", root, e->d_name);
        if (!dir_exists(cpath(folder, real_folder, sizeof(real_folder))))
            continue;
        if (is_title_id(e->d_name))
            add_appmeta_title(apps, count, cap, folder, e->d_name);
        else if (depth == 0)
            scan_appmeta(apps, count, cap, folder, 1); /* such as /user/appmeta/external */
    }
    closedir(dir);
}

static void scan_homebrew(app_t **apps, int *count, int *cap, const char *root)
{
    char real[512];
    DIR *dir = opendir(cpath(root, real, sizeof(real)));
    if (dir == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char sce_sys[400], param[440], id[16];
        snprintf(sce_sys, sizeof(sce_sys), "%s/%s/sce_sys", root, e->d_name);
        snprintf(param, sizeof(param), "%s/param.json", sce_sys);
        if (!title_id_from_param_json(param, id, sizeof(id))) {
            if (!is_title_id(e->d_name))
                continue;
            copy_str(id, sizeof(id), e->d_name);
        }
        app_t *app = find_or_add(apps, count, cap, id);
        if (app == NULL)
            continue;
        copy_str(app->kind, sizeof(app->kind), "homebrew");
        add_art(app, sce_sys);
        if (!app->name[0])
            name_from_param_json(param, app->name, sizeof(app->name));
    }
    closedir(dir);
}

static int by_name(const void *a, const void *b)
{
    const app_t *x = a, *y = b;
    const int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->id, y->id);
}

static void write_report(const app_t *apps, int count)
{
    buf_t text = {0};
    for (int i = 0; i < count; i++) {
        buf_printf(&text, "%s\t%s\t%s\n", apps[i].id, apps[i].kind, apps[i].name);
        for (int j = 0; j < apps[i].nfiles; j++) {
            const art_file_t *f = &apps[i].files[j];
            buf_printf(&text, "\t%s\t%s %dx%d mips %d\t%s%s\n", f->slot, format_name(&f->fmt), f->fmt.w,
                       f->fmt.h, f->fmt.mips, f->path, f->has_old ? "\t(.old kept)" : "");
        }
    }
    char path[512];
    file_write_replace(data_path("scan.txt", path, sizeof(path)), text.data ? text.data : (uint8_t *)"",
                       text.len, NULL);
    buf_free(&text);
}

/* The copy of a title's sce_sys that ShadowMountPlus made in /user/app/<id> when it installed it: the
 * home screen's tile reads it, and nothing copies it again until the title is installed again (a
 * restart only mounts the game), so a restart shows what is written there. A package's /user/app/<id>
 * has none. Skipped when it is a folder the title's files are in already. */
static void add_installed_copy(app_t *app)
{
    char folder[400], real[512];
    struct stat st;
    snprintf(folder, sizeof(folder), "/user/app/%s/sce_sys", app->id);
    if (stat(cpath(folder, real, sizeof(real)), &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    for (int i = 0; i < app->nfiles; i++) {
        char dir[400], real_dir[512];
        struct stat other;
        copy_str(dir, sizeof(dir), app->files[i].path);
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = 0;
        if (stat(cpath(dir, real_dir, sizeof(real_dir)), &other) == 0 && other.st_dev == st.st_dev &&
            other.st_ino == st.st_ino)
            return;
    }
    add_art(app, folder);
}

bool scan_apps(app_t **apps_out, int *count_out)
{
    const uint64_t start = now_ms();
    app_t *apps = NULL;
    int count = 0, cap = 0;
    g_listing.len = 0;
    scan_appmeta(&apps, &count, &cap, "/user/appmeta", 0);
    char listing[512];
    file_write_replace(data_path("appmeta-files.txt", listing, sizeof(listing)),
                       g_listing.data ? g_listing.data : (uint8_t *)"", g_listing.len, NULL);
    for (size_t i = 0; i < sizeof(kHomebrewRoots) / sizeof(kHomebrewRoots[0]); i++)
        scan_homebrew(&apps, &count, &cap, kHomebrewRoots[i]);
    for (int i = 0; i < count; i++)
        add_installed_copy(&apps[i]);

    /* only titles with art to change */
    int kept = 0;
    for (int i = 0; i < count; i++)
        if (apps[i].nfiles > 0)
            apps[kept++] = apps[i];
    count = kept;

    bool unnamed = false;
    for (int i = 0; i < count; i++)
        unnamed |= apps[i].name[0] == 0;
    if (unnamed)
        names_from_app_db(apps, count);
    for (int i = 0; i < count; i++) {
        if (!apps[i].name[0])
            copy_str(apps[i].name, sizeof(apps[i].name), apps[i].id);
        clean_name(apps[i].name);
    }
    matches_apply(apps, count);
    if (count > 1)
        qsort(apps, (size_t)count, sizeof(app_t), by_name);
    write_report(apps, count);
    log_line("scan: %d titles with art in %llu ms", count, (unsigned long long)(now_ms() - start));
    *apps_out = apps;
    *count_out = count;
    return true;
}

/* ---- the SteamGridDB game each title was matched with ---- */

void matches_apply(app_t *apps, int count)
{
    char path[512];
    buf_t text = {0};
    if (!file_read(data_path("matches.tsv", path, sizeof(path)), &text) || text.len == 0) {
        buf_free(&text);
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r((char *)text.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *id = line, *game = strchr(id, '\t');
        if (game == NULL)
            continue;
        *game++ = 0;
        char *name = strchr(game, '\t');
        if (name)
            *name++ = 0;
        for (int i = 0; i < count; i++)
            if (strcmp(apps[i].id, id) == 0) {
                apps[i].sgdb_id = strtol(game, NULL, 10);
                copy_str(apps[i].sgdb_name, sizeof(apps[i].sgdb_name), name ? name : "");
            }
    }
    buf_free(&text);
}

bool matches_set(const char *id, long game, const char *name)
{
    char path[512];
    data_path("matches.tsv", path, sizeof(path));
    buf_t text = {0}, out = {0};
    file_read(path, &text);
    char *save = NULL;
    if (text.len > 0)
        for (char *line = strtok_r((char *)text.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            const size_t id_len = strlen(id);
            if (strncmp(line, id, id_len) == 0 && line[id_len] == '\t')
                continue; /* replaced below */
            buf_printf(&out, "%s\n", line);
        }
    if (game > 0) {
        buf_printf(&out, "%s\t%ld\t", id, game);
        buf_field(&out, name);
        buf_printf(&out, "\n");
    }
    const bool ok = file_write_replace(path, out.data ? out.data : (uint8_t *)"", out.len, NULL);
    buf_free(&text);
    buf_free(&out);
    return ok;
}
