/*
 * Art Swap helper - a logo on a title's home-screen page (logo.h).
 *
 * A game's page on the home screen shows its logo, or else its name in plain
 * text: the home screen draws the picture its database names in the title's
 * logoImageInfo (tbl_conceptmetadata of /system_data/priv/mms/app.db). Store
 * games get theirs from PlayStation Network; homebrew and installed backups
 * have none. Art Swap writes the PNG beside the title's art in /user/appmeta
 * (the home screen cannot read the homebrew folders) and points logoImageInfo
 * at it, editing the database in place with SQLite, which honours the home
 * screen's own locks; the page shows it at once. (ensamvarg360's Spela found
 * the way, and the console showed it on 13.60, 2026-10-23.)
 *
 * Art Swap's own copy of each logo is kept in /data/artswap/logos/<id>.png,
 * and the value it replaced in <id>.orig, for Restore. An install writes the
 * title's entry afresh, without the logo, and ShadowMountPlus's uninstall
 * empties its folder in /user/appmeta: logo_keep puts the logo back from the
 * copy. The database is copied aside before Art Swap changes it (appdb.c).
 *
 * Art Swap's own logo is built into the helper (own_logo.h, made from
 * helper/res/artswap-logo.png by the Makefile) and given to Art Swap's page
 * the first time the helper finds it with none; logos/PPSA99318.given says it
 * was, so a Restore is not undone at the next start.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "logo.h"
#include "appdb.h"
#include "own_logo.h" /* artswap_logo_png, artswap_logo_png_len */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Sony's own are about 2000x500; a larger one is made smaller, as a PNG */
#define LOGO_MAX_W 2048
#define LOGO_MAX_H 1024

static const char *own_path(const char *id, const char *ext, char *out, size_t size)
{
    char name[64];
    snprintf(name, sizeof(name), "logos/%s.%s", id, ext);
    return data_path(name, out, size);
}

bool logo_kept(const char *id)
{
    char path[512];
    return file_exists(own_path(id, "png", path, sizeof(path)));
}

/* The title's folder in /user/appmeta, as the console names it */
static bool logo_folder(const char *id, char *out, size_t size)
{
    static const char *roots[] = {"/user/appmeta", "/user/appmeta/external"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        char real[512];
        snprintf(out, size, "%s/%s", roots[i], id);
        if (dir_exists(cpath(out, real, sizeof(real))))
            return true;
    }
    return false;
}

/* ---- the home screen's database (appdb.c) ---- */

/* The title's logoImageInfo: 1 with it in value ("" for none), 0 when the home screen has no entry for the
 * title, -1 on an error */
static int db_get(sqlite3 *db, const char *id, char *value, size_t size)
{
    sqlite3_stmt *st = NULL;
    int found = -1;
    value[0] = 0;
    if (sqlite3_prepare_v2(db,
                           "SELECT m.logoImageInfo FROM tbl_contentinfo c JOIN tbl_conceptmetadata m "
                           "ON m.localConceptId = c.localConceptId WHERE c.titleId = ?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(st, 0);
            copy_str(value, size, text ? text : "");
            found = 1;
        } else if (rc == SQLITE_DONE) {
            found = 0;
        }
    }
    sqlite3_finalize(st);
    return found;
}

/* Sets the title's logoImageInfo ("" for none) */
static bool db_put(sqlite3 *db, const char *id, const char *value, char *err, size_t errlen)
{
    if (!appdb_backup(db, err, errlen))
        return false;
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(db,
                                 "UPDATE tbl_conceptmetadata SET logoImageInfo = ?1 WHERE localConceptId = "
                                 "(SELECT localConceptId FROM tbl_contentinfo WHERE titleId = ?2)",
                                 -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        if (value[0])
            sqlite3_bind_text(st, 1, value, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, 1);
        sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
    }
    if (!ok)
        snprintf(err, errlen, "The home screen's database was not changed (%s)", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return ok;
}

/* ---- the logo ---- */

/* Art Swap's copy written beside the title's art, and the database pointed at it */
static bool show(sqlite3 *db, const char *id, buf_t *report, char *err, size_t errlen)
{
    char folder[400], dest[420], real[512], own[512], icon[440], like[512];
    if (!logo_folder(id, folder, sizeof(folder))) {
        snprintf(err, errlen, "%s has no folder in /user/appmeta for a logo", id);
        return false;
    }
    snprintf(dest, sizeof(dest), "%s/logo.png", folder);
    snprintf(icon, sizeof(icon), "%s/icon0.png", folder);
    cpath(icon, like, sizeof(like));
    buf_t png = {0};
    bool ok = file_read(own_path(id, "png", own, sizeof(own)), &png) && png.len > 0;
    if (!ok)
        snprintf(err, errlen, "Art Swap's copy of the logo (%s) cannot be read", own);
    if (ok && !file_write_replace(cpath(dest, real, sizeof(real)), png.data, png.len, file_exists(like) ? like : NULL)) {
        snprintf(err, errlen, "%s could not be written", dest);
        ok = false;
    }
    buf_free(&png);
    /* ts: the home screen loads the picture again, as it does for its own art */
    char info[480];
    snprintf(info, sizeof(info), "%s?ts=%lld", dest, (long long)time(NULL));
    ok = ok && db_put(db, id, info, err, errlen);
    if (ok && report != NULL)
        buf_printf(report, "W\t%s\tlogo\n", dest);
    return ok;
}

bool logo_set(const app_t *app, const char *url, buf_t *report, char *err, size_t errlen)
{
    buf_t data = {0}, png = {0};
    image_t img = {0};
    art_format_t fmt;
    bool ok = sgdb_fetch(url, &data, err, errlen) && image_decode(data.data, data.len, &img, err, errlen) &&
              image_probe(data.data, data.len, &fmt);
    if (ok && fmt.container == CONTAINER_PNG && img.w <= LOGO_MAX_W && img.h <= LOGO_MAX_H) {
        png = data; /* as it is */
        data = (buf_t){0};
    } else if (ok) {
        /* too big, or not a PNG: made one, of a size the home screen draws comfortably */
        image_t small = {0};
        ok = image_shrink(&img, LOGO_MAX_W, LOGO_MAX_H, &small);
        const art_format_t like = {.container = CONTAINER_PNG, .w = small.w, .h = small.h, .channels = 4};
        ok = ok && image_encode_like(&small, &like, NULL, 0, &png, err, errlen);
        image_free(&small);
    }
    const int w = img.w, h = img.h;
    image_free(&img);
    buf_free(&data);

    sqlite3 *db = ok ? appdb_open(SQLITE_OPEN_READWRITE, err, errlen) : NULL;
    char before[512] = "";
    const int found = db ? db_get(db, app->id, before, sizeof(before)) : -1;
    if (db != NULL && found == 0)
        snprintf(err, errlen, "The home screen has no entry for %s, so it has no page for a logo", app->name);
    else if (db != NULL && found < 0)
        snprintf(err, errlen, "The home screen's database could not be read (%s)", sqlite3_errmsg(db));
    ok = found == 1;

    char folder[512], own[512], orig[512];
    mkdirs(data_path("logos", folder, sizeof(folder)));
    own_path(app->id, "png", own, sizeof(own));
    own_path(app->id, "orig", orig, sizeof(orig));
    /* the first time: what the page had, kept for Restore; a logo.png of its own beside the art as logo.png.old */
    if (ok && !file_exists(orig)) {
        char meta[400], dest[420], real[512], old[440], real_old[512];
        if (logo_folder(app->id, meta, sizeof(meta))) {
            snprintf(dest, sizeof(dest), "%s/logo.png", meta);
            snprintf(old, sizeof(old), "%s.old", dest);
            cpath(dest, real, sizeof(real));
            cpath(old, real_old, sizeof(real_old));
            if (file_exists(real) && !file_exists(real_old) && rename(real, real_old) == 0)
                buf_printf(report, "O\t%s\n", old);
        }
        ok = file_write_replace(orig, before, strlen(before), NULL);
        if (!ok)
            snprintf(err, errlen, "What %s's page had could not be kept, so nothing was changed", app->name);
    }
    if (ok && !file_write_replace(own, png.data, png.len, NULL)) {
        snprintf(err, errlen, "The logo could not be kept in %s", own);
        ok = false;
    }
    ok = ok && show(db, app->id, report, err, errlen);
    sqlite3_close(db);
    buf_free(&png);
    log_line("logo %s: %dx%d from %s%s%s", app->id, w, h, url, ok ? "" : ": ", ok ? "" : err);
    return ok;
}

bool logo_restore(const app_t *app, buf_t *report, char *err, size_t errlen)
{
    char own[512], orig[512];
    own_path(app->id, "png", own, sizeof(own));
    own_path(app->id, "orig", orig, sizeof(orig));
    if (!file_exists(own)) {
        snprintf(err, errlen, "%s has no logo from Art Swap", app->name);
        return false;
    }
    buf_t kept = {0};
    char before[512] = "";
    if (file_read(orig, &kept) && kept.len > 0)
        copy_str(before, sizeof(before), (const char *)kept.data);
    buf_free(&kept);
    sqlite3 *db = appdb_open(SQLITE_OPEN_READWRITE, err, errlen);
    char now[512];
    const int found = db ? db_get(db, app->id, now, sizeof(now)) : -1;
    /* no entry: the title has left the home screen, and nothing there needs putting back */
    bool ok = found == 0 || (found == 1 && db_put(db, app->id, before, err, errlen));
    if (db != NULL && found < 0)
        snprintf(err, errlen, "The home screen's database could not be read (%s)", sqlite3_errmsg(db));
    sqlite3_close(db);
    char meta[400], dest[420], real[512], old[440], real_old[512];
    if (ok && logo_folder(app->id, meta, sizeof(meta))) {
        snprintf(dest, sizeof(dest), "%s/logo.png", meta);
        snprintf(old, sizeof(old), "%s.old", dest);
        cpath(dest, real, sizeof(real));
        if (file_exists(cpath(old, real_old, sizeof(real_old))))
            rename(real_old, real);
        else
            unlink(real);
        buf_printf(report, "R\t%s\n", dest);
    }
    if (ok) {
        unlink(own);
        unlink(orig);
    }
    log_line("logo %s: restored%s%s", app->id, ok ? "" : ": ", ok ? "" : err);
    return ok;
}

int logo_keep(const char *id, int wait_s)
{
    char folder[512];
    DIR *dir = opendir(data_path("logos", folder, sizeof(folder)));
    if (dir == NULL)
        return 0;
    char ids[256][16];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < 256) {
        const size_t len = strlen(e->d_name);
        if (len != 13 || strcmp(e->d_name + 9, ".png") != 0)
            continue;
        snprintf(ids[n], sizeof(ids[n]), "%.9s", e->d_name);
        if (id == NULL || strcmp(ids[n], id) == 0)
            n++;
    }
    closedir(dir);
    if (n == 0)
        return 0;
    pthread_mutex_lock(&g_apply_lock);
    char err[300] = "";
    sqlite3 *db = appdb_open(SQLITE_OPEN_READWRITE, err, sizeof(err));
    int put = 0;
    for (int i = 0; i < n && db != NULL; i++) {
        char value[512], meta[400], dest[420], real[512];
        int found = db_get(db, ids[i], value, sizeof(value));
        for (int waited = 0; found == 0 && waited < wait_s; waited++) {
            sleep(1);
            found = db_get(db, ids[i], value, sizeof(value));
        }
        if (found != 1) {
            if (found == 0 && id != NULL)
                log_line("logo %s: not on the home screen after %d s, so not put back yet", ids[i], wait_s);
            continue;
        }
        const bool here = logo_folder(ids[i], meta, sizeof(meta));
        snprintf(dest, sizeof(dest), "%s/logo.png", meta);
        if (here && strncmp(value, dest, strlen(dest)) == 0 && file_exists(cpath(dest, real, sizeof(real))))
            continue; /* it shows */
        if (show(db, ids[i], NULL, err, sizeof(err))) {
            put++;
            log_line("logo %s: put back on its page", ids[i]);
        } else {
            log_line("logo %s: not put back: %s", ids[i], err);
        }
    }
    if (db == NULL)
        log_line("logo: %s", err);
    sqlite3_close(db);
    pthread_mutex_unlock(&g_apply_lock);
    return put;
}

bool logo_own(void)
{
    char given[512], own[512], orig[512], folder[512];
    if (file_exists(own_path(ARTSWAP_ID, "given", given, sizeof(given))))
        return false;
    mkdirs(data_path("logos", folder, sizeof(folder)));
    if (logo_kept(ARTSWAP_ID)) {
        /* chosen in Art Swap already */
        file_write_replace(given, "", 0, NULL);
        return false;
    }
    pthread_mutex_lock(&g_apply_lock);
    char err[300] = "", value[512] = "", meta[400];
    sqlite3 *db = appdb_open(SQLITE_OPEN_READWRITE, err, sizeof(err));
    const int found = db ? db_get(db, ARTSWAP_ID, value, sizeof(value)) : -1;
    bool put = false;
    /* no entry yet, or no folder for the art: again at the next scan */
    if (found == 1 && value[0]) {
        log_line("logo %s: its page has a logo already, so Art Swap's own is not put there", ARTSWAP_ID);
        file_write_replace(given, "", 0, NULL);
    } else if (found == 1 && logo_folder(ARTSWAP_ID, meta, sizeof(meta))) {
        own_path(ARTSWAP_ID, "png", own, sizeof(own));
        own_path(ARTSWAP_ID, "orig", orig, sizeof(orig));
        put = file_write_replace(own, artswap_logo_png, artswap_logo_png_len, NULL) &&
              file_write_replace(orig, "", 0, NULL) && show(db, ARTSWAP_ID, NULL, err, sizeof(err));
        if (put) {
            file_write_replace(given, "", 0, NULL);
            log_line("logo %s: Art Swap's own logo put on its page", ARTSWAP_ID);
        } else {
            unlink(own);
            unlink(orig);
            log_line("logo %s: Art Swap's own logo not put on its page: %s", ARTSWAP_ID, err[0] ? err : "not written");
        }
    }
    sqlite3_close(db);
    pthread_mutex_unlock(&g_apply_lock);
    return put;
}

bool logo_current(const app_t *app, int max_w, int max_h, image_t *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    char path[512];
    own_path(app->id, "png", path, sizeof(path));
    if (!file_exists(path)) {
        /* not Art Swap's: the picture the database names, if any */
        sqlite3 *db = appdb_open(SQLITE_OPEN_READONLY, err, errlen);
        char value[512] = "";
        const int found = db ? db_get(db, app->id, value, sizeof(value)) : -1;
        sqlite3_close(db);
        value[strcspn(value, "?")] = 0;
        if (found != 1 || value[0] != '/') {
            snprintf(err, errlen, "%s has no logo: its page shows its name", app->name);
            return false;
        }
        cpath(value, path, sizeof(path));
    }
    buf_t data = {0};
    image_t full = {0};
    bool ok = file_read(path, &data) && image_decode(data.data, data.len, &full, err, errlen);
    if (ok && !(ok = image_shrink(&full, max_w, max_h, out)))
        snprintf(err, errlen, "out of memory");
    else if (!ok && !err[0])
        snprintf(err, errlen, "%s's logo cannot be read", app->name);
    buf_free(&data);
    image_free(&full);
    return ok;
}
