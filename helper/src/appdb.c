/*
 * Art Swap helper - the home screen's database (appdb.h).
 *
 * /system_data/priv/mms/app.db holds what the home screen shows of each
 * title: its name, the files of its art, a logo (logo.c). Art Swap edits it in
 * place with SQLite, which honours the home screen's own locks. Before Art Swap
 * first changes it, it is copied to /data/artswap/app.db.first, and before each
 * run's first change to app.db.backup, through SQLite's backup, so each copy
 * is whole.
 *
 * A title's install writes its name in several places, and the home screen
 * shows what they hold (a restart of it shows no new name of param.json's):
 * - tbl_contentinfo: titleName, and in its AppInfoJson TITLE and a
 *   TITLE_<language> for each language its param.json names (01 is US
 *   English);
 * - tbl_conceptmetadata, for a concept of the title's own (homebrew's is its
 *   number): conceptName, and in its ConceptInfoJson concept_name and
 *   concept_name_<language>, each with its length in bytes beside it;
 * - each user's tbl_iconinfo_<user> (titleName) and tbl_concepticoninfo_<user>
 *   (primaryTitleName, and conceptName for its own concept).
 * appdb_rename writes them all in one transaction. It replaces the install a
 * rename once needed, which deleted the title's download data (smp.c). The
 * JSON is edited as text, so only those values change, byte for byte: a
 * parser would round its 64-bit numbers. (The console's app.db, 13.60,
 * 2026-10-23.)
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "appdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define APP_DB "/system_data/priv/mms/app.db"

static bool g_backed_up; /* this run has copied the database aside */

sqlite3 *appdb_open(int flags, char *err, size_t errlen)
{
    char real[512];
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(cpath(APP_DB, real, sizeof(real)), &db, flags, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "The home screen's database did not open (%s)", db ? sqlite3_errmsg(db) : "no memory");
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 10000); /* the home screen holds its lock only briefly */
    return db;
}

/* A whole copy of the open database, through SQLite's backup */
static bool db_copy(sqlite3 *db, const char *to)
{
    char partial[520];
    snprintf(partial, sizeof(partial), "%s.new", to);
    unlink(partial);
    sqlite3 *out = NULL;
    bool ok = sqlite3_open(partial, &out) == SQLITE_OK;
    sqlite3_backup *backup = ok ? sqlite3_backup_init(out, "main", db, "main") : NULL;
    ok = backup != NULL;
    int rc = SQLITE_OK;
    for (int tries = 0; ok && tries < 100; tries++) {
        rc = sqlite3_backup_step(backup, -1);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
            break;
        sqlite3_sleep(100);
    }
    if (backup != NULL)
        sqlite3_backup_finish(backup);
    ok = ok && rc == SQLITE_DONE;
    sqlite3_close(out);
    ok = ok && rename(partial, to) == 0;
    if (!ok)
        unlink(partial);
    return ok;
}

bool appdb_backup(sqlite3 *db, char *err, size_t errlen)
{
    if (g_backed_up)
        return true;
    char first[512], latest[512];
    data_path("app.db.first", first, sizeof(first));
    data_path("app.db.backup", latest, sizeof(latest));
    if ((!file_exists(first) && !db_copy(db, first)) || !db_copy(db, latest)) {
        snprintf(err, errlen, "The home screen's database could not be copied aside first, so it was not changed");
        return false;
    }
    log_line("app.db: copied to %s", latest);
    g_backed_up = true;
    return true;
}

bool appdb_has_column(sqlite3 *db, const char *table, const char *column)
{
    sqlite3_stmt *st = NULL;
    bool has = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info(?1) WHERE name = ?2", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, column, -1, SQLITE_TRANSIENT);
        has = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return has;
}

int appdb_title(const char *id, appdb_title_t *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    sqlite3 *db = appdb_open(SQLITE_OPEN_READONLY, err, errlen);
    if (db == NULL)
        return -1;
    out->art_known = appdb_has_column(db, "tbl_contentinfo", "pic0Info") && appdb_has_column(db, "tbl_contentinfo", "snd0Info");
    sqlite3_stmt *st = NULL;
    int found = -1;
    if (sqlite3_prepare_v2(db,
                           out->art_known ? "SELECT titleName, pic0Info, snd0Info FROM tbl_contentinfo WHERE titleId = ?1"
                                          : "SELECT titleName, '', '' FROM tbl_contentinfo WHERE titleId = ?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            char *fields[] = {out->name, out->pic0, out->snd0};
            const size_t sizes[] = {sizeof(out->name), sizeof(out->pic0), sizeof(out->snd0)};
            for (int i = 0; i < 3; i++) {
                const char *text = (const char *)sqlite3_column_text(st, i);
                copy_str(fields[i], sizes[i], text ? text : "");
            }
            found = 1;
        } else if (rc == SQLITE_DONE) {
            found = 0;
        }
    }
    if (found < 0)
        snprintf(err, errlen, "The home screen's database could not be read (%s)", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return found;
}

/* ---- the names inside the entries' JSON, edited as text ---- */

#define EDITS_MAX 80

typedef struct {
    size_t at, len; /* the bytes replaced */
    char with[600]; /* by these */
} edit_t;

typedef struct {
    edit_t list[EDITS_MAX];
    int n;
} edits_t;

static size_t skip_space(const char *s, size_t i, size_t n)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    return i;
}

/* The string whose opening quote is at s[i]: the index after its closing quote, 0 when it is not whole. Its
 * text, escapes as they are (enough for the keys looked for), goes into out when out is not NULL. */
static size_t string_end(const char *s, size_t i, size_t n, char *out, size_t size)
{
    if (i >= n || s[i] != '"')
        return 0;
    size_t o = 0;
    for (i++; i < n; i++) {
        if (s[i] == '"') {
            if (out != NULL)
                out[o] = 0;
            return i + 1;
        }
        if (out != NULL && o + 1 < size)
            out[o++] = s[i];
        if (s[i] == '\\') {
            if (++i >= n)
                return 0;
            if (out != NULL && o + 1 < size)
                out[o++] = s[i];
        }
    }
    return 0;
}

/* The index after the value starting at s[i], 0 when it is not whole */
static size_t value_end(const char *s, size_t i, size_t n)
{
    if (i >= n)
        return 0;
    if (s[i] == '"')
        return string_end(s, i, n, NULL, 0);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < n) {
            if (s[i] == '"') {
                const size_t end = string_end(s, i, n, NULL, 0);
                if (end == 0)
                    return 0;
                i = end;
                continue;
            }
            if (s[i] == '{' || s[i] == '[')
                depth++;
            else if ((s[i] == '}' || s[i] == ']') && --depth == 0)
                return i + 1;
            i++;
        }
        return 0;
    }
    const size_t start = i;
    while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' &&
           s[i] != '\r')
        i++;
    return i > start ? i : 0;
}

/* For key prefix ("TITLE"), prefix_NN ("TITLE_01"): -1 for the prefix alone, NN for a language, -2 otherwise */
static int language_of(const char *key, const char *prefix)
{
    const size_t n = strlen(prefix);
    if (strncmp(key, prefix, n) != 0)
        return -2;
    if (key[n] == 0)
        return -1;
    if (key[n] == '_' && key[n + 1] >= '0' && key[n + 1] <= '9' && key[n + 2] >= '0' && key[n + 2] <= '9' &&
        key[n + 3] == 0)
        return (key[n + 1] - '0') * 10 + (key[n + 2] - '0');
    return -2;
}

static const char *name_for(const names_t *names, int language)
{
    return language >= 0 && language < NAME_LANGUAGES && names->lang[language][0] ? names->lang[language]
                                                                                    : names->def;
}

/* name as a JSON string, quotes and all, as the console writes them: UTF-8 as it is, " \ / and control
 * characters escaped */
static bool quoted(const char *name, char *out, size_t size)
{
    size_t o = 0;
    out[o++] = '"';
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        char piece[8] = {(char)*p, 0};
        if (*p == '"' || *p == '\\' || *p == '/') {
            piece[0] = '\\';
            piece[1] = (char)*p;
            piece[2] = 0;
        } else if (*p < 0x20) {
            snprintf(piece, sizeof(piece), "\\u%04x", *p);
        }
        const size_t len = strlen(piece);
        if (o + len + 2 > size)
            return false;
        memcpy(out + o, piece, len);
        o += len;
    }
    out[o++] = '"';
    out[o] = 0;
    return true;
}

static bool add_edit(edits_t *edits, size_t at, size_t len, const char *with)
{
    if (edits->n >= EDITS_MAX || strlen(with) >= sizeof(edits->list[0].with))
        return false;
    edit_t *e = &edits->list[edits->n++];
    e->at = at;
    e->len = len;
    copy_str(e->with, sizeof(e->with), with);
    return true;
}

/* json with the edits made (malloc'd); they are put in order first */
static char *apply_edits(const char *json, edits_t *edits)
{
    for (int i = 1; i < edits->n; i++)
        for (int j = i; j > 0 && edits->list[j - 1].at > edits->list[j].at; j--) {
            const edit_t t = edits->list[j];
            edits->list[j] = edits->list[j - 1];
            edits->list[j - 1] = t;
        }
    const size_t n = strlen(json);
    size_t size = n + 1;
    for (int i = 0; i < edits->n; i++)
        size += strlen(edits->list[i].with);
    char *out = malloc(size);
    if (out == NULL)
        return NULL;
    size_t from = 0, o = 0;
    for (int i = 0; i < edits->n; i++) {
        const edit_t *e = &edits->list[i];
        if (e->at < from) { /* overlapping: not expected */
            free(out);
            return NULL;
        }
        memcpy(out + o, json + from, e->at - from);
        o += e->at - from;
        const size_t len = strlen(e->with);
        memcpy(out + o, e->with, len);
        o += len;
        from = e->at + e->len;
    }
    memcpy(out + o, json + from, n - from);
    o += n - from;
    out[o] = 0;
    return out;
}

/* The members of the object at json[i]: for each, its key and where its value lies. visit returns false to stop
 * (the whole walk then fails). The index after the object, 0 when it is not a whole object. */
typedef bool (*member_fn)(const char *json, size_t n, const char *key, size_t value, size_t end, void *ctx);

static size_t walk_object(const char *json, size_t n, size_t i, member_fn visit, void *ctx)
{
    i = skip_space(json, i, n);
    if (i >= n || json[i] != '{')
        return 0;
    i = skip_space(json, i + 1, n);
    if (i < n && json[i] == '}')
        return i + 1;
    for (;;) {
        char key[40];
        const size_t key_end = string_end(json, i, n, key, sizeof(key));
        if (key_end == 0)
            return 0;
        i = skip_space(json, key_end, n);
        if (i >= n || json[i] != ':')
            return 0;
        const size_t value = skip_space(json, i + 1, n);
        const size_t end = value_end(json, value, n);
        if (end == 0 || !visit(json, n, key, value, end, ctx))
            return 0;
        i = skip_space(json, end, n);
        if (i < n && json[i] == ',') {
            i = skip_space(json, i + 1, n);
            continue;
        }
        return i < n && json[i] == '}' ? i + 1 : 0;
    }
}

typedef struct {
    const names_t *names;
    edits_t *edits;
    int changed;
} rename_ctx_t;

/* AppInfoJson: TITLE, TITLE_NN */
static bool app_info_member(const char *json, size_t n, const char *key, size_t value, size_t end, void *ctx)
{
    rename_ctx_t *c = ctx;
    const int language = language_of(key, "TITLE");
    if (language == -2 || json[value] != '"')
        return true;
    char with[600];
    if (!quoted(name_for(c->names, language), with, sizeof(with)) || !add_edit(c->edits, value, end - value, with))
        return false;
    c->changed++;
    return true;
}

/* One entry of ConceptInfoJson's field_list: {"data": ..., "key": ..., "size": ..., "type": ...} */
typedef struct {
    char key[40];
    size_t data, data_end, size, size_end;
} field_t;

static bool field_member(const char *json, size_t n, const char *key, size_t value, size_t end, void *ctx)
{
    field_t *f = ctx;
    if (strcmp(key, "key") == 0 && json[value] == '"')
        string_end(json, value, n, f->key, sizeof(f->key));
    else if (strcmp(key, "data") == 0 && json[value] == '"') {
        f->data = value;
        f->data_end = end;
    } else if (strcmp(key, "size") == 0 && json[value] >= '0' && json[value] <= '9') {
        f->size = value;
        f->size_end = end;
    }
    return true;
}

static bool concept_member(const char *json, size_t n, const char *key, size_t value, size_t end, void *ctx)
{
    rename_ctx_t *c = ctx;
    if (strcmp(key, "field_list") != 0 || json[value] != '[')
        return true;
    size_t i = skip_space(json, value + 1, n);
    while (i < end && json[i] != ']') {
        field_t f;
        memset(&f, 0, sizeof(f));
        const size_t after = walk_object(json, n, i, field_member, &f);
        if (after == 0)
            return false;
        const int language = language_of(f.key, "concept_name");
        if (language != -2 && f.data_end > 0 && f.size_end > 0) {
            const char *name = name_for(c->names, language);
            char with[600], length[24];
            snprintf(length, sizeof(length), "%zu", strlen(name));
            if (!quoted(name, with, sizeof(with)) || !add_edit(c->edits, f.data, f.data_end - f.data, with) ||
                !add_edit(c->edits, f.size, f.size_end - f.size, length))
                return false;
            c->changed++;
        }
        i = skip_space(json, after, n);
        if (i < end && json[i] == ',')
            i = skip_space(json, i + 1, n);
    }
    return true;
}

/* The JSON with its names changed (malloc'd), or NULL when it is not what the console writes. *changed: how many. */
static char *json_renamed(const char *json, bool concept, const names_t *names, int *changed)
{
    edits_t *edits = calloc(1, sizeof(*edits));
    if (edits == NULL)
        return NULL;
    rename_ctx_t c = {names, edits, 0};
    const size_t n = strlen(json);
    char *out = walk_object(json, n, 0, concept ? concept_member : app_info_member, &c) != 0
                    ? apply_edits(json, edits)
                    : NULL;
    *changed = c.changed;
    free(edits);
    return out;
}

/* ---- the rename ---- */

static bool run(sqlite3 *db, const char *sql, const char *a, const char *b, const char *c)
{
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK;
    const char *values[] = {a, b, c};
    for (int i = 0; ok && i < 3; i++)
        if (values[i] != NULL)
            sqlite3_bind_text(st, i + 1, values[i], -1, SQLITE_TRANSIENT);
    ok = ok && sqlite3_step(st) == SQLITE_DONE;
    if (!ok)
        log_line("app.db: %s: %s", sql, sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return ok;
}

/* The first column of the first row, as text (malloc'd), or NULL */
static char *one_text(sqlite3 *db, const char *sql, const char *a)
{
    sqlite3_stmt *st = NULL;
    char *text = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) != NULL)
            text = strdup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return text;
}

/* A table's name as the database lists it: letters, digits and _ only, so it can go into the SQL */
static bool plain(const char *name)
{
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return false;
    return name[0] != 0;
}

#define USER_TABLES_MAX 48

bool appdb_rename(const char *id, const names_t *names, char *err, size_t errlen)
{
    err[0] = 0;
    sqlite3 *db = appdb_open(SQLITE_OPEN_READWRITE, err, errlen);
    if (db == NULL)
        return false;
    bool ok = appdb_backup(db, err, errlen);
    if (ok && sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "The home screen's database is busy (%s)", sqlite3_errmsg(db));
        ok = false;
    }
    const bool begun = ok;
    const char *name = names->def;
    char *local = NULL, *info = NULL, *new_info = NULL, *concept = NULL, *new_concept = NULL;
    int titles = 0, concepts = 0, users = 0;

    /* the title's entry */
    if (ok) {
        local = one_text(db, "SELECT ifnull(localConceptId, '') FROM tbl_contentinfo WHERE titleId = ?1", id);
        if (local == NULL) {
            snprintf(err, errlen, "%s has no entry in the home screen's database", id);
            ok = false;
        }
    }
    if (ok && appdb_has_column(db, "tbl_contentinfo", "AppInfoJson") &&
        (info = one_text(db, "SELECT AppInfoJson FROM tbl_contentinfo WHERE titleId = ?1", id)) != NULL &&
        info[0] != 0) {
        new_info = json_renamed(info, false, names, &titles);
        if (new_info == NULL) {
            snprintf(err, errlen, "%s's entry in the home screen's database is not as Art Swap knows it", id);
            ok = false;
        }
    }
    ok = ok && (new_info ? run(db, "UPDATE tbl_contentinfo SET titleName = ?1, AppInfoJson = ?2 WHERE titleId = ?3",
                               name, new_info, id)
                         : run(db, "UPDATE tbl_contentinfo SET titleName = ?1 WHERE titleId = ?2", name, id, NULL));

    /* its concept, when it is the title's own (homebrew's: its number), not a store game's shared by others */
    bool own = false;
    if (ok && local[0] != 0) {
        char *count = one_text(db, "SELECT count(*) FROM tbl_contentinfo WHERE localConceptId = ?1", local);
        own = count != NULL && strcmp(count, "1") == 0;
        free(count);
    }
    if (ok && own && appdb_has_column(db, "tbl_conceptmetadata", "ConceptInfoJson") &&
        (concept = one_text(db, "SELECT ConceptInfoJson FROM tbl_conceptmetadata WHERE localConceptId = ?1", local)) !=
            NULL &&
        concept[0] != 0) {
        new_concept = json_renamed(concept, true, names, &concepts);
        if (new_concept == NULL) {
            snprintf(err, errlen, "%s's concept in the home screen's database is not as Art Swap knows it", id);
            ok = false;
        }
    }
    if (ok && own && appdb_has_column(db, "tbl_conceptmetadata", "conceptName"))
        ok = new_concept ? run(db,
                               "UPDATE tbl_conceptmetadata SET conceptName = ?1, ConceptInfoJson = ?2 "
                               "WHERE localConceptId = ?3",
                               name, new_concept, local)
                         : run(db, "UPDATE tbl_conceptmetadata SET conceptName = ?1 WHERE localConceptId = ?2", name,
                               local, NULL);

    /* each user's tables */
    static char tables[USER_TABLES_MAX][64];
    int ntables = 0;
    sqlite3_stmt *st = NULL;
    if (ok && sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table'", -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW && ntables < USER_TABLES_MAX) {
            const char *table = (const char *)sqlite3_column_text(st, 0);
            if (table != NULL && plain(table) && strlen(table) < sizeof(tables[0]) &&
                (strncmp(table, "tbl_iconinfo_", 13) == 0 || strncmp(table, "tbl_concepticoninfo_", 20) == 0 ||
                 strncmp(table, "tbl_appbrowse_", 14) == 0))
                copy_str(tables[ntables++], sizeof(tables[0]), table);
        }
    sqlite3_finalize(st);
    for (int i = 0; ok && i < ntables; i++) {
        char sql[200];
        const bool icons = strncmp(tables[i], "tbl_concepticoninfo_", 20) != 0;
        if (icons && appdb_has_column(db, tables[i], "titleName") && appdb_has_column(db, tables[i], "titleId")) {
            snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET titleName = ?1 WHERE titleId = ?2", tables[i]);
            ok = run(db, sql, name, id, NULL);
            users++;
        }
        if (!icons && appdb_has_column(db, tables[i], "primaryTitleName") && appdb_has_column(db, tables[i], "primaryTitleId")) {
            snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET primaryTitleName = ?1 WHERE primaryTitleId = ?2", tables[i]);
            ok = run(db, sql, name, id, NULL);
            users++;
        }
        if (ok && !icons && own && appdb_has_column(db, tables[i], "conceptName") &&
            appdb_has_column(db, tables[i], "localConceptId")) {
            snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET conceptName = ?1 WHERE localConceptId = ?2", tables[i]);
            ok = run(db, sql, name, local, NULL);
        }
    }

    if (begun && ok && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "The home screen's database did not take the name (%s)", sqlite3_errmsg(db));
        ok = false;
    }
    if (begun && !ok) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (err[0] == 0)
            snprintf(err, errlen, "The home screen's database was not changed (%s)", sqlite3_errmsg(db));
    }
    log_line("app.db: %s named \"%s\"%s (%d title names, %d concept names, %d user tables)%s%s", id, name,
             ok ? "" : " NOT", titles, concepts, users, ok ? "" : ": ", ok ? "" : err);
    free(local);
    free(info);
    free(new_info);
    free(concept);
    free(new_concept);
    sqlite3_close(db);
    return ok;
}
