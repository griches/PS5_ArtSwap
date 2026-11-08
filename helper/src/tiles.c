/*
 * Art Swap helper - the home screen's tiles in its databases (tiles.h).
 *
 * Ported from bps2414's Lineup (app/payloads/lineupd.c, GPL-3.0-or-later,
 * https://github.com/bps2414/lineup), which found how the home screen keeps
 * its tiles in /system_data/priv/mms/app.db and appinfo.db, and how to make it
 * keep what was chosen. The places themselves were first found by
 * elripalda's PSGFX.
 *
 * - A tile's tab is kept in many places, and all of them must agree, or the
 *   console puts it back on the tab its install names: an app's dispLocation
 *   is 138 on Games and 188 on Media; the system's own tiles keep it as bits,
 *   0x02 Games and 0x04 Media, with 0x01 for a tile the system hides.
 * - The console writes those places again at a restart, out of rest mode and
 *   at an install. So what Art Swap changes it keeps in tables of its own
 *   (artswap_*, in both databases), and triggers put it back inside the
 *   console's own writes. When the helper starts, it writes again whatever
 *   the triggers could not (tiles_startup).
 * - Hiding and order are each profile's (tbl_iconinfo_<user>); the tab is the
 *   console's (tbl_contentinfo, appinfo.db), so a move is on every profile.
 * - A trigger is made only on columns the firmware has: one naming a column
 *   that is not there would fail the console's own writes.
 *
 * Copyright (C) 2026 Gary Riches
 * Ported from Lineup, Copyright (C) 2026 bps2414
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tiles.h"
#include "appdb.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define APPINFO_DB "/system_data/priv/mms/appinfo.db"
#define HOME_DIR "/user/home"

#define BIT_HIDDEN 1
#define BIT_GAMES 2
#define BIT_MEDIA 4
#define MEDIA_CATEGORY 65536
/* The home screen shows this many of a row's tiles; the rest are in the library */
#define GAMES_HOME 12
#define MEDIA_HOME 11

#define STORE_ID "NPXS40047"
#define GALLERY_ID "NPXS40075"
#define WELCOME_ID "NPXS40063"
#define GAME_LIBRARY_ID "NPXS40071"
#define APP_LIBRARY_ID "NPXS40139"

/* The system's tiles that are ordered with the apps (sortPriority 100, as theirs): the Media Gallery, Music, Remote
 * Play and Share Play. The rest are hubs the home screen keeps at the front of a row by their priority (4 to 13),
 * and the libraries at its end (1000). */
#define ROW_SYSTEM "('NPXS40075', 'NPXS40054', 'NPXS40074', 'NPXS40099')"

/* Disc placeholders and such: never shown (PSGFX's list, less Welcome, which Art Swap shows) */
static const char *kInternal[] = {"NPXS40140", "NPXS40144", "NPXS40145", "NPXS40148", "NPXS40149",
                                  "NPXS40150", "NPXS40205", "NPXS40106"};

static bool internal(const char *id)
{
    for (size_t i = 0; i < sizeof(kInternal) / sizeof(kInternal[0]); i++)
        if (strcmp(id, kInternal[i]) == 0)
            return true;
    return false;
}

static bool library(const char *id)
{
    return strcmp(id, GAME_LIBRARY_ID) == 0 || strcmp(id, APP_LIBRARY_ID) == 0;
}

static bool row_system(const char *id)
{
    return strcmp(id, GALLERY_ID) == 0 || strcmp(id, "NPXS40054") == 0 || strcmp(id, "NPXS40074") == 0 ||
           strcmp(id, "NPXS40099") == 0;
}

static bool is_system(const char *id)
{
    return strncmp(id, "NPXS", 4) == 0;
}

/* The tab bit of a system tile's value: BIT_GAMES, BIT_MEDIA, or 0 on neither */
static int tab_bit(long long value)
{
    return value & BIT_GAMES ? BIT_GAMES : value & BIT_MEDIA ? BIT_MEDIA : 0;
}

/* ---- SQL ---- */

static bool user_id(const char *id)
{
    if (id == NULL || strlen(id) != 10)
        return false;
    for (int i = 0; i < 10; i++)
        if (!isdigit((unsigned char)id[i]))
            return false;
    return true;
}

/* Whether `schema` (main or info) has a table or trigger named `name` */
static bool has_object_in(sqlite3 *db, const char *schema, const char *name)
{
    char sql[96];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM %s.sqlite_master WHERE name = ?", schema);
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return found;
}

static bool has_object(sqlite3 *db, const char *name)
{
    return has_object_in(db, "main", name);
}

/* Whether `schema`.`table` has `column`: a firmware may not have it */
static bool has_column(sqlite3 *db, const char *schema, const char *table, const char *column)
{
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info(?1, ?2) WHERE name = ?3", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, schema, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, column, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return found;
}

/* Whether `schema`.`table` has every column of `columns` (NULL-terminated) */
static bool has_columns(sqlite3 *db, const char *schema, const char *table, const char *const *columns)
{
    for (; *columns; columns++)
        if (!has_column(db, schema, table, *columns))
            return false;
    return true;
}

/* Runs `sql` with ?1 the number and ?2, ?3 the texts, as far as it has them. The rows changed, or -1. */
static int run_values(sqlite3 *db, const char *sql, long long number, const char *second, const char *third)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        log_line("tiles: %s: %s", sqlite3_errmsg(db), sql);
        return -1;
    }
    const int params = sqlite3_bind_parameter_count(st);
    if (params >= 1)
        sqlite3_bind_int64(st, 1, number);
    if (params >= 2)
        sqlite3_bind_text(st, 2, second, -1, SQLITE_STATIC);
    if (params >= 3)
        sqlite3_bind_text(st, 3, third, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        log_line("tiles: %s: %s", sqlite3_errmsg(db), sql);
        return -1;
    }
    return sqlite3_changes(db);
}

/* Runs `sql` with ?1 the title and ?2 the user, when it has a ?2. The rows changed, or -1. */
static int run_title(sqlite3 *db, const char *sql, const char *user, const char *title)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        log_line("tiles: %s: %s", sqlite3_errmsg(db), sql);
        return -1;
    }
    sqlite3_bind_text(st, 1, title, -1, SQLITE_STATIC);
    if (sqlite3_bind_parameter_count(st) > 1)
        sqlite3_bind_text(st, 2, user, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        log_line("tiles: %s: %s", sqlite3_errmsg(db), sql);
        return -1;
    }
    return sqlite3_changes(db);
}

/* Whether `sql`, with ?1 the text when it has one, finds a row: 1, 0, or -1 */
static int finds_row(sqlite3 *db, const char *sql, const char *text)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_bind_parameter_count(st))
        sqlite3_bind_text(st, 1, text, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

static bool exec(sqlite3 *db, const char *sql)
{
    char *error = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &error) == SQLITE_OK)
        return true;
    log_line("tiles: %s", error ? error : sqlite3_errmsg(db));
    sqlite3_free(error);
    return false;
}

/* The first column of `sql`'s first row as text, "failed" when it cannot; 0 when that is "ok" */
static int pragma_text(sqlite3 *db, const char *sql, char *out, size_t size)
{
    sqlite3_stmt *st = NULL;
    copy_str(out, size, "failed");
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        copy_str(out, size, (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return strcmp(out, "ok") == 0 ? 0 : -1;
}

/* `json` when it is valid, else an empty object: json_each fails the whole statement on malformed JSON */
#define SAFE_JSON(json) "CASE WHEN json_valid(" json ") THEN " json " ELSE '{}' END"

/* ---- profiles ---- */

int tiles_users(sqlite3 *db, char users[][16], int max)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT substr(name, 14) FROM main.sqlite_master WHERE type = 'table' AND name GLOB "
                               "'tbl_iconinfo_[0-9]*' ORDER BY name",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    int count = 0, rc = SQLITE_DONE;
    while (count < max && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *user = (const char *)sqlite3_column_text(st, 0);
        if (user_id(user))
            copy_str(users[count++], 16, user);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? count : -1;
}

bool tiles_user_known(sqlite3 *db, const char *user)
{
    char table[64];
    if (!user_id(user))
        return false;
    snprintf(table, sizeof(table), "tbl_iconinfo_%s", user);
    return has_object(db, table);
}

int tiles_profiles(sqlite3 *db, tiles_profile_t *out, int max)
{
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0)
        return -1;
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        tiles_profile_t *p = &out[n++];
        copy_str(p->user, sizeof(p->user), users[i]);
        copy_str(p->name, sizeof(p->name), users[i]);
        /* the profile's folder is its id in eight hex digits: 1000000001 is 3b9aca01 */
        char home[512], path[512];
        buf_t name = {0};
        DIR *dir = opendir(cpath(HOME_DIR, home, sizeof(home)));
        for (struct dirent *e; dir && (e = readdir(dir)) != NULL;) {
            char *end = NULL;
            const unsigned long id = strtoul(e->d_name, &end, 16);
            if (strlen(e->d_name) == 8 && *end == 0 && id == strtoul(users[i], NULL, 10)) {
                snprintf(path, sizeof(path), "%s/%s/username.dat", home, e->d_name);
                file_read(path, &name);
                break;
            }
        }
        if (dir)
            closedir(dir);
        if (name.len > 0) {
            const size_t len = name.len < sizeof(p->name) - 1 ? name.len : sizeof(p->name) - 1;
            memcpy(p->name, name.data, len);
            p->name[len] = 0;
            p->name[strcspn(p->name, "\r\n")] = 0;
            for (size_t k = strlen(p->name); k > 0 && isspace((unsigned char)p->name[k - 1]); k--)
                p->name[k - 1] = 0;
            for (char *c = p->name; *c; c++)
                if (*c == '\t')
                    *c = ' ';
            if (p->name[0] == 0)
                copy_str(p->name, sizeof(p->name), users[i]);
        }
        buf_free(&name);
    }
    return n;
}

/* ---- both databases ---- */

bool tiles_conflict(sqlite3 *db)
{
    if (finds_row(db, "SELECT 1 FROM main.sqlite_master WHERE name GLOB 'lineup_*' LIMIT 1", NULL) == 1)
        return true;
    char real[512];
    sqlite3 *info = NULL;
    bool found = false;
    if (sqlite3_open_v2(cpath(APPINFO_DB, real, sizeof(real)), &info, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
        found = finds_row(info, "SELECT 1 FROM sqlite_master WHERE name GLOB 'lineup_*' LIMIT 1", NULL) == 1;
    sqlite3_close(info);
    return found;
}

bool tiles_attach(sqlite3 *db, char *err, size_t errlen)
{
    if (finds_row(db, "SELECT 1 FROM pragma_database_list WHERE name = 'info'", NULL) == 1)
        return true;
    char real[512], check[128];
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(db, "ATTACH ?1 AS info", -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, cpath(APPINFO_DB, real, sizeof(real)), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    if (!ok) {
        snprintf(err, errlen, "The home screen's appinfo.db did not open (%s)", sqlite3_errmsg(db));
        return false;
    }
    if (pragma_text(db, "PRAGMA info.quick_check", check, sizeof(check)) != 0) {
        snprintf(err, errlen, "The home screen's appinfo.db did not pass its check (%s), so nothing was changed", check);
        return false;
    }
    return true;
}

bool tiles_backup(sqlite3 *db, char *err, size_t errlen)
{
    char check[128];
    if (pragma_text(db, "PRAGMA main.quick_check", check, sizeof(check)) != 0) {
        snprintf(err, errlen, "The home screen's database did not pass its check (%s), so nothing was changed", check);
        return false;
    }
    return appdb_backup(db, err, errlen) && tiles_attach(db, err, errlen) && appdb_backup_info(db, err, errlen);
}

/* ---- rows and pins ----
 *
 * A row is a tab's tiles on a profile, in tbl_iconinfo_<user>, by lastAccessIndex, largest first. Opening an app
 * raises its index, which is how the one played last comes first. Ordering gives the row's tiles the indices the
 * row already has, in a new order, so no index grows past the counter the home screen keeps in memory.
 *
 * A pinned tile keeps its place when the system raises another: three triggers on tbl_iconinfo_<user>
 * (artswap_pin_u_, _i_ and _d_<user>) run REORDER inside the system's own write. artswap_pin holds the places,
 * artswap_seq is 0..TILES_MAX-1 (to find the k-th free place) and artswap_tmp is REORDER's scratch table, empty
 * between writes. The SQL is what the console's SQLite reads: no CTEs (not allowed in triggers anyway), window
 * functions, UPSERT or RETURNING. */

/* The tab a row of tbl_iconinfo_<user> is on, for a row: 138 or 188 as an app's are kept, and the system's tiles
 * ordered with the apps by the tab bit of their own value; NULL off both rows, or for a hub. p is NEW., OLD. or "". */
#define TAB_OF(p)                                                                                                     \
    "(CASE WHEN " p "dispLocation IN (138, 188) THEN " p "dispLocation WHEN " p "titleId IN " ROW_SYSTEM            \
    " AND (" p "dispLocation & 1) = 0 THEN CASE WHEN " p "dispLocation & 4 THEN 188 WHEN " p                         \
    "dispLocation & 2 THEN 138 END END)"

#define PINNED "SELECT titleId FROM artswap_pin WHERE user = '%1$s'"

/* %1$s is the user and `row` NEW. or OLD., the tile that fired it. In artswap_tmp, v is a tile's index, r the place
 * it gets and nv that place's index (before that, for a tile not pinned, how many of those were used after it). */
#define REORDER(row)                                                                                                  \
    /* the row as it is now */                                                                                        \
    "INSERT OR IGNORE INTO artswap_tmp (titleId, v) SELECT titleId, lastAccessIndex FROM tbl_iconinfo_%1$s "        \
    "WHERE visible = 1 AND " TAB_OF("") " = " TAB_OF(row) ";"                                                       \
    /* a pinned tile keeps its place; pinned past the end of a shorter row, they pack at its end */                   \
    "UPDATE artswap_tmp SET r = (SELECT MIN(p.slot, (SELECT COUNT(*) FROM artswap_tmp) - 1 - (SELECT COUNT(*) "     \
    "FROM artswap_pin q, artswap_tmp t WHERE q.user = p.user AND t.titleId = q.titleId AND q.slot > p.slot)) "       \
    "FROM artswap_pin p WHERE p.user = '%1$s' AND p.titleId = artswap_tmp.titleId) WHERE titleId IN (" PINNED ");"  \
    /* the others take the free places by how recently they were used, as the system orders them */                  \
    "UPDATE artswap_tmp SET nv = (SELECT COUNT(*) FROM artswap_tmp o WHERE o.titleId NOT IN (" PINNED ") AND "      \
    "(o.v > artswap_tmp.v OR (o.v = artswap_tmp.v AND o.titleId < artswap_tmp.titleId))) WHERE titleId NOT IN ("    \
    PINNED ");"                                                                                                       \
    "UPDATE artswap_tmp SET r = (SELECT MIN(s.i) FROM artswap_seq s WHERE s.i < (SELECT COUNT(*) FROM artswap_tmp) " \
    "AND s.i NOT IN (SELECT t.r FROM artswap_tmp t WHERE t.titleId IN (" PINNED ")) AND s.i - (SELECT COUNT(*) "    \
    "FROM artswap_tmp t WHERE t.titleId IN (" PINNED ") AND t.r < s.i) = artswap_tmp.nv) WHERE titleId NOT IN ("    \
    PINNED ");"                                                                                                       \
    /* place r gets the r-th largest index the row already has */                                                     \
    "UPDATE artswap_tmp SET nv = (SELECT a.v FROM artswap_tmp a WHERE (SELECT COUNT(*) FROM artswap_tmp b "          \
    "WHERE b.v > a.v OR (b.v = a.v AND b.titleId < a.titleId)) = artswap_tmp.r);"                                    \
    "UPDATE tbl_iconinfo_%1$s SET lastAccessIndex = (SELECT t.nv FROM artswap_tmp t WHERE t.titleId = "             \
    "tbl_iconinfo_%1$s.titleId) WHERE visible = 1 AND " TAB_OF("") " = " TAB_OF(row) " AND titleId IN "             \
    "(SELECT titleId FROM artswap_tmp WHERE nv <> v);"                                                                \
    "DELETE FROM artswap_tmp;"

/* Opening an app raises its index (twice, a few seconds apart); installing inserts and uninstalling deletes a row.
 * recursive_triggers is off by default, so REORDER's own UPDATE does not fire them again; a non-empty artswap_tmp
 * stops them too, should it be on. */
#define PIN_TRIGGER(kind, event, when, row)                                                                           \
    "CREATE TRIGGER IF NOT EXISTS artswap_pin_" kind "_%1$s AFTER " event " ON tbl_iconinfo_%1$s WHEN " when row    \
    "visible = 1 AND " TAB_OF(row) " IS NOT NULL AND NOT EXISTS (SELECT 1 FROM artswap_tmp) BEGIN " REORDER(row)     \
    " END;"
#define PIN_TRIGGERS                                                                                                  \
    PIN_TRIGGER("u", "UPDATE OF lastAccessIndex", "NEW.lastAccessIndex > OLD.lastAccessIndex AND ", "NEW.")         \
    PIN_TRIGGER("i", "INSERT", "", "NEW.") PIN_TRIGGER("d", "DELETE", "", "OLD.")

static const char *const ICON_PIN_COLUMNS[] = {"titleId", "lastAccessIndex", "visible", "dispLocation", NULL};

typedef struct {
    char id[16];
    long long index;
} row_t;

/* The tiles of `user`'s row on `tab`, the most recent first; their count, or -1 */
static int read_row(sqlite3 *db, const char *user, int tab, row_t *rows, int max)
{
    char sql[640];
    snprintf(sql, sizeof(sql), "SELECT titleId, lastAccessIndex FROM tbl_iconinfo_%s WHERE visible = 1 AND " TAB_OF("")
                               " = %d ORDER BY lastAccessIndex DESC, titleId",
             user, tab);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        copy_str(rows[n].id, sizeof(rows[n].id), (const char *)sqlite3_column_text(st, 0));
        rows[n++].index = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    return n;
}

static bool drop_pin_triggers(sqlite3 *db, const char *user)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS artswap_pin_u_%1$s; DROP TRIGGER IF EXISTS artswap_pin_i_%1$s;"
             "DROP TRIGGER IF EXISTS artswap_pin_d_%1$s;",
             user);
    return exec(db, sql);
}

static bool create_pin_triggers(sqlite3 *db, const char *user)
{
    static char sql[16 * 1024];
    return snprintf(sql, sizeof(sql), PIN_TRIGGERS, user) < (int)sizeof(sql) && exec(db, sql);
}

/* Every profile's pin triggers and the three tables */
static bool remove_pins(sqlite3 *db)
{
    static char names[TILES_MAX][64];
    int n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT name FROM main.sqlite_master WHERE type = 'trigger' AND name GLOB "
                               "'artswap_pin_[uid]_*'",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    while (n < TILES_MAX && sqlite3_step(st) == SQLITE_ROW)
        copy_str(names[n++], sizeof(names[0]), (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    for (int i = 0; i < n; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "DROP TRIGGER \"%s\"", names[i]);
        if (!exec(db, sql))
            return false;
    }
    return exec(db, "DROP TABLE IF EXISTS artswap_pin; DROP TABLE IF EXISTS artswap_seq; DROP TABLE IF EXISTS "
                    "artswap_tmp;");
}

/* 1 when `user` has a pinned tile on either tab, 0 when not, -1 on failure */
static int has_pins(sqlite3 *db, const char *user)
{
    if (!has_object(db, "artswap_pin"))
        return 0;
    return finds_row(db, "SELECT 1 FROM artswap_pin WHERE user = ?1 LIMIT 1", user);
}

/* Once no profile has a pinned tile, none of the pins' tables stays */
static bool remove_pins_if_unused(sqlite3 *db)
{
    if (has_object(db, "artswap_pin")) {
        const int used = finds_row(db, "SELECT 1 FROM artswap_pin LIMIT 1", NULL);
        if (used != 0)
            return used == 1;
    }
    return remove_pins(db);
}

/* Takes `id` out of `user`'s pins; the triggers go with the profile's last one */
static bool forget_pin(sqlite3 *db, const char *user, const char *id)
{
    if (!has_object(db, "artswap_pin"))
        return true;
    if (run_title(db, "DELETE FROM artswap_pin WHERE titleId = ?1 AND user = ?2", user, id) < 0)
        return false;
    const int kept = has_pins(db, user);
    if (kept != 0)
        return kept == 1;
    return drop_pin_triggers(db, user) && remove_pins_if_unused(db);
}

static bool forget_pin_everywhere(sqlite3 *db, const char *id)
{
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    bool ok = count >= 0;
    for (int i = 0; ok && i < count; i++)
        ok = forget_pin(db, users[i], id);
    return ok;
}

/* The place `id` is pinned at on `user`'s row, or -1 */
static int pin_of(sqlite3 *db, const char *user, const char *id)
{
    if (!has_object(db, "artswap_pin"))
        return -1;
    sqlite3_stmt *st = NULL;
    int slot = -1;
    if (sqlite3_prepare_v2(db, "SELECT slot FROM artswap_pin WHERE user = ?1 AND titleId = ?2", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, user, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            slot = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return slot;
}

/* After the profile's pin triggers were dropped: the pinned tiles of `user`'s row on `tab`, at their places in
 * `order`, and the triggers again while the profile has pins on either tab */
static bool save_pins(sqlite3 *db, const char *user, int tab, const row_t *order, const bool *pinned, int n)
{
    int pins = 0;
    for (int i = 0; i < n; i++)
        pins += pinned[i];
    static char sql[1024];
    /* the other tab's pins stay; the rest are this tab's, or of tiles no longer on a row */
    if (has_object(db, "artswap_pin")) {
        snprintf(sql, sizeof(sql),
                 "DELETE FROM artswap_pin WHERE user = '%1$s' AND titleId NOT IN (SELECT titleId FROM tbl_iconinfo_%1$s "
                 "WHERE visible = 1 AND " TAB_OF("") " = %2$d)",
                 user, tab == GAMES_TAB ? MEDIA_TAB : GAMES_TAB);
        if (!exec(db, sql))
            return false;
    }
    if (pins == 0) {
        const int kept = has_pins(db, user);
        if (kept < 0)
            return false;
        return kept ? create_pin_triggers(db, user) : remove_pins_if_unused(db);
    }
    if (!exec(db, "CREATE TABLE IF NOT EXISTS artswap_pin (user TEXT, titleId TEXT, slot INTEGER, PRIMARY KEY (user, "
                  "titleId));"
                  "CREATE TABLE IF NOT EXISTS artswap_seq (i INTEGER PRIMARY KEY);"
                  "CREATE TABLE IF NOT EXISTS artswap_tmp (titleId TEXT PRIMARY KEY, v INTEGER, r INTEGER, nv "
                  "INTEGER);"))
        return false;
    bool ok = true;
    for (int i = 0; ok && i < TILES_MAX; i++)
        ok = run_values(db, "INSERT OR IGNORE INTO artswap_seq VALUES (?1)", i, NULL, NULL) >= 0;
    for (int i = 0; ok && i < n; i++)
        if (pinned[i])
            ok = run_values(db, "INSERT INTO artswap_pin VALUES (?2, ?3, ?1)", i, user, order[i].id) >= 0;
    return ok && create_pin_triggers(db, user);
}

int tiles_order(sqlite3 *db, const char *user, int tab, char ids[][16], const bool *pins, int n, char *msg,
                size_t msglen)
{
    char table[64];
    snprintf(table, sizeof(table), "tbl_iconinfo_%s", user);
    if (!tiles_user_known(db, user) || !has_columns(db, "main", table, ICON_PIN_COLUMNS)) {
        snprintf(msg, msglen, "no such profile, or its row is kept in a way Art Swap does not know");
        return 1;
    }
    static row_t now[TILES_MAX], order[TILES_MAX];
    static bool pinned[TILES_MAX], taken[TILES_MAX];
    static long long indices[TILES_MAX];
    const int count = read_row(db, user, tab, now, TILES_MAX);
    if (count < 0) {
        snprintf(msg, msglen, "the row could not be read (%s)", sqlite3_errmsg(db));
        return -1;
    }
    /* the tiles named that are on the row, in the order wished; then the others, the most recent first */
    int k = 0;
    memset(taken, 0, sizeof(bool) * (size_t)count);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < count; j++)
            if (!taken[j] && strcmp(now[j].id, ids[i]) == 0) {
                taken[j] = true;
                pinned[k] = pins[i];
                order[k++] = now[j];
            }
    for (int j = 0; j < count; j++)
        if (!taken[j]) {
            pinned[k] = false;
            order[k++] = now[j];
        }
    /* with every place on the home screen pinned, the app opened last would have none there: one stays free */
    const int home = count < (tab == MEDIA_TAB ? MEDIA_HOME : GAMES_HOME) ? count : (tab == MEDIA_TAB ? MEDIA_HOME
                                                                                                     : GAMES_HOME);
    int free_places = 0;
    for (int i = 0; i < home; i++)
        free_places += !pinned[i];
    if (home > 0 && free_places == 0) {
        log_line("tiles: every place of %s's row was pinned: the first is left free", user);
        pinned[0] = false;
    }
    for (int i = 0; i < count; i++)
        indices[i] = now[i].index; /* already largest first */
    char sql[160];
    snprintf(sql, sizeof(sql), "UPDATE %s SET lastAccessIndex = ?1 WHERE titleId = ?2", table);
    /* the pin triggers go first, or these writes would fire them */
    bool ok = drop_pin_triggers(db, user);
    for (int i = 0; ok && i < count; i++)
        if (order[i].index != indices[i])
            ok = run_values(db, sql, indices[i], order[i].id, NULL) == 1;
    for (int i = 0; i < count; i++)
        order[i].index = indices[i];
    ok = ok && save_pins(db, user, tab, order, pinned, count);
    if (!ok) {
        snprintf(msg, msglen, "the row could not be written (%s)", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

/* The pin triggers of every profile with pins, made again: a database rebuilt without them, or one of 1.4's */
static int ensure_pin_triggers(sqlite3 *db, bool create)
{
    if (!has_object(db, "artswap_pin"))
        return 0;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0)
        return -1;
    int missing = 0;
    for (int i = 0; i < count; i++) {
        if (has_pins(db, users[i]) != 1)
            continue;
        bool all = true;
        for (const char *kind = "uid"; *kind; kind++) {
            char name[64];
            snprintf(name, sizeof(name), "artswap_pin_%c_%s", *kind, users[i]);
            all = all && has_object(db, name);
        }
        if (all)
            continue;
        missing++;
        if (create && !(drop_pin_triggers(db, users[i]) && create_pin_triggers(db, users[i])))
            return -1;
    }
    return missing;
}

/* ---- hidden on a profile ----
 *
 * visible = 0 takes a tile off the home screen, on that profile only. artswap_hidden lists what Art Swap hid, so
 * bringing back can only bring back those, never the system's own hidden entries. The console puts visible back
 * to 1 at a restart, so a profile with hidden tiles has two triggers that hide them again, whether the system
 * updates the row or writes it anew. They read artswap_hidden: a write to the profile's table would fail if the
 * table went before them, so it goes with its last row, after them. */

#define HIDE_TRIGGER(kind, event)                                                                                     \
    "CREATE TRIGGER IF NOT EXISTS artswap_hide_" kind "_%1$s AFTER " event " ON tbl_iconinfo_%1$s WHEN NEW.visible " \
    "= 1 AND NEW.titleId IN (SELECT titleId FROM artswap_hidden WHERE user = '%1$s') BEGIN UPDATE tbl_iconinfo_%1$s " \
    "SET visible = 0 WHERE titleId = NEW.titleId; END;"
#define HIDE_TRIGGERS HIDE_TRIGGER("u", "UPDATE OF visible") HIDE_TRIGGER("i", "INSERT")

static bool create_hide_triggers(sqlite3 *db, const char *user)
{
    char sql[1024];
    return snprintf(sql, sizeof(sql), HIDE_TRIGGERS, user) < (int)sizeof(sql) && exec(db, sql);
}

static bool drop_hide_triggers(sqlite3 *db, const char *user)
{
    char sql[160];
    snprintf(sql, sizeof(sql),
             "DROP TRIGGER IF EXISTS artswap_hide_u_%1$s; DROP TRIGGER IF EXISTS artswap_hide_i_%1$s;", user);
    return exec(db, sql);
}

static bool hidden_by_us(sqlite3 *db, const char *user, const char *id)
{
    if (!has_object(db, "artswap_hidden"))
        return false;
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM artswap_hidden WHERE user = ?1 AND titleId = ?2", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, user, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return found;
}

/* A system tile Art Swap shows (artswap_system.shown) */
static bool shown_by_us(sqlite3 *db, const char *id)
{
    return has_object(db, "artswap_system") &&
           finds_row(db, "SELECT 1 FROM artswap_system WHERE titleId = ?1 AND shown = 1", id) == 1;
}

int tiles_hide(sqlite3 *db, const char *user, const char *id, bool hide, char *msg, size_t msglen)
{
    char table[64], sql[256];
    snprintf(table, sizeof(table), "tbl_iconinfo_%s", user);
    if (!tiles_user_known(db, user) || !has_column(db, "main", table, "visible")) {
        snprintf(msg, msglen, "no such profile");
        return 1;
    }
    if (library(id) || strcmp(id, WELCOME_ID) == 0 || strcmp(id, ARTSWAP_ID) == 0 || shown_by_us(db, id)) {
        snprintf(msg, msglen, "%s does not hide on a profile", id);
        return 1;
    }
    if (hide) {
        snprintf(sql, sizeof(sql), "UPDATE %s SET visible = 0 WHERE titleId = ?1 AND visible = 1", table);
        const int changed = run_title(db, sql, NULL, id);
        if (changed < 0)
            return snprintf(msg, msglen, "it could not be hidden (%s)", sqlite3_errmsg(db)), -1;
        if (changed == 0)
            return snprintf(msg, msglen, "%s is not showing", id), 1;
        if (!exec(db, "CREATE TABLE IF NOT EXISTS artswap_hidden (user TEXT, titleId TEXT, PRIMARY KEY (user, "
                      "titleId))") ||
            run_title(db, "INSERT OR IGNORE INTO artswap_hidden VALUES (?2, ?1)", user, id) < 0 ||
            !create_hide_triggers(db, user) || !forget_pin(db, user, id))
            return snprintf(msg, msglen, "it could not be hidden (%s)", sqlite3_errmsg(db)), -1;
        return 0;
    }
    if (!hidden_by_us(db, user, id))
        return snprintf(msg, msglen, "Art Swap did not hide %s", id), 1;
    /* the row goes first: while it is there, the triggers hide the tile again */
    snprintf(sql, sizeof(sql), "UPDATE %s SET visible = 1 WHERE titleId = ?1 AND visible = 0", table);
    if (run_title(db, "DELETE FROM artswap_hidden WHERE titleId = ?1 AND user = ?2", user, id) < 0 ||
        run_title(db, sql, NULL, id) < 0)
        return snprintf(msg, msglen, "it could not be brought back (%s)", sqlite3_errmsg(db)), -1;
    const int others = finds_row(db, "SELECT 1 FROM artswap_hidden WHERE user = ?1 LIMIT 1", user);
    const int any = finds_row(db, "SELECT 1 FROM artswap_hidden LIMIT 1", NULL);
    if (others < 0 || any < 0 || (!others && !drop_hide_triggers(db, user)) ||
        (!any && !exec(db, "DROP TABLE artswap_hidden")))
        return snprintf(msg, msglen, "it could not be brought back (%s)", sqlite3_errmsg(db)), -1;
    return 0;
}

/* Everything Art Swap hid, on every profile, brought back, and artswap_hidden dropped */
static bool show_all_hidden(sqlite3 *db)
{
    if (!has_object(db, "artswap_hidden"))
        return true;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0)
        return false;
    for (int i = 0; i < count; i++) {
        char sql[300];
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE visible = 0 AND titleId IN (SELECT titleId FROM "
                 "artswap_hidden WHERE user = '%1$s')",
                 users[i]);
        if (!drop_hide_triggers(db, users[i]) || !exec(db, sql))
            return false;
    }
    return exec(db, "DROP TABLE artswap_hidden");
}

/* The profiles whose hidden tiles came back or lost their triggers: hidden again (create), or counted */
static int ensure_hidden(sqlite3 *db, bool create)
{
    if (!has_object(db, "artswap_hidden"))
        return 0;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0)
        return -1;
    int stale = 0;
    for (int i = 0; i < count; i++) {
        char sql[300], name[64];
        if (finds_row(db, "SELECT 1 FROM artswap_hidden WHERE user = ?1 LIMIT 1", users[i]) != 1)
            continue;
        snprintf(sql, sizeof(sql),
                 "SELECT 1 FROM tbl_iconinfo_%1$s WHERE visible = 1 AND titleId IN (SELECT titleId FROM artswap_hidden "
                 "WHERE user = '%1$s') LIMIT 1",
                 users[i]);
        const int back = finds_row(db, sql, NULL);
        snprintf(name, sizeof(name), "artswap_hide_u_%s", users[i]);
        bool missing = !has_object(db, name);
        snprintf(name, sizeof(name), "artswap_hide_i_%s", users[i]);
        missing = missing || !has_object(db, name);
        if (back < 0)
            return -1;
        if (back == 0 && !missing)
            continue;
        stale++;
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_iconinfo_%1$s SET visible = 0 WHERE visible = 1 AND titleId IN (SELECT titleId FROM "
                 "artswap_hidden WHERE user = '%1$s')",
                 users[i]);
        if (create && !(exec(db, sql) && create_hide_triggers(db, users[i])))
            return -1;
    }
    return stale;
}

/* ---- an app moved between the tabs ----
 *
 * What puts an app on a tab is written in several places, and they must agree, or it ends up half game, half
 * media app: dispLocation and hubAppUri on every profile's tbl_iconinfo_<user>; dispLocation, categoryType,
 * is3rdPartyVideoApp, pprHubAppUri and the same values in AppInfoJson on tbl_contentinfo; DISPLAYLOCATION,
 * CATEGORY_TYPE and HUBAPP_URI on appinfo.db's tbl_appinfo; and for an app whose concept is its own, its concept:
 * tbl_concepticoninfo_<user> (1024 above the tab on Games, 2048 on Media: 1162 and 2236), tbl_conceptmetadata (and
 * display_location in ConceptInfoJson) and appinfo.db's tbl_conceptinfo. They get what a native app of the tab
 * holds. Bit 65536 of the category is the Media one, so the rest of it survives a move there and back.
 *
 * Installing an app again writes it back on the tab its param.json names, and some homebrew does that each time it
 * starts (Game Compressor's payload; Art Swap itself, opened). So artswap_moved, in both databases, keeps each
 * moved app's tab, and triggers put it back whenever the system writes it anew or changes its tab. They leave the
 * JSON alone, since the console's SQLite may lack the JSON functions: tiles_startup puts it right. An app moved back
 * to the tab it started on (`home`) leaves artswap_moved, and the last one takes the tables and triggers with it. */

#define MOVED_TAB "(SELECT tab FROM artswap_moved WHERE titleId = NEW.titleId)"
#define HUB_OF(tab)                                                                                                   \
    "(CASE " tab " WHEN 188 THEN 'psmediahub:main?titleId=' ELSE 'pshome:gamehub?titleId=' END || NEW.titleId)"
/* Only a row on a tab and off the one it was moved to: the triggers' own writes do not fire them again */
#define MOVE_WHEN                                                                                                     \
    "WHEN NEW.dispLocation IN (138, 188) AND EXISTS (SELECT 1 FROM artswap_moved WHERE titleId = NEW.titleId AND "  \
    "tab <> NEW.dispLocation) BEGIN "
#define CONTENT_TRIGGER(kind, event)                                                                                  \
    "CREATE TRIGGER IF NOT EXISTS artswap_move_c" kind " AFTER " event " ON tbl_contentinfo " MOVE_WHEN              \
    "UPDATE tbl_contentinfo SET dispLocation = " MOVED_TAB ", categoryType = CASE " MOVED_TAB " WHEN 188 THEN "       \
    "categoryType | 65536 ELSE categoryType & ~65536 END, is3rdPartyVideoApp = " MOVED_TAB " = 188, pprHubAppUri = " \
    HUB_OF(MOVED_TAB) " WHERE titleId = NEW.titleId; END;"
#define ICON_TRIGGER(kind, event)                                                                                     \
    "CREATE TRIGGER IF NOT EXISTS artswap_move_" kind "_%1$s AFTER " event " ON tbl_iconinfo_%1$s " MOVE_WHEN       \
    "UPDATE tbl_iconinfo_%1$s SET dispLocation = " MOVED_TAB ", hubAppUri = " HUB_OF(MOVED_TAB) " WHERE titleId = " \
    "NEW.titleId; END;"
/* tbl_appinfo holds one row a key; a row that already has its value fires nothing */
#define INFO_VALUE                                                                                                    \
    "CASE NEW.key WHEN 'DISPLAYLOCATION' THEN " MOVED_TAB " WHEN 'CATEGORY_TYPE' THEN CASE " MOVED_TAB              \
    " WHEN 188 THEN NEW.val | 65536 ELSE NEW.val & ~65536 END ELSE " HUB_OF(MOVED_TAB) " END"
#define INFO_TRIGGER(kind, event)                                                                                     \
    "CREATE TRIGGER IF NOT EXISTS info.artswap_move_a" kind " AFTER " event " ON tbl_appinfo WHEN NEW.key IN "       \
    "('DISPLAYLOCATION', 'CATEGORY_TYPE', 'HUBAPP_URI') AND EXISTS (SELECT 1 FROM artswap_moved WHERE titleId = "    \
    "NEW.titleId) AND NEW.val IS NOT (" INFO_VALUE ") BEGIN UPDATE tbl_appinfo SET val = " INFO_VALUE " WHERE "     \
    "titleId = NEW.titleId AND key = NEW.key; END;"

/* The concept's places: artswap_moved_concept gives their triggers (artswap_move_k*) the title and the tab */
#define CONCEPT_OF(tab) "(" tab " + CASE " tab " WHEN 188 THEN 2048 ELSE 1024 END)"
#define K_TAB(key) "(SELECT tab FROM artswap_moved_concept WHERE concept = NEW." key ")"
#define K_HUB(key)                                                                                                    \
    "(CASE " K_TAB(key) " WHEN 188 THEN 'psmediahub:main?titleId=' ELSE 'pshome:gamehub?titleId=' END || "          \
    "(SELECT titleId FROM artswap_moved_concept WHERE concept = NEW." key "))"
#define K_ICON_TRIGGER(kind, event)                                                                                   \
    "CREATE TRIGGER IF NOT EXISTS artswap_move_k" kind "_%1$s AFTER " event " ON tbl_concepticoninfo_%1$s WHEN "    \
    "NEW.dispLocation IN (1162, 2236) AND EXISTS (SELECT 1 FROM artswap_moved_concept WHERE concept = "              \
    "NEW.localConceptId AND " CONCEPT_OF("tab") " <> NEW.dispLocation) BEGIN UPDATE tbl_concepticoninfo_%1$s SET "  \
    "dispLocation = " CONCEPT_OF(K_TAB("localConceptId")) ", hubAppUri = " K_HUB("localConceptId") " WHERE "        \
    "localConceptId = NEW.localConceptId; END;"
#define K_META_TRIGGER(kind, event)                                                                                   \
    "CREATE TRIGGER IF NOT EXISTS artswap_move_km" kind " AFTER " event " ON tbl_conceptmetadata WHEN "              \
    "NEW.dispLocation IN (138, 188) AND EXISTS (SELECT 1 FROM artswap_moved_concept WHERE concept = "                \
    "NEW.localConceptId AND tab <> NEW.dispLocation) BEGIN UPDATE tbl_conceptmetadata SET dispLocation = "           \
    K_TAB("localConceptId") ", pprHubAppUri = " K_HUB("localConceptId") " WHERE localConceptId = "                   \
    "NEW.localConceptId; END;"
#define K_INFO_VALUE                                                                                                  \
    "CASE NEW.key WHEN 'display_location' THEN " K_TAB("local_concept_id") " ELSE " K_HUB("local_concept_id") " END"
#define K_INFO_TRIGGER(kind, event)                                                                                   \
    "CREATE TRIGGER IF NOT EXISTS info.artswap_move_ka" kind " AFTER " event " ON tbl_conceptinfo WHEN NEW.key IN "  \
    "('display_location', 'hubapp_uri') AND EXISTS (SELECT 1 FROM artswap_moved_concept WHERE concept = "             \
    "NEW.local_concept_id) AND NEW.val IS NOT (" K_INFO_VALUE ") BEGIN UPDATE tbl_conceptinfo SET val = "            \
    K_INFO_VALUE " WHERE local_concept_id = NEW.local_concept_id AND key = NEW.key; END;"

#define MOVED_TABLES                                                                                                  \
    "CREATE TABLE IF NOT EXISTS main.artswap_moved (titleId TEXT PRIMARY KEY, tab INTEGER, home INTEGER);"           \
    "CREATE TABLE IF NOT EXISTS info.artswap_moved (titleId TEXT PRIMARY KEY, tab INTEGER);"                         \
    "CREATE TABLE IF NOT EXISTS main.artswap_moved_concept (concept TEXT PRIMARY KEY, titleId TEXT, tab INTEGER);"   \
    "CREATE TABLE IF NOT EXISTS info.artswap_moved_concept (concept TEXT PRIMARY KEY, titleId TEXT, tab INTEGER);"

static const char *const CONTENT_MOVE_COLUMNS[] = {"titleId", "dispLocation", "categoryType", "is3rdPartyVideoApp",
                                                   "pprHubAppUri", NULL};
static const char *const ICON_MOVE_COLUMNS[] = {"titleId", "dispLocation", "hubAppUri", NULL};
static const char *const INFO_COLUMNS[] = {"titleId", "key", "val", NULL};
static const char *const CONCEPT_ICON_COLUMNS[] = {"localConceptId", "dispLocation", "hubAppUri", NULL};
static const char *const CONCEPT_META_COLUMNS[] = {"localConceptId", "dispLocation", "pprHubAppUri", NULL};
static const char *const CONCEPT_INFO_COLUMNS[] = {"local_concept_id", "key", "val", NULL};

/* Only homebrew and emulators installed under /user/app change tabs, Art Swap too: the system's apps, games and
 * PS4 titles stay out */
static bool can_move_app(const char *id, int type, int place)
{
    return (type == 0 || type == 5) && place == 2 && strncmp(id, "CUSA", 4) != 0 && !is_system(id);
}

static bool app_movable(sqlite3 *db, const char *id, int *tab)
{
    if (!has_column(db, "main", "tbl_contentinfo", "appType") ||
        !has_column(db, "main", "tbl_contentinfo", "contentLocation"))
        return false;
    sqlite3_stmt *st = NULL;
    bool movable = false;
    if (sqlite3_prepare_v2(db, "SELECT appType, contentLocation, dispLocation FROM tbl_contentinfo WHERE titleId = ?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            movable = can_move_app(id, sqlite3_column_int(st, 0), sqlite3_column_int(st, 1));
            if (tab)
                *tab = sqlite3_column_int(st, 2);
        }
    }
    sqlite3_finalize(st);
    return movable;
}

/* `id`'s concept into `out` when no other title has it: 1, 0 when it has none or shares it, -1 on failure */
static int own_concept(sqlite3 *db, const char *id, char *out, size_t size)
{
    if (!has_column(db, "main", "tbl_contentinfo", "localConceptId"))
        return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT localConceptId FROM tbl_contentinfo c WHERE titleId = ?1 AND "
                               "ifnull(localConceptId, '') <> '' AND NOT EXISTS (SELECT 1 FROM tbl_contentinfo o WHERE "
                               "o.localConceptId = c.localConceptId AND o.titleId <> ?1)",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        copy_str(out, size, (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/* ConceptInfoJson keeps a value as the data of its field `key`: made `value` (or the row's dispLocation when
 * value is NULL). Only that value changes; a concept without the field is left as it is. */
static bool fix_concept_field(sqlite3 *db, const char *concept, const char *field, const char *value)
{
    if (!has_column(db, "main", "tbl_conceptmetadata", "ConceptInfoJson"))
        return true;
    char sql[1100];
    snprintf(sql, sizeof(sql),
             "UPDATE tbl_conceptmetadata SET ConceptInfoJson = json_set(ConceptInfoJson, '$.field_list[' || (SELECT "
             "e.key FROM json_each(" SAFE_JSON("ConceptInfoJson") ", '$.field_list') e WHERE e.value ->> '$.key' = "
             "'%1$s' LIMIT 1) || '].data', %2$s) WHERE localConceptId = ?2 AND json_valid(ConceptInfoJson) AND EXISTS "
             "(SELECT 1 FROM json_each(" SAFE_JSON("ConceptInfoJson") ", '$.field_list') e WHERE e.value ->> '$.key' = "
             "'%1$s' AND e.value ->> '$.data' IS NOT %2$s)",
             field, value ? value : "dispLocation");
    return run_values(db, sql, 0, concept, NULL) >= 0;
}

/* The concept triggers that are missing (create false), or made (create true: with artswap_moved_concept in both).
 * How many were missing, or -1. */
static int concept_triggers(sqlite3 *db, char users[][16], int count, bool create)
{
    int missing = 0;
    static char sql[4096];
    if (has_columns(db, "main", "tbl_conceptmetadata", CONCEPT_META_COLUMNS) &&
        (!has_object(db, "artswap_move_kmi") || !has_object(db, "artswap_move_kmu"))) {
        missing++;
        if (create && !exec(db, K_META_TRIGGER("i", "INSERT") K_META_TRIGGER("u", "UPDATE OF dispLocation")))
            return -1;
    }
    if (has_columns(db, "info", "tbl_conceptinfo", CONCEPT_INFO_COLUMNS) &&
        (!has_object_in(db, "info", "artswap_move_kai") || !has_object_in(db, "info", "artswap_move_kau"))) {
        missing++;
        if (create && !exec(db, K_INFO_TRIGGER("i", "INSERT") K_INFO_TRIGGER("u", "UPDATE OF val")))
            return -1;
    }
    for (int i = 0; i < count; i++) {
        char table[64], insert[64], update[64];
        snprintf(table, sizeof(table), "tbl_concepticoninfo_%s", users[i]);
        snprintf(insert, sizeof(insert), "artswap_move_ki_%s", users[i]);
        snprintf(update, sizeof(update), "artswap_move_ku_%s", users[i]);
        if (!has_columns(db, "main", table, CONCEPT_ICON_COLUMNS) ||
            (has_object(db, insert) && has_object(db, update)))
            continue;
        missing++;
        if (create &&
            (snprintf(sql, sizeof(sql), K_ICON_TRIGGER("i", "INSERT") K_ICON_TRIGGER("u", "UPDATE OF dispLocation"),
                      users[i]) >= (int)sizeof(sql) ||
             !exec(db, sql)))
            return -1;
    }
    return missing;
}

/* The triggers of every place the firmware has (create true), or how many are missing */
static int move_triggers(sqlite3 *db, char users[][16], int count, bool create)
{
    int missing = 0;
    static char sql[4096];
    if (has_columns(db, "main", "tbl_contentinfo", CONTENT_MOVE_COLUMNS) &&
        (!has_object(db, "artswap_move_ci") || !has_object(db, "artswap_move_cu"))) {
        missing++;
        if (create && !exec(db, CONTENT_TRIGGER("i", "INSERT") CONTENT_TRIGGER("u", "UPDATE OF dispLocation")))
            return -1;
    }
    if (has_columns(db, "info", "tbl_appinfo", INFO_COLUMNS) &&
        (!has_object_in(db, "info", "artswap_move_ai") || !has_object_in(db, "info", "artswap_move_au"))) {
        missing++;
        if (create && !exec(db, INFO_TRIGGER("i", "INSERT") INFO_TRIGGER("u", "UPDATE OF val")))
            return -1;
    }
    for (int i = 0; i < count; i++) {
        char table[64], insert[64], update[64];
        snprintf(table, sizeof(table), "tbl_iconinfo_%s", users[i]);
        snprintf(insert, sizeof(insert), "artswap_move_i_%s", users[i]);
        snprintf(update, sizeof(update), "artswap_move_u_%s", users[i]);
        if (!has_columns(db, "main", table, ICON_MOVE_COLUMNS) || (has_object(db, insert) && has_object(db, update)))
            continue;
        missing++;
        if (create && (snprintf(sql, sizeof(sql), ICON_TRIGGER("i", "INSERT") ICON_TRIGGER("u", "UPDATE OF dispLocation"),
                                users[i]) >= (int)sizeof(sql) ||
                       !exec(db, sql)))
            return -1;
    }
    const int concepts = concept_triggers(db, users, count, create);
    return concepts < 0 ? -1 : missing + concepts;
}

/* The triggers and tables of the moves, in both databases (the system tiles' have their own) */
static bool drop_moves(sqlite3 *db)
{
    static char names[4 * TILES_MAX][96];
    int n = 0;
    for (int schema = 0; schema < 2; schema++) {
        char sql[220];
        snprintf(sql, sizeof(sql),
                 "SELECT name FROM %s.sqlite_master WHERE type = 'trigger' AND name GLOB 'artswap_move_*' AND name NOT "
                 "GLOB 'artswap_move_sys_*'",
                 schema ? "info" : "main");
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
            return false;
        while (n < (int)(sizeof(names) / sizeof(names[0])) && sqlite3_step(st) == SQLITE_ROW)
            snprintf(names[n++], sizeof(names[0]), "%s.\"%s\"", schema ? "info" : "main",
                     (const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    for (int i = 0; i < n; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "DROP TRIGGER %s", names[i]);
        if (!exec(db, sql))
            return false;
    }
    return exec(db, "DROP TABLE IF EXISTS main.artswap_moved; DROP TABLE IF EXISTS info.artswap_moved;"
                    "DROP TABLE IF EXISTS main.artswap_moved_concept; DROP TABLE IF EXISTS info.artswap_moved_concept;");
}

/* Keeps `id`'s new tab in artswap_moved, or lets it go when it is back on the tab it started on */
static bool record_move(sqlite3 *db, const char *id, int from, int tab, char users[][16], int count)
{
    if (!exec(db, MOVED_TABLES))
        return false;
    sqlite3_stmt *st = NULL;
    int home = from;
    bool recorded = false;
    if (sqlite3_prepare_v2(db, "SELECT home FROM main.artswap_moved WHERE titleId = ?1", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        recorded = true;
        home = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    /* an app another tool moved is on the tab it was moved to, not its own: the Media bit of the category is the
     * install's, the better guess at where an install puts it back */
    if (!recorded && sqlite3_prepare_v2(db, "SELECT categoryType FROM tbl_contentinfo WHERE titleId = ?1", -1, &st,
                                        NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            home = sqlite3_column_int64(st, 0) & MEDIA_CATEGORY ? MEDIA_TAB : GAMES_TAB;
        sqlite3_finalize(st);
    }
    if (home == tab) {
        if (run_title(db, "DELETE FROM main.artswap_moved WHERE titleId = ?1", NULL, id) < 0 ||
            run_title(db, "DELETE FROM info.artswap_moved WHERE titleId = ?1", NULL, id) < 0 ||
            run_title(db, "DELETE FROM main.artswap_moved_concept WHERE titleId = ?1", NULL, id) < 0 ||
            run_title(db, "DELETE FROM info.artswap_moved_concept WHERE titleId = ?1", NULL, id) < 0)
            return false;
        const int left = finds_row(db, "SELECT 1 FROM main.artswap_moved LIMIT 1", NULL);
        return left < 0 ? false : left ? true : drop_moves(db);
    }
    char sql[160];
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO main.artswap_moved VALUES (?2, %d, %d)", tab, home);
    if (run_values(db, sql, 0, id, NULL) < 0)
        return false;
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO info.artswap_moved VALUES (?2, %d)", tab);
    if (run_values(db, sql, 0, id, NULL) < 0 || move_triggers(db, users, count, true) < 0)
        return false;
    char concept[96];
    const int own = own_concept(db, id, concept, sizeof(concept));
    if (own < 0)
        return false;
    if (own == 0)
        return true;
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO main.artswap_moved_concept VALUES (?2, ?3, %d)", tab);
    if (run_values(db, sql, 0, concept, id) < 0)
        return false;
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO info.artswap_moved_concept VALUES (?2, ?3, %d)", tab);
    return run_values(db, sql, 0, concept, id) >= 0;
}

/* Puts `id` on `tab` in every place the firmware has */
static bool write_move(sqlite3 *db, const char *id, int tab, char users[][16], int count)
{
    const bool media = tab == MEDIA_TAB;
    sqlite3_stmt *st = NULL;
    long long category = 0;
    if (sqlite3_prepare_v2(db, "SELECT categoryType FROM tbl_contentinfo WHERE titleId = ?1", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            category = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    category = media ? category | MEDIA_CATEGORY : category & ~(long long)MEDIA_CATEGORY;
    char hub[96], sql[400], number[32];
    snprintf(hub, sizeof(hub), media ? "psmediahub:main?titleId=%s" : "pshome:gamehub?titleId=%s", id);
    snprintf(sql, sizeof(sql),
             "UPDATE tbl_contentinfo SET dispLocation = %d, categoryType = ?1, is3rdPartyVideoApp = %d, pprHubAppUri = "
             "?3 WHERE titleId = ?2",
             tab, media);
    if (run_values(db, sql, category, id, hub) != 1)
        return false;
    if (has_column(db, "main", "tbl_contentinfo", "AppInfoJson")) {
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', %d, "
                 "'$.CATEGORY_TYPE', ?1, '$.HUBAPP_URI', ?3) WHERE titleId = ?2 AND json_valid(AppInfoJson)",
                 tab);
        if (run_values(db, sql, category, id, hub) < 0)
            return false;
    }
    if (has_columns(db, "info", "tbl_appinfo", INFO_COLUMNS)) {
        snprintf(number, sizeof(number), "%lld", category);
        snprintf(sql, sizeof(sql),
                 "UPDATE info.tbl_appinfo SET val = CASE key WHEN 'DISPLAYLOCATION' THEN %d WHEN 'CATEGORY_TYPE' THEN "
                 "?1 ELSE ?3 END WHERE titleId = ?2 AND key IN ('DISPLAYLOCATION', 'CATEGORY_TYPE', 'HUBAPP_URI')",
                 tab);
        if (run_values(db, sql, category, id, hub) < 0)
            return false;
    }
    for (int i = 0; i < count; i++) {
        char table[64];
        snprintf(table, sizeof(table), "tbl_iconinfo_%s", users[i]);
        if (!has_columns(db, "main", table, ICON_MOVE_COLUMNS))
            continue;
        snprintf(sql, sizeof(sql), "UPDATE %s SET dispLocation = %d, hubAppUri = ?3 WHERE titleId = ?2", table, tab);
        if (run_values(db, sql, 0, id, hub) < 0)
            return false;
    }
    /* its own concept */
    char concept[96];
    const int own = own_concept(db, id, concept, sizeof(concept));
    if (own <= 0)
        return own == 0;
    for (int i = 0; i < count; i++) {
        char table[64];
        snprintf(table, sizeof(table), "tbl_concepticoninfo_%s", users[i]);
        if (!has_columns(db, "main", table, CONCEPT_ICON_COLUMNS))
            continue;
        snprintf(sql, sizeof(sql), "UPDATE %s SET dispLocation = %d, hubAppUri = ?3 WHERE localConceptId = ?2", table,
                 tab + (media ? 2048 : 1024));
        if (run_values(db, sql, 0, concept, hub) < 0)
            return false;
    }
    if (has_columns(db, "main", "tbl_conceptmetadata", CONCEPT_META_COLUMNS)) {
        snprintf(sql, sizeof(sql),
                 "UPDATE tbl_conceptmetadata SET dispLocation = %d, pprHubAppUri = ?3 WHERE localConceptId = ?2", tab);
        if (run_values(db, sql, 0, concept, hub) < 0 || !fix_concept_field(db, concept, "display_location", NULL))
            return false;
    }
    if (has_columns(db, "info", "tbl_conceptinfo", CONCEPT_INFO_COLUMNS)) {
        snprintf(sql, sizeof(sql),
                 "UPDATE info.tbl_conceptinfo SET val = CASE key WHEN 'display_location' THEN %d ELSE ?3 END WHERE "
                 "local_concept_id = ?2 AND key IN ('display_location', 'hubapp_uri')",
                 tab);
        if (run_values(db, sql, 0, concept, hub) < 0)
            return false;
    }
    return true;
}

static int move_app(sqlite3 *db, const char *id, int tab, char *msg, size_t msglen)
{
    int from = 0;
    if (!app_movable(db, id, &from)) {
        snprintf(msg, msglen, "%s does not move: only homebrew and emulators do", id);
        return 1;
    }
    if (from == tab)
        return snprintf(msg, msglen, "%s is on that tab already", id), 1;
    if (from != GAMES_TAB && from != MEDIA_TAB)
        return snprintf(msg, msglen, "%s is on neither tab", id), 1;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0 || !record_move(db, id, from, tab, users, count) || !write_move(db, id, tab, users, count) ||
        !forget_pin_everywhere(db, id)) {
        snprintf(msg, msglen, "it could not be moved (%s)", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

/* ---- the system's own tiles ----
 *
 * They keep their tab as bits of dispLocation, 0x02 Games and 0x04 Media, and 0x01 for one the system hides (with
 * visible = 0): the PS Store is 146 on Games and 148 on Media, as App Library is; the Media Gallery 154 and 156;
 * Welcome and PS Plus, which the system hides, 147. tbl_concepticoninfo_<user> keeps the same plus 2048. A value
 * with neither tab bit (1: a tile the system keeps off both) is never changed.
 *
 * Moving sets one tab bit and clears the other, and showing clears the hidden bit and sets visible on every
 * profile, in every place: tbl_iconinfo_<user> and tbl_concepticoninfo_<user>, tbl_contentinfo (and DISPLAYLOCATION
 * in AppInfoJson), tbl_conceptmetadata (and display_location in ConceptInfoJson), and appinfo.db's tbl_appinfo
 * (DISPLAYLOCATION) and tbl_conceptinfo (display_location). A tile moved onto the Games row goes into each
 * profile's MyHome group before Game Library, as PSGFX found the system's tiles are, and leaves it off Games.
 *
 * artswap_system, in both databases, keeps for each tile the bits to set and to clear, what it had (home, its tab
 * bit, and whether the system hid it) and whether Art Swap shows it; artswap_move_sys_* triggers write the bits
 * again when the console writes a bit that should be clear, and visible when it hides one shown. A tile back as the
 * system had it leaves artswap_system, and the last one takes the tables and triggers. Game Library and App Library
 * never change. */

typedef struct {
    const char *schema, *table, *key, *by, *value, *field, *name;
} place_t;

static const place_t PLACES[] = {
    {"main", "tbl_contentinfo", "titleId", "titleId", "dispLocation", NULL, "c"},
    {"main", "tbl_conceptmetadata", "localConceptId", "concept", "dispLocation", NULL, "m"},
    {"info", "tbl_appinfo", "titleId", "titleId", "val", "DISPLAYLOCATION", "a"},
    {"info", "tbl_conceptinfo", "local_concept_id", "concept", "val", "display_location", "k"},
    {"main", "tbl_iconinfo_%s", "titleId", "titleId", "dispLocation", NULL, "icon_%s"},
    {"main", "tbl_concepticoninfo_%s", "localConceptId", "concept", "dispLocation", NULL, "kicon_%s"},
};
#define PLACES_SHARED 4 /* the first four are the console's; the others, each profile's */
#define PLACE_COUNT ((int)(sizeof(PLACES) / sizeof(PLACES[0])))
#define PLACE_ICON 4 /* tbl_iconinfo_<user>, which has visible too */

#define SYSTEM_TABLES                                                                                                 \
    "CREATE TABLE IF NOT EXISTS main.artswap_system (titleId TEXT PRIMARY KEY, concept TEXT, setbits INTEGER, "     \
    "clearbits INTEGER, home INTEGER, hid INTEGER, shown INTEGER);"                                                  \
    "CREATE TABLE IF NOT EXISTS info.artswap_system (titleId TEXT PRIMARY KEY, concept TEXT, setbits INTEGER, "     \
    "clearbits INTEGER);"

static bool place_ready(sqlite3 *db, const place_t *place, const char *user, char *table, size_t size)
{
    snprintf(table, size, place->table, user);
    return has_column(db, place->schema, table, place->key) && has_column(db, place->schema, table, place->value) &&
           (!place->field || has_column(db, place->schema, table, "key"));
}

static void field_condition(const place_t *place, const char *prefix, char *out, size_t size)
{
    if (place->field)
        snprintf(out, size, "%skey = '%s' AND ", prefix, place->field);
    else
        *out = 0;
}

/* The triggers missing on the places the firmware has (create false), or made (create true, with artswap_system in
 * both). How many were missing, or -1. */
static int system_triggers(sqlite3 *db, char users[][16], int count, bool create)
{
    int missing = 0;
    static char sql[2048];
    for (int p = 0; p < PLACE_COUNT; p++)
        for (int u = 0; u < (p < PLACES_SHARED ? 1 : count); u++) {
            const place_t *place = &PLACES[p];
            const char *user = p < PLACES_SHARED ? "" : users[u];
            char table[64], name[64], insert[96], update[96];
            if (!place_ready(db, place, user, table, sizeof(table)))
                continue;
            snprintf(name, sizeof(name), place->name, user);
            /* a tile shown: the system sets visible back to 0 at a restart */
            if (p == PLACE_ICON && has_column(db, "main", table, "visible")) {
                snprintf(insert, sizeof(insert), "artswap_move_sys_vi_%s", user);
                snprintf(update, sizeof(update), "artswap_move_sys_vu_%s", user);
                if (!has_object(db, insert) || !has_object(db, update)) {
                    missing++;
                    for (int kind = 0; create && kind < 2; kind++)
                        if (snprintf(sql, sizeof(sql),
                                     "CREATE TRIGGER IF NOT EXISTS main.%s AFTER %s ON %s WHEN NEW.visible = 0 AND EXISTS "
                                     "(SELECT 1 FROM artswap_system WHERE titleId = NEW.titleId AND shown = 1) BEGIN "
                                     "UPDATE %s SET visible = 1 WHERE titleId = NEW.titleId; END;",
                                     kind ? update : insert, kind ? "UPDATE OF visible" : "INSERT", table, table) >=
                                (int)sizeof(sql) ||
                            !exec(db, sql))
                            return -1;
                }
            }
            snprintf(insert, sizeof(insert), "artswap_move_sys_i_%s", name);
            snprintf(update, sizeof(update), "artswap_move_sys_u_%s", name);
            if (has_object_in(db, place->schema, insert) && has_object_in(db, place->schema, update))
                continue;
            missing++;
            if (!create)
                continue;
            char when[64], where[64], set[160], clear[160];
            field_condition(place, "NEW.", when, sizeof(when));
            field_condition(place, "", where, sizeof(where));
            snprintf(set, sizeof(set), "(SELECT setbits FROM artswap_system WHERE %s = NEW.%s)", place->by, place->key);
            snprintf(clear, sizeof(clear), "(SELECT clearbits FROM artswap_system WHERE %s = NEW.%s)", place->by,
                     place->key);
            for (int kind = 0; kind < 2; kind++) {
                /* the system writes a bit the tile should not have: it gets its own back; a value with neither tab
                 * bit stays as it is */
                char event[64];
                snprintf(event, sizeof(event), kind ? "UPDATE OF %s" : "INSERT", place->value);
                if (snprintf(sql, sizeof(sql),
                             "CREATE TRIGGER IF NOT EXISTS %s.%s AFTER %s ON %s WHEN %s(CAST(NEW.%s AS INTEGER) & 6) <> 0 "
                             "AND EXISTS (SELECT 1 FROM artswap_system s WHERE s.%s = NEW.%s AND (CAST(NEW.%s AS INTEGER) "
                             "& s.clearbits) <> 0) BEGIN UPDATE %s SET %s = (CAST(%s AS INTEGER) | %s) & ~%s WHERE "
                             "%s%s = NEW.%s AND (CAST(%s AS INTEGER) & %s) <> 0; END;",
                             place->schema, kind ? update : insert, event, table, when, place->value, place->by,
                             place->key, place->value, table, place->value, place->value, set, clear, where, place->key,
                             place->key, place->value, clear) >= (int)sizeof(sql) ||
                    !exec(db, sql))
                    return -1;
            }
        }
    return missing;
}

static bool drop_system(sqlite3 *db)
{
    static char names[4 * TILES_MAX][96];
    int n = 0;
    for (int schema = 0; schema < 2; schema++) {
        char sql[180];
        snprintf(sql, sizeof(sql),
                 "SELECT name FROM %s.sqlite_master WHERE type = 'trigger' AND name GLOB 'artswap_move_sys_*'",
                 schema ? "info" : "main");
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
            return false;
        while (n < (int)(sizeof(names) / sizeof(names[0])) && sqlite3_step(st) == SQLITE_ROW)
            snprintf(names[n++], sizeof(names[0]), "%s.\"%s\"", schema ? "info" : "main",
                     (const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    for (int i = 0; i < n; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "DROP TRIGGER %s", names[i]);
        if (!exec(db, sql))
            return false;
    }
    return exec(db, "DROP TABLE IF EXISTS main.artswap_system; DROP TABLE IF EXISTS info.artswap_system;");
}

/* A system tile's value in tbl_contentinfo and its concept: 1, 0 when there is no such tile, -1 */
static int system_tile(sqlite3 *db, const char *id, long long *value, char *concept, size_t size)
{
    const bool concepts = has_column(db, "main", "tbl_contentinfo", "localConceptId");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, concepts ? "SELECT dispLocation, ifnull(localConceptId, '') FROM tbl_contentinfo WHERE "
                                          "titleId = ?1"
                                        : "SELECT dispLocation, '' FROM tbl_contentinfo WHERE titleId = ?1",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *value = sqlite3_column_int64(st, 0);
        copy_str(concept, size, (const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/* Sets `set` and clears `clear` on `id` in every place the firmware has, where the value has a tab bit and a bit of
 * `only`; the JSON copies follow */
static bool write_system(sqlite3 *db, const char *id, const char *concept, int set, int clear, int only,
                         char users[][16], int count)
{
    for (int p = 0; p < PLACE_COUNT; p++)
        for (int u = 0; u < (p < PLACES_SHARED ? 1 : count); u++) {
            const place_t *place = &PLACES[p];
            const bool by_concept = strcmp(place->by, "concept") == 0;
            char table[64], where[64];
            static char sql[512];
            if ((by_concept && !*concept) ||
                !place_ready(db, place, p < PLACES_SHARED ? "" : users[u], table, sizeof(table)))
                continue;
            field_condition(place, "", where, sizeof(where));
            snprintf(sql, sizeof(sql),
                     "UPDATE %s.%s SET %s = (CAST(%s AS INTEGER) | %d) & ~%d WHERE %s%s = ?2 AND (CAST(%s AS INTEGER) "
                     "& 6) <> 0 AND (CAST(%s AS INTEGER) & %d) <> 0",
                     place->schema, table, place->value, place->value, set, clear, where, place->key, place->value,
                     place->value, only);
            if (run_values(db, sql, 0, by_concept ? concept : id, NULL) < 0)
                return false;
        }
    if (has_column(db, "main", "tbl_contentinfo", "AppInfoJson") &&
        run_values(db, "UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', "
                       "dispLocation) WHERE titleId = ?2 AND json_valid(AppInfoJson) AND json_type(AppInfoJson, "
                       "'$.DISPLAYLOCATION') IS NOT NULL AND AppInfoJson ->> '$.DISPLAYLOCATION' IS NOT dispLocation",
                   0, id, NULL) < 0)
        return false;
    return !*concept || fix_concept_field(db, concept, "display_location", NULL);
}

/* visible on every profile */
static bool system_visible(sqlite3 *db, const char *id, bool visible, char users[][16], int count)
{
    for (int u = 0; u < count; u++) {
        char table[64], sql[160];
        snprintf(table, sizeof(table), "tbl_iconinfo_%s", users[u]);
        if (!has_column(db, "main", table, "visible"))
            continue;
        snprintf(sql, sizeof(sql), "UPDATE %s SET visible = ?1 WHERE titleId = ?2 AND visible IS NOT ?1", table);
        if (run_values(db, sql, visible, id, NULL) < 0)
            return false;
    }
    return true;
}

/* MyHome, on every profile: in before Game Library, or out */
static bool myhome(sqlite3 *db, const char *id, bool in, char users[][16], int count)
{
    for (int u = 0; u < count; u++) {
        char table[64], sql[400];
        snprintf(table, sizeof(table), "tbl_group_%s", users[u]);
        if (!has_column(db, "main", table, "itemId") || !has_column(db, "main", table, "groupName") ||
            !has_column(db, "main", table, "idx") || !has_column(db, "main", table, "type"))
            continue;
        snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE groupName = 'MyHome' AND itemId = ?1", table);
        const int there = finds_row(db, sql, id);
        if (there < 0)
            return false;
        if (in && !there) {
            /* the places from Game Library's (else the end) on move one along */
            snprintf(sql, sizeof(sql),
                     "SELECT ifnull((SELECT idx FROM %1$s WHERE groupName = 'MyHome' AND itemId = '" GAME_LIBRARY_ID
                     "'), (SELECT ifnull(max(idx), 0) + 1 FROM %1$s WHERE groupName = 'MyHome'))",
                     table);
            sqlite3_stmt *st = NULL;
            long long at = 1;
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
                return false;
            if (sqlite3_step(st) == SQLITE_ROW)
                at = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            snprintf(sql, sizeof(sql), "UPDATE %s SET idx = idx + 1 WHERE groupName = 'MyHome' AND idx >= ?1", table);
            if (run_values(db, sql, at, NULL, NULL) < 0)
                return false;
            snprintf(sql, sizeof(sql), "INSERT INTO %s (itemId, groupName, type, idx) VALUES (?2, 'MyHome', 'title', ?1)",
                     table);
            if (run_values(db, sql, at, id, NULL) < 0)
                return false;
        } else if (!in && there) {
            snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE groupName = 'MyHome' AND itemId = ?2", table);
            if (run_values(db, sql, 0, id, NULL) < 0)
                return false;
            snprintf(sql, sizeof(sql),
                     "UPDATE %1$s SET idx = (SELECT COUNT(*) FROM %1$s g WHERE g.groupName = 'MyHome' AND g.idx <= "
                     "%1$s.idx) WHERE groupName = 'MyHome'",
                     table);
            if (run_values(db, sql, 0, NULL, NULL) < 0)
                return false;
        }
    }
    return true;
}

typedef struct {
    bool kept;
    int tab;    /* the tab bit kept, 0 for the one it has */
    bool shown; /* shown by Art Swap */
    int home;   /* its tab bit as the system had it */
    bool hid;   /* the system hid it */
} system_state_t;

static bool system_state(sqlite3 *db, const char *id, system_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!has_object(db, "artswap_system"))
        return true;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT setbits, home, hid, shown FROM main.artswap_system WHERE titleId = ?1", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->kept = true;
        out->tab = sqlite3_column_int(st, 0) & (BIT_GAMES | BIT_MEDIA);
        out->home = sqlite3_column_int(st, 1);
        out->hid = sqlite3_column_int(st, 2) != 0;
        out->shown = sqlite3_column_int(st, 3) != 0;
    }
    sqlite3_finalize(st);
    return true;
}

/* Keeps `state` in artswap_system, or lets the tile go when it is as the system had it */
static bool keep_system(sqlite3 *db, const char *id, const char *concept, const system_state_t *state,
                        char users[][16], int count)
{
    if (!state->tab && !state->shown) {
        if (!has_object(db, "artswap_system"))
            return true;
        if (run_title(db, "DELETE FROM main.artswap_system WHERE titleId = ?1", NULL, id) < 0 ||
            run_title(db, "DELETE FROM info.artswap_system WHERE titleId = ?1", NULL, id) < 0)
            return false;
        const int left = finds_row(db, "SELECT 1 FROM main.artswap_system LIMIT 1", NULL);
        return left < 0 ? false : left ? true : drop_system(db);
    }
    const int set = state->tab;
    const int clear = (state->tab ? (BIT_GAMES | BIT_MEDIA) & ~state->tab : 0) | (state->shown ? BIT_HIDDEN : 0);
    char sql[200];
    if (!exec(db, SYSTEM_TABLES))
        return false;
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO main.artswap_system VALUES (?3, ?2, %d, %d, %d, %d, %d)", set,
             clear, state->home, state->hid, state->shown);
    if (run_values(db, sql, 0, concept, id) < 0)
        return false;
    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO info.artswap_system VALUES (?3, ?2, %d, %d)", set, clear);
    return run_values(db, sql, 0, concept, id) >= 0 && system_triggers(db, users, count, true) >= 0;
}

/* The system tile `id` the person may change: not a disc placeholder, not a library */
static bool system_changeable(const char *id)
{
    return is_system(id) && !internal(id) && !library(id);
}

static int move_system(sqlite3 *db, const char *id, int tab, char *msg, size_t msglen)
{
    if (!system_changeable(id) || strcmp(id, WELCOME_ID) == 0)
        return snprintf(msg, msglen, "%s does not move", id), 1;
    long long value = 0;
    char concept[96] = "";
    const int found = system_tile(db, id, &value, concept, sizeof(concept));
    if (found <= 0)
        return snprintf(msg, msglen, found < 0 ? "it could not be read" : "no such tile"), found < 0 ? -1 : 1;
    const int from = tab_bit(value), to = tab == MEDIA_TAB ? BIT_MEDIA : BIT_GAMES;
    if (from == 0)
        return snprintf(msg, msglen, "the system keeps %s off both tabs", id), 1;
    if (from == to)
        return snprintf(msg, msglen, "%s is on that tab already", id), 1;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    system_state_t state;
    if (count < 0 || !system_state(db, id, &state))
        return snprintf(msg, msglen, "it could not be read (%s)", sqlite3_errmsg(db)), -1;
    if (!state.kept) {
        state.home = from;
        state.hid = (value & BIT_HIDDEN) != 0;
    }
    state.tab = to == state.home ? 0 : to;
    if (!keep_system(db, id, concept, &state, users, count) ||
        !write_system(db, id, concept, to, (BIT_GAMES | BIT_MEDIA) & ~to, (BIT_GAMES | BIT_MEDIA) & ~to, users,
                      count) ||
        !myhome(db, id, to == BIT_GAMES, users, count) || !forget_pin_everywhere(db, id))
        return snprintf(msg, msglen, "it could not be moved (%s)", sqlite3_errmsg(db)), -1;
    return 0;
}

int tiles_move(sqlite3 *db, const char *id, int tab, char *msg, size_t msglen)
{
    if (tab != GAMES_TAB && tab != MEDIA_TAB)
        return snprintf(msg, msglen, "a tile goes on games or media"), 1;
    return is_system(id) ? move_system(db, id, tab, msg, msglen) : move_app(db, id, tab, msg, msglen);
}

/* Whether the system hides `id`: its hidden bit, or it shows on no profile */
static bool system_hides(sqlite3 *db, const char *id, long long value)
{
    if (value & BIT_HIDDEN)
        return true;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    for (int u = 0; u < count; u++) {
        char sql[160];
        snprintf(sql, sizeof(sql), "SELECT 1 FROM tbl_iconinfo_%s WHERE titleId = ?1 AND visible = 1", users[u]);
        if (finds_row(db, sql, id) == 1)
            return false;
    }
    return true;
}

int tiles_show(sqlite3 *db, const char *id, bool show, char *msg, size_t msglen)
{
    if (!system_changeable(id))
        return snprintf(msg, msglen, "%s does not show", id), 1;
    long long value = 0;
    char concept[96] = "";
    const int found = system_tile(db, id, &value, concept, sizeof(concept));
    if (found <= 0)
        return snprintf(msg, msglen, found < 0 ? "it could not be read" : "no such tile"), found < 0 ? -1 : 1;
    if (tab_bit(value) == 0)
        return snprintf(msg, msglen, "the system keeps %s off both tabs", id), 1;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    system_state_t state;
    if (count < 0 || !system_state(db, id, &state))
        return snprintf(msg, msglen, "it could not be read (%s)", sqlite3_errmsg(db)), -1;
    if (show == state.shown)
        return snprintf(msg, msglen, show ? "%s is shown already" : "Art Swap did not show %s", id), 1;
    if (show && !system_hides(db, id, value))
        return snprintf(msg, msglen, "%s shows already", id), 1;
    if (!state.kept) {
        state.home = tab_bit(value);
        state.hid = true;
    }
    state.shown = show;
    bool ok = true;
    /* shown on every profile, it is no longer one a profile hid */
    for (int u = 0; ok && show && u < count; u++)
        if (hidden_by_us(db, users[u], id)) {
            char why[160];
            ok = tiles_hide(db, users[u], id, false, why, sizeof(why)) == 0;
        }
    ok = ok && keep_system(db, id, concept, &state, users, count) &&
         (show ? write_system(db, id, concept, 0, BIT_HIDDEN, BIT_HIDDEN, users, count)
               : write_system(db, id, concept, BIT_HIDDEN, 0, BIT_GAMES | BIT_MEDIA, users, count)) &&
         system_visible(db, id, show, users, count) && (show || forget_pin_everywhere(db, id));
    if (!ok)
        return snprintf(msg, msglen, "it could not be changed (%s)", sqlite3_errmsg(db)), -1;
    return 0;
}

/* The kept tiles' bits written again, and their triggers: what the triggers could not keep (the JSON, a profile
 * made since, a database rebuilt without them). How many tiles were off, or -1. */
static int ensure_system(sqlite3 *db, bool create)
{
    if (!has_object(db, "artswap_system"))
        return 0;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0)
        return -1;
    static char ids[64][16], concepts[64][96];
    static int sets[64], clears[64], shown[64];
    int n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT titleId, ifnull(concept, ''), setbits, clearbits, shown FROM main.artswap_system",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    while (n < 64 && sqlite3_step(st) == SQLITE_ROW) {
        copy_str(ids[n], sizeof(ids[0]), (const char *)sqlite3_column_text(st, 0));
        copy_str(concepts[n], sizeof(concepts[0]), (const char *)sqlite3_column_text(st, 1));
        sets[n] = sqlite3_column_int(st, 2);
        clears[n] = sqlite3_column_int(st, 3);
        shown[n++] = sqlite3_column_int(st, 4);
    }
    sqlite3_finalize(st);
    int off = 0;
    for (int i = 0; i < n; i++) {
        bool stale = false;
        for (int p = 0; !stale && p < PLACE_COUNT; p++)
            for (int u = 0; !stale && u < (p < PLACES_SHARED ? 1 : count); u++) {
                const place_t *place = &PLACES[p];
                const bool by_concept = strcmp(place->by, "concept") == 0;
                char table[64], where[64], sql[400];
                if ((by_concept && !concepts[i][0]) ||
                    !place_ready(db, place, p < PLACES_SHARED ? "" : users[u], table, sizeof(table)))
                    continue;
                field_condition(place, "", where, sizeof(where));
                snprintf(sql, sizeof(sql),
                         "SELECT 1 FROM %s.%s WHERE %s%s = ?1 AND (CAST(%s AS INTEGER) & 6) <> 0 AND (CAST(%s AS "
                         "INTEGER) & %d) <> 0",
                         place->schema, table, where, place->key, place->value, place->value, clears[i]);
                stale = finds_row(db, sql, by_concept ? concepts[i] : ids[i]) == 1;
                if (!stale && p == PLACE_ICON && shown[i] && has_column(db, "main", table, "visible")) {
                    snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE titleId = ?1 AND visible = 0", table);
                    stale = finds_row(db, sql, ids[i]) == 1;
                }
            }
        if (!stale && has_column(db, "main", "tbl_contentinfo", "AppInfoJson"))
            stale = finds_row(db, "SELECT 1 FROM tbl_contentinfo WHERE titleId = ?1 AND json_valid(AppInfoJson) AND "
                                  "json_type(AppInfoJson, '$.DISPLAYLOCATION') IS NOT NULL AND AppInfoJson ->> "
                                  "'$.DISPLAYLOCATION' IS NOT dispLocation",
                              ids[i]) == 1;
        if (!stale)
            continue;
        off++;
        const bool known = (sets[i] & ~(BIT_GAMES | BIT_MEDIA)) == 0 && (clears[i] & sets[i]) == 0 &&
                           (clears[i] & ~(BIT_HIDDEN | BIT_GAMES | BIT_MEDIA)) == 0;
        if (create && (!known || !write_system(db, ids[i], concepts[i], sets[i], clears[i], clears[i], users, count) ||
                       (shown[i] && !system_visible(db, ids[i], true, users, count))))
            return -1;
    }
    const int missing = system_triggers(db, users, count, false);
    if (missing < 0 || (missing > 0 && create && system_triggers(db, users, count, true) < 0))
        return -1;
    return off + missing;
}

/* The moved apps written again wherever they are off their tab (the JSON, a profile made since, a write while the
 * triggers were missing), and the triggers. How many were off, or -1. */
static int ensure_moves(sqlite3 *db, bool create)
{
    if (!has_object(db, "artswap_moved"))
        return 0;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0)
        return -1;
    static char ids[TILES_MAX][16];
    static int tabs[TILES_MAX];
    int n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT titleId, tab FROM main.artswap_moved", -1, &st, NULL) != SQLITE_OK)
        return -1;
    while (n < TILES_MAX && sqlite3_step(st) == SQLITE_ROW) {
        copy_str(ids[n], sizeof(ids[0]), (const char *)sqlite3_column_text(st, 0));
        tabs[n++] = sqlite3_column_int(st, 1);
    }
    sqlite3_finalize(st);
    int off = 0;
    for (int i = 0; i < n; i++) {
        char sql[400], concept[96];
        const int own = own_concept(db, ids[i], concept, sizeof(concept));
        if (own < 0)
            return -1;
        snprintf(sql, sizeof(sql), "SELECT 1 FROM tbl_contentinfo WHERE titleId = ?1 AND (dispLocation <> %d%s)",
                 tabs[i],
                 has_column(db, "main", "tbl_contentinfo", "AppInfoJson")
                     ? " OR (json_valid(AppInfoJson) AND AppInfoJson ->> '$.DISPLAYLOCATION' IS NOT dispLocation)"
                     : "");
        bool stale = finds_row(db, sql, ids[i]) == 1;
        if (!stale && has_columns(db, "info", "tbl_appinfo", INFO_COLUMNS)) {
            snprintf(sql, sizeof(sql),
                     "SELECT 1 FROM info.tbl_appinfo WHERE titleId = ?1 AND key = 'DISPLAYLOCATION' AND val IS NOT %d",
                     tabs[i]);
            stale = finds_row(db, sql, ids[i]) == 1;
        }
        for (int u = 0; !stale && u < count; u++) {
            snprintf(sql, sizeof(sql),
                     "SELECT 1 FROM tbl_iconinfo_%s WHERE titleId = ?1 AND dispLocation IN (138, 188) AND dispLocation "
                     "<> %d",
                     users[u], tabs[i]);
            stale = finds_row(db, sql, ids[i]) == 1;
        }
        if (!stale && own == 1) {
            /* its concept: recorded for the triggers, and on the tab */
            stale = finds_row(db, "SELECT 1 FROM main.artswap_moved_concept WHERE concept = ?1", concept) != 1;
            if (!stale && has_columns(db, "main", "tbl_conceptmetadata", CONCEPT_META_COLUMNS)) {
                snprintf(sql, sizeof(sql),
                         "SELECT 1 FROM tbl_conceptmetadata WHERE localConceptId = ?1 AND dispLocation IN (138, 188) "
                         "AND dispLocation <> %d",
                         tabs[i]);
                stale = finds_row(db, sql, concept) == 1;
            }
            if (!stale && has_columns(db, "info", "tbl_conceptinfo", CONCEPT_INFO_COLUMNS)) {
                snprintf(sql, sizeof(sql),
                         "SELECT 1 FROM info.tbl_conceptinfo WHERE local_concept_id = ?1 AND key = 'display_location' "
                         "AND val IS NOT %d",
                         tabs[i]);
                stale = finds_row(db, sql, concept) == 1;
            }
            for (int u = 0; !stale && u < count; u++) {
                char table[64];
                snprintf(table, sizeof(table), "tbl_concepticoninfo_%s", users[u]);
                if (!has_columns(db, "main", table, CONCEPT_ICON_COLUMNS))
                    continue;
                snprintf(sql, sizeof(sql),
                         "SELECT 1 FROM %s WHERE localConceptId = ?1 AND dispLocation IN (1162, 2236) AND dispLocation "
                         "<> %d",
                         table, tabs[i] + (tabs[i] == MEDIA_TAB ? 2048 : 1024));
                stale = finds_row(db, sql, concept) == 1;
            }
        }
        if (!stale)
            continue;
        off++;
        if (!create)
            continue;
        /* the app may be gone; a move recorded again keeps its home */
        if (finds_row(db, "SELECT 1 FROM tbl_contentinfo WHERE titleId = ?1", ids[i]) != 1)
            continue;
        if (!exec(db, MOVED_TABLES) || !write_move(db, ids[i], tabs[i], users, count))
            return -1;
        if (own == 1) {
            snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO main.artswap_moved_concept VALUES (?2, ?3, %d)",
                     tabs[i]);
            if (run_values(db, sql, 0, concept, ids[i]) < 0)
                return -1;
            snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO info.artswap_moved_concept VALUES (?2, ?3, %d)",
                     tabs[i]);
            if (run_values(db, sql, 0, concept, ids[i]) < 0)
                return -1;
        }
    }
    const int missing = move_triggers(db, users, count, false);
    if (missing < 0 || (missing > 0 && create && (!exec(db, MOVED_TABLES) || move_triggers(db, users, count, true) < 0)))
        return -1;
    return off + missing;
}

/* ---- TabShifter's leftovers ----
 *
 * TabShifter put a "TS" launcher tile, TBSH00001, into both databases and three folders of its own, and it stays
 * after TabShifter is deleted. Its nsx_content_flag_* rows in tbl_info_<user> stay too: the system writes rows of
 * that name, so TabShifter's cannot be told from them. */

#define TABSHIFTER_ID "TBSH00001"
#define TABSHIFTER_CONCEPT "cid:local:" TABSHIFTER_ID

static int delete_where(sqlite3 *db, const char *schema, const char *table, const char *column, const char *value)
{
    if (!has_column(db, schema, table, column))
        return 0;
    char sql[200];
    snprintf(sql, sizeof(sql), "DELETE FROM %s.\"%s\" WHERE \"%s\" = ?2", schema, table, column);
    return run_values(db, sql, 0, value, NULL);
}

int tiles_tabshifter(sqlite3 *db, int *removed, char *msg, size_t msglen)
{
    *removed = 0;
    static const char *const shared[][3] = {{"main", "tbl_contentinfo", "titleId"},
                                            {"main", "tbl_conceptmetadata", "localConceptId"},
                                            {"info", "tbl_appinfo", "titleId"},
                                            {"info", "tbl_conceptinfo", "local_concept_id"}};
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0)
        return snprintf(msg, msglen, "the profiles could not be read"), -1;
    for (int i = 0; i < 4; i++)
        for (int v = 0; v < 2; v++) {
            const int n = delete_where(db, shared[i][0], shared[i][1], shared[i][2], v ? TABSHIFTER_CONCEPT : TABSHIFTER_ID);
            if (n < 0)
                return snprintf(msg, msglen, "TabShifter's tile could not be removed (%s)", sqlite3_errmsg(db)), -1;
            *removed += n;
        }
    for (int u = 0; u < count; u++) {
        char icon[64], concept[64], group[64];
        snprintf(icon, sizeof(icon), "tbl_iconinfo_%s", users[u]);
        snprintf(concept, sizeof(concept), "tbl_concepticoninfo_%s", users[u]);
        snprintf(group, sizeof(group), "tbl_group_%s", users[u]);
        const char *const places[][3] = {{icon, "titleId", TABSHIFTER_ID},
                                         {concept, "localConceptId", TABSHIFTER_CONCEPT},
                                         {concept, "primaryTitleId", TABSHIFTER_ID},
                                         {group, "itemId", TABSHIFTER_ID}};
        for (int j = 0; j < 4; j++) {
            const int n = delete_where(db, "main", places[j][0], places[j][1], places[j][2]);
            if (n < 0)
                return snprintf(msg, msglen, "TabShifter's tile could not be removed (%s)", sqlite3_errmsg(db)), -1;
            *removed += n;
        }
        if (!forget_pin(db, users[u], TABSHIFTER_ID))
            return snprintf(msg, msglen, "TabShifter's tile could not be removed (%s)", sqlite3_errmsg(db)), -1;
    }
    if (has_object(db, "artswap_hidden") && delete_where(db, "main", "artswap_hidden", "titleId", TABSHIFTER_ID) < 0)
        return snprintf(msg, msglen, "TabShifter's tile could not be removed (%s)", sqlite3_errmsg(db)), -1;
    return 0;
}

static bool remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    DIR *dir = opendir(path);
    if (dir == NULL)
        return false;
    bool ok = true;
    for (struct dirent *e; ok && (e = readdir(dir)) != NULL;) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        ok = remove_tree(child);
    }
    closedir(dir);
    return ok && rmdir(path) == 0;
}

void tiles_tabshifter_folders(void)
{
    static const char *const folders[] = {"/user/app/" TABSHIFTER_ID, "/user/appmeta/" TABSHIFTER_ID,
                                          "/system_data/priv/appmeta/" TABSHIFTER_ID};
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        char real[512];
        if (!remove_tree(cpath(folders[i], real, sizeof(real))))
            log_line("tiles: %s could not be removed: %s", real, strerror(errno));
    }
}

/* ---- what Art Swap 1.4 left ----
 *
 * 1.4 locked the Games row far in the future: lastAccessIndex from 1000000000 down, lastPlayedIndex the same, and
 * the dates beside them 2099-12-31. Those go back to the console's counters, in the same order, and to now. It also
 * moved an app by flipping its tab bits, which made 140 of Media's 188 and 186 of Games' 138 (2188 and 2234 in
 * tbl_concepticoninfo_<user>): those are put right, and the app moved as Art Swap moves one now, so it stays. */

#define LOCK_FLOOR 900000000LL

static const char *const kLockTables[] = {"tbl_contentinfo", "tbl_conceptmetadata"};
static const char *const kLockTimes[] = {"lastAccessTime",
                                         "lastPlayedDate",
                                         "lastPlayedDateOnConsole",
                                         "recentActivityDatePlayedOrPurchased",
                                         "recentActivityDatePlayedOrInstalled",
                                         "lastInteractedTime"};

/* Every value of `column` at LOCK_FLOOR or more, in every table that has it, given the counter's next values in the
 * same order. The values changed, or -1. */
static int unlock_column(sqlite3 *db, char tables[][64], int ntables, const char *column, const char *counter)
{
    static char sql[8192];
    size_t used = 0;
    int parts = 0;
    for (int t = 0; t < ntables; t++)
        if (has_column(db, "main", tables[t], column))
            used += (size_t)snprintf(sql + used, sizeof(sql) - used, "%sSELECT \"%s\" v FROM \"%s\" WHERE \"%s\" >= %lld",
                                     parts++ ? " UNION " : "", column, tables[t], column, LOCK_FLOOR);
    if (parts == 0 || used >= sizeof(sql))
        return 0;
    /* the distinct values, smallest first: each the counter plus its rank */
    static long long values[TILES_MAX * 4];
    int n = 0;
    char query[8300];
    snprintf(query, sizeof(query), "SELECT DISTINCT v FROM (%s) ORDER BY v", sql);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &st, NULL) != SQLITE_OK)
        return -1;
    while (n < (int)(sizeof(values) / sizeof(values[0])) && sqlite3_step(st) == SQLITE_ROW)
        values[n++] = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (n == 0)
        return 0;
    long long base = 0;
    const bool versioned = has_object(db, "tbl_version");
    if (versioned && sqlite3_prepare_v2(db, "SELECT status FROM tbl_version WHERE category = ?1", -1, &st, NULL) ==
                         SQLITE_OK) {
        sqlite3_bind_text(st, 1, counter, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            base = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (base >= LOCK_FLOOR)
        base = 0;
    int changed = 0;
    for (int t = 0; t < ntables; t++) {
        if (!has_column(db, "main", tables[t], column))
            continue;
        char update[300];
        snprintf(update, sizeof(update), "UPDATE \"%s\" SET \"%s\" = ?1 WHERE \"%s\" = CAST(?2 AS INTEGER)", tables[t],
                 column, column);
        for (int i = 0; i < n; i++) {
            char old[32];
            snprintf(old, sizeof(old), "%lld", values[i]);
            const int c = run_values(db, update, base + 1 + i, old, NULL);
            if (c < 0)
                return -1;
            changed += c;
        }
    }
    if (versioned && run_values(db, "UPDATE tbl_version SET status = ?1 WHERE category = ?2", base + n, counter, NULL) <
                         0)
        return -1;
    return changed;
}

int tiles_normalize(sqlite3 *db, int *changed, char *msg, size_t msglen)
{
    *changed = 0;
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    if (count < 0)
        return snprintf(msg, msglen, "the profiles could not be read"), -1;
    static char tables[2 * TILES_USERS + 2][64];
    int ntables = 0;
    for (int u = 0; u < count; u++) {
        snprintf(tables[ntables++], sizeof(tables[0]), "tbl_iconinfo_%s", users[u]);
        snprintf(tables[ntables], sizeof(tables[0]), "tbl_concepticoninfo_%s", users[u]);
        if (has_object(db, tables[ntables]))
            ntables++;
    }
    for (size_t t = 0; t < sizeof(kLockTables) / sizeof(kLockTables[0]); t++)
        if (has_object(db, kLockTables[t]))
            copy_str(tables[ntables++], sizeof(tables[0]), kLockTables[t]);
    /* the locked order, with the pins' triggers out of the way */
    for (int u = 0; u < count; u++)
        if (!drop_pin_triggers(db, users[u]))
            return snprintf(msg, msglen, "%s", sqlite3_errmsg(db)), -1;
    const int access = unlock_column(db, tables, ntables, "lastAccessIndex", "access_index");
    const int played = access < 0 ? -1 : unlock_column(db, tables, ntables, "lastPlayedIndex", "played_index");
    if (access < 0 || played < 0 || ensure_pin_triggers(db, true) < 0)
        return snprintf(msg, msglen, "the locked order could not be undone (%s)", sqlite3_errmsg(db)), -1;
    *changed += access;
    char now[40];
    const time_t t = time(NULL);
    struct tm g;
    gmtime_r(&t, &g);
    strftime(now, sizeof(now), "%Y-%m-%d %H:%M:%S.000", &g);
    for (int i = 0; i < ntables; i++)
        for (size_t c = 0; c < sizeof(kLockTimes) / sizeof(kLockTimes[0]); c++) {
            if (!has_column(db, "main", tables[i], kLockTimes[c]))
                continue;
            char sql[300];
            snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET \"%s\" = ?2 WHERE \"%s\" LIKE '2099-12-31%%'", tables[i],
                     kLockTimes[c], kLockTimes[c]);
            const int n = run_values(db, sql, 0, now, NULL);
            if (n < 0)
                return snprintf(msg, msglen, "the locked order could not be undone (%s)", sqlite3_errmsg(db)), -1;
            *changed += n;
        }
    /* an app moved by its bits */
    static char moved[TILES_MAX][16];
    static int moved_tab[TILES_MAX];
    int nmoved = 0;
    sqlite3_stmt *st = NULL;
    char sql[400];
    for (int u = 0; u < count; u++) {
        snprintf(sql, sizeof(sql),
                 "SELECT titleId, dispLocation FROM tbl_iconinfo_%s WHERE dispLocation IN (140, 186) AND titleId NOT "
                 "GLOB 'NPXS*'",
                 users[u]);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
            continue;
        while (nmoved < TILES_MAX && sqlite3_step(st) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(st, 0);
            bool seen = false;
            for (int i = 0; i < nmoved && !seen; i++)
                seen = strcmp(moved[i], id) == 0;
            if (!seen) {
                copy_str(moved[nmoved], sizeof(moved[0]), id);
                moved_tab[nmoved++] = sqlite3_column_int(st, 1) == 140 ? MEDIA_TAB : GAMES_TAB;
            }
        }
        sqlite3_finalize(st);
    }
    for (int i = 0; i < ntables; i++) {
        const bool concept = strncmp(tables[i], "tbl_concepticoninfo_", 20) == 0;
        if (!has_column(db, "main", tables[i], "dispLocation"))
            continue;
        snprintf(sql, sizeof(sql),
                 concept ? "UPDATE \"%s\" SET dispLocation = CASE dispLocation WHEN 2188 THEN 2236 ELSE 1162 END WHERE "
                           "dispLocation IN (2188, 2234) AND localConceptId NOT GLOB 'cid:local:NPXS*'"
                         : "UPDATE \"%s\" SET dispLocation = CASE dispLocation WHEN 140 THEN 188 ELSE 138 END WHERE "
                           "dispLocation IN (140, 186) AND %s NOT GLOB 'NPXS*'",
                 tables[i], strcmp(tables[i], "tbl_conceptmetadata") == 0 ? "localConceptId" : "titleId");
        if (strcmp(tables[i], "tbl_conceptmetadata") == 0)
            snprintf(sql, sizeof(sql),
                     "UPDATE tbl_conceptmetadata SET dispLocation = CASE dispLocation WHEN 140 THEN 188 ELSE 138 END "
                     "WHERE dispLocation IN (140, 186) AND localConceptId NOT GLOB 'cid:local:NPXS*'");
        const int n = run_values(db, sql, 0, NULL, NULL);
        if (n < 0)
            return snprintf(msg, msglen, "the tabs could not be put right (%s)", sqlite3_errmsg(db)), -1;
        *changed += n;
    }
    for (int i = 0; i < nmoved; i++) {
        /* back on the tab its bits named, then moved as Art Swap moves one now, when it is an app that moves */
        int from = 0;
        if (!app_movable(db, moved[i], &from) || (from != GAMES_TAB && from != MEDIA_TAB))
            continue;
        if (!record_move(db, moved[i], moved_tab[i] == MEDIA_TAB ? GAMES_TAB : MEDIA_TAB, moved_tab[i], users, count) ||
            !write_move(db, moved[i], moved_tab[i], users, count))
            return snprintf(msg, msglen, "%s could not be moved (%s)", moved[i], sqlite3_errmsg(db)), -1;
        log_line("tiles: %s, moved by Art Swap 1.4, kept on %s", moved[i], moved_tab[i] == MEDIA_TAB ? "Media" : "Games");
    }
    return 0;
}

/* ---- everything ---- */

int tiles_reset(sqlite3 *db, char *msg, size_t msglen)
{
    char users[TILES_USERS][16];
    const int count = tiles_users(db, users, TILES_USERS);
    bool ok = count >= 0 && show_all_hidden(db) && remove_pins(db);
    /* the tiles Art Swap shows go back to what the system had; moved ones stay where they are */
    if (ok && has_object(db, "artswap_system")) {
        static char ids[64][16], concepts[64][96];
        int n = 0;
        sqlite3_stmt *st = NULL;
        ok = sqlite3_prepare_v2(db, "SELECT titleId, ifnull(concept, '') FROM main.artswap_system WHERE shown = 1", -1,
                                &st, NULL) == SQLITE_OK;
        while (ok && n < 64 && sqlite3_step(st) == SQLITE_ROW) {
            copy_str(ids[n], sizeof(ids[0]), (const char *)sqlite3_column_text(st, 0));
            copy_str(concepts[n++], sizeof(concepts[0]), (const char *)sqlite3_column_text(st, 1));
        }
        sqlite3_finalize(st);
        ok = ok && drop_system(db);
        for (int i = 0; ok && i < n; i++)
            ok = write_system(db, ids[i], concepts[i], BIT_HIDDEN, 0, BIT_GAMES | BIT_MEDIA, users, count) &&
                 system_visible(db, ids[i], false, users, count);
    }
    ok = ok && drop_moves(db);
    if (!ok)
        return snprintf(msg, msglen, "Art Swap's changes could not be removed (%s)", sqlite3_errmsg(db)), -1;
    return 0;
}

/* ---- reading ---- */

static int by_home_order(const void *a, const void *b)
{
    const tile_t *x = a, *y = b;
    /* the Games tab, then Media, then those on neither or hidden */
    const int gx = !x->visible || !x->tab ? 2 : x->tab == GAMES_TAB ? 0 : 1;
    const int gy = !y->visible || !y->tab ? 2 : y->tab == GAMES_TAB ? 0 : 1;
    if (gx != gy)
        return gx - gy;
    if (gx == 2)
        return strcmp(x->name, y->name);
    /* a row as the home screen orders it: by priority (the hubs, the row, the libraries), then the most recent */
    if (x->priority != y->priority)
        return x->priority < y->priority ? -1 : 1;
    if (x->index != y->index)
        return x->index > y->index ? -1 : 1;
    return strcmp(x->id, y->id);
}

int tiles_read(sqlite3 *db, const char *user, tile_t *out, int max, char *err, size_t errlen)
{
    char icon[64], concept[64];
    snprintf(icon, sizeof(icon), "tbl_iconinfo_%s", user);
    snprintf(concept, sizeof(concept), "tbl_concepticoninfo_%s", user);
    static const char *const needed[] = {"titleId", "dispLocation", "visible", NULL};
    if (!tiles_user_known(db, user) || !has_columns(db, "main", icon, needed)) {
        snprintf(err, errlen, "This console's home-screen database keeps its tiles in a way Art Swap does not know");
        return -1;
    }
    const bool concepts = has_object(db, concept) && has_column(db, "main", concept, "conceptName") &&
                          has_column(db, "main", icon, "localConceptId");
    const bool content = has_column(db, "main", "tbl_contentinfo", "titleId");
    char sql[1600], joins[300] = "";
    if (concepts)
        snprintf(joins, sizeof(joins), "LEFT JOIN \"%s\" c ON c.localConceptId = i.localConceptId ", concept);
    if (content)
        snprintf(joins + strlen(joins), sizeof(joins) - strlen(joins),
                 "LEFT JOIN tbl_contentinfo t ON t.titleId = i.titleId");
    snprintf(sql, sizeof(sql),
             "SELECT i.titleId, %s, %s, i.dispLocation, i.visible, %s, %s, %s, %s, %s FROM \"%s\" i %s",
             has_column(db, "main", icon, "titleName") ? "i.titleName" : "''", concepts ? "c.conceptName" : "''",
             has_column(db, "main", icon, "lastAccessIndex") ? "i.lastAccessIndex" : "0",
             has_column(db, "main", icon, "installStatus") ? "i.installStatus" : "2",
             content && has_column(db, "main", "tbl_contentinfo", "sortPriority") ? "ifnull(t.sortPriority, 100)" : "100",
             content && has_column(db, "main", "tbl_contentinfo", "appType") ? "ifnull(t.appType, -1)" : "-1",
             content && has_column(db, "main", "tbl_contentinfo", "contentLocation") ? "ifnull(t.contentLocation, -1)"
                                                                                       : "-1",
             icon, joins);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "The home screen's database could not be read (%s)", sqlite3_errmsg(db));
        return -1;
    }
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(st, 0);
        if (id == NULL || strlen(id) != 9 || internal(id) || strcmp(id, TABSHIFTER_ID) == 0)
            continue;
        bool seen = false;
        for (int i = 0; i < n && !seen; i++)
            seen = strcmp(out[i].id, id) == 0;
        if (seen)
            continue;
        tile_t *t = &out[n];
        memset(t, 0, sizeof(*t));
        copy_str(t->id, sizeof(t->id), id);
        const char *title_name = (const char *)sqlite3_column_text(st, 1);
        const char *concept_name = (const char *)sqlite3_column_text(st, 2);
        copy_str(t->name, sizeof(t->name), concept_name && *concept_name ? concept_name : title_name ? title_name : id);
        for (char *c = t->name; *c; c++)
            if (*c == '\t' || *c == '\n' || *c == '\r')
                *c = ' ';
        const long long value = sqlite3_column_int64(st, 3);
        t->visible = sqlite3_column_int(st, 4) == 1;
        t->index = sqlite3_column_int64(st, 5);
        const int install = sqlite3_column_int(st, 6);
        t->priority = sqlite3_column_int(st, 7);
        const int type = sqlite3_column_int(st, 8), place = sqlite3_column_int(st, 9);
        t->system = is_system(id);
        if (t->system) {
            const int bit = tab_bit(value);
            t->tab = bit == BIT_GAMES ? GAMES_TAB : bit == BIT_MEDIA ? MEDIA_TAB : 0;
        } else {
            /* 140 and 186: an app Art Swap 1.4 moved by its bits */
            t->tab = value == GAMES_TAB || value == 186 ? GAMES_TAB : value == MEDIA_TAB || value == 140 ? MEDIA_TAB : 0;
        }
        t->hidden_by_us = hidden_by_us(db, user, id);
        t->shown = t->system && shown_by_us(db, id);
        t->pin = pin_of(db, user, id);
        /* a leftover of something uninstalled, or one hidden on neither tab: not a tile of the home screen */
        if (!t->visible && !t->hidden_by_us && (install != 2 || !t->tab))
            continue;
        if (t->system) {
            if (library(id))
                t->caps = 0;
            else if (strcmp(id, WELCOME_ID) == 0)
                t->caps = t->tab ? TILE_SHOW : 0;
            else if (t->tab) {
                t->caps = TILE_MOVE;
                if (t->visible && !t->shown)
                    t->caps |= TILE_HIDE;
                if (t->shown || (!t->visible && !t->hidden_by_us && system_hides(db, id, value)))
                    t->caps |= TILE_SHOW;
                if (t->visible && row_system(id) && !(value & BIT_HIDDEN))
                    t->caps |= TILE_ORDER;
            }
            /* one the system hides and Art Swap does not show is not on its tab: under Hidden */
            if (!t->visible && !t->hidden_by_us && !(t->caps & TILE_SHOW))
                continue;
        } else {
            if (can_move_app(id, type, place) && t->tab && (value == GAMES_TAB || value == MEDIA_TAB))
                t->caps |= TILE_MOVE;
            if ((t->visible || t->hidden_by_us) && strcmp(id, ARTSWAP_ID) != 0)
                t->caps |= TILE_HIDE;
            if (t->visible && (value == GAMES_TAB || value == MEDIA_TAB))
                t->caps |= TILE_ORDER;
        }
        t->row = (t->caps & TILE_ORDER) != 0;
        n++;
    }
    sqlite3_finalize(st);
    qsort(out, (size_t)n, sizeof(out[0]), by_home_order);
    return n;
}

/* ---- when the helper starts ---- */

void tiles_startup(void)
{
    char err[300] = "";
    sqlite3 *db = appdb_open(SQLITE_OPEN_READWRITE, err, sizeof(err));
    if (db == NULL)
        return;
    const bool ours = has_object(db, "artswap_pin") || has_object(db, "artswap_hidden") ||
                      has_object(db, "artswap_moved") || has_object(db, "artswap_system");
    if (!ours || tiles_conflict(db)) {
        if (ours)
            log_line("tiles: Lineup's changes are in the database too: left alone");
        sqlite3_close(db);
        return;
    }
    if (!tiles_attach(db, err, sizeof(err))) {
        log_line("tiles: not checked: %s", err);
        sqlite3_close(db);
        return;
    }
    const int pins = ensure_pin_triggers(db, false), hidden = ensure_hidden(db, false), moves = ensure_moves(db, false),
              system = ensure_system(db, false);
    if (pins < 0 || hidden < 0 || moves < 0 || system < 0) {
        log_line("tiles: not checked: %s", sqlite3_errmsg(db));
    } else if (pins + hidden + moves + system > 0) {
        bool ok = tiles_backup(db, err, sizeof(err)) && exec(db, "BEGIN IMMEDIATE");
        const bool began = ok;
        ok = ok && ensure_pin_triggers(db, true) >= 0 && ensure_hidden(db, true) >= 0 && ensure_moves(db, true) >= 0 &&
             ensure_system(db, true) >= 0;
        char check[128] = "";
        ok = ok && pragma_text(db, "PRAGMA integrity_check", check, sizeof(check)) == 0;
        ok = ok && exec(db, "COMMIT");
        if (!ok && began)
            exec(db, "ROLLBACK");
        log_line("tiles: %s (pins %d, hidden %d, moves %d, system tiles %d)%s%s",
                 ok ? "put back what the console undid" : "not put back", pins, hidden, moves, system,
                 *err ? ": " : "", err);
    }
    sqlite3_close(db);
}
