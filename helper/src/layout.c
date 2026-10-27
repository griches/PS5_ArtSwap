/*
 * Art Swap helper - the home screen's layout (layout.h).
 *
 * Ported from PSGFX by elripalda (src/layout.h, GPL-3.0-or-later,
 * https://github.com/elripalda/psgfx), which found where the home screen
 * keeps it, in its database, /system_data/priv/mms/app.db:
 * - each user's tbl_iconinfo_<user> has a row a tile: dispLocation, a bit
 *   field (0x02 the Games row, 0x04 the Media row; only those two bits are
 *   changed), and visible (0 hides the tile);
 * - the same dispLocation is kept in each user's tbl_concepticoninfo_<user>
 *   and in tbl_conceptmetadata (by the concept), and in tbl_contentinfo (by
 *   the title);
 * - the system's own tiles on the Games row (PlayStation Store, Game Library)
 *   are the group MyHome of tbl_group_<user>, in idx order: for those the
 *   console goes by the group, not by dispLocation;
 * - a row is in the order its tiles were last used: lastAccessIndex, and the
 *   times beside it. A locked order is set far in the future (2099), so a
 *   game played later comes after it rather than first.
 *
 * The person's wishes are kept in /data/artswap/layout.txt and written at
 * Apply changes (home.c), just before the home screen restarts, every time:
 * a firmware update or a rebuilt database that put a tile back is put right
 * by the next Apply changes. Every user's tables get the same layout. The
 * database is copied aside before the first change (appdb_backup), and the
 * whole change is one transaction, checked before it is committed.
 *
 * layout.txt, a line each, tab separated:
 *   place <id> <where> <original>   a tile Art Swap moves (original: where it was before)
 *   order <none|locked|free>        free: written once, then none
 *   seq <id> <id> ...               the Games row's order wished
 *   dirty 1                         an order waits to be written
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "layout.h"
#include "appdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TILES_MAX 1024
#define PLACES_MAX 512
#define SEQ_MAX 512
#define USERS_MAX 16

#define BIT_GAMES 0x02
#define BIT_MEDIA 0x04
#define LIBRARY "NPXS40071"
#define LOCK_INDEX 1000000000LL
#define LOCK_TIME 4102401600LL /* 2099-12-31 12:00 UTC */

/* Disc placeholders and such: never shown (PSGFX's list) */
static const char *kInternal[] = {"NPXS40140", "NPXS40144", "NPXS40145", "NPXS40148", "NPXS40149",
                                  "NPXS40150", "NPXS40205", "NPXS40106", "NPXS40063"};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* layout.txt */

/* ---- the person's wishes (layout.txt), g_lock held ---- */

typedef struct {
    char id[16];
    char where[8];
    char original[8];
} place_t;

typedef struct {
    place_t places[PLACES_MAX];
    int nplaces;
    char mode[8];
    char seq[SEQ_MAX][16];
    int nseq;
    bool dirty;
} state_t;

static bool plain_id(const char *id)
{
    const size_t len = strlen(id);
    if (len == 0 || len > 15)
        return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' ||
              *p == '-'))
            return false;
    return true;
}

static bool known_place(const char *where)
{
    return strcmp(where, "games") == 0 || strcmp(where, "media") == 0 || strcmp(where, "hidden") == 0;
}

static void state_load(state_t *s)
{
    memset(s, 0, sizeof(*s));
    copy_str(s->mode, sizeof(s->mode), "none");
    char path[512];
    buf_t text = {0};
    if (!file_read(data_path("layout.txt", path, sizeof(path)), &text) || text.len == 0 || !buf_append(&text, "", 1)) {
        buf_free(&text);
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r((char *)text.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *rest = NULL;
        const char *what = strtok_r(line, "\t", &rest);
        if (what == NULL)
            continue;
        if (strcmp(what, "place") == 0 && s->nplaces < PLACES_MAX) {
            const char *id = strtok_r(NULL, "\t", &rest), *where = strtok_r(NULL, "\t", &rest),
                       *original = strtok_r(NULL, "\t", &rest);
            if (id && where && original && plain_id(id) && known_place(where) && known_place(original)) {
                place_t *p = &s->places[s->nplaces++];
                copy_str(p->id, sizeof(p->id), id);
                copy_str(p->where, sizeof(p->where), where);
                copy_str(p->original, sizeof(p->original), original);
            }
        } else if (strcmp(what, "order") == 0) {
            const char *mode = strtok_r(NULL, "\t", &rest);
            if (mode && (strcmp(mode, "locked") == 0 || strcmp(mode, "free") == 0))
                copy_str(s->mode, sizeof(s->mode), mode);
        } else if (strcmp(what, "seq") == 0) {
            for (const char *id; (id = strtok_r(NULL, "\t", &rest)) != NULL && s->nseq < SEQ_MAX;)
                if (plain_id(id))
                    copy_str(s->seq[s->nseq++], sizeof(s->seq[0]), id);
        } else if (strcmp(what, "dirty") == 0) {
            const char *flag = strtok_r(NULL, "\t", &rest);
            s->dirty = flag && strcmp(flag, "1") == 0;
        }
    }
    buf_free(&text);
}

static bool state_save(const state_t *s)
{
    char path[512];
    buf_t text = {0};
    for (int i = 0; i < s->nplaces; i++)
        buf_printf(&text, "place\t%s\t%s\t%s\n", s->places[i].id, s->places[i].where, s->places[i].original);
    buf_printf(&text, "order\t%s\n", s->mode);
    if (s->nseq > 0) {
        buf_printf(&text, "seq");
        for (int i = 0; i < s->nseq; i++)
            buf_printf(&text, "\t%s", s->seq[i]);
        buf_printf(&text, "\n");
    }
    if (s->dirty)
        buf_printf(&text, "dirty\t1\n");
    const bool ok = text.data != NULL &&
                    file_write_replace(data_path("layout.txt", path, sizeof(path)), text.data, text.len, NULL);
    buf_free(&text);
    return ok;
}

static bool waiting(const state_t *s)
{
    return s->nplaces > 0 || strcmp(s->mode, "locked") == 0 || s->dirty;
}

/* What LAYOUT says of the order: free only while it waits to be written */
static const char *order_shown(const state_t *s)
{
    if (strcmp(s->mode, "locked") == 0)
        return "locked";
    return strcmp(s->mode, "free") == 0 && s->dirty ? "free" : "none";
}

/* ---- the home screen's tiles, as its database has them ---- */

typedef struct {
    char id[16];
    char name[256];
    char concept[96];
    long long disp, access, home_idx;
    long long install;
    bool visible, system, home;
    const char *now; /* games, media or hidden */
} tile_t;

typedef struct {
    tile_t *tiles;
    int n;
    char users[USERS_MAX][32];
    int nusers;
    char user[32]; /* the one read: the most tiles */
    long long access_index, played_index;
    bool has_version;
} home_t;

static void home_free(home_t *h)
{
    free(h->tiles);
    memset(h, 0, sizeof(*h));
}

static bool table_exists(sqlite3 *db, const char *table)
{
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return found;
}

static long long version(sqlite3 *db, const char *category)
{
    sqlite3_stmt *st = NULL;
    long long value = 0;
    if (sqlite3_prepare_v2(db, "SELECT status FROM tbl_version WHERE category = ?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, category, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            value = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

static int count_rows(sqlite3 *db, const char *table)
{
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM \"%s\"", table);
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static bool internal(const char *id)
{
    for (size_t i = 0; i < sizeof(kInternal) / sizeof(kInternal[0]); i++)
        if (strcmp(id, kInternal[i]) == 0)
            return true;
    return false;
}

static tile_t *find_tile(home_t *h, const char *id);

/* The users, the tiles of the one with the most, where each is now */
static bool home_read(sqlite3 *db, home_t *h, char *err, size_t errlen)
{
    memset(h, 0, sizeof(*h));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name LIKE 'tbl_iconinfo_%' "
                               "ORDER BY name",
                           -1, &st, NULL) == SQLITE_OK) {
        int best = -1;
        while (sqlite3_step(st) == SQLITE_ROW && h->nusers < USERS_MAX) {
            const char *table = (const char *)sqlite3_column_text(st, 0);
            if (table == NULL || strlen(table) - 13 >= sizeof(h->users[0]))
                continue;
            copy_str(h->users[h->nusers], sizeof(h->users[0]), table + 13);
            h->nusers++;
        }
        sqlite3_finalize(st);
        st = NULL;
        for (int i = 0; i < h->nusers; i++) {
            char table[64];
            snprintf(table, sizeof(table), "tbl_iconinfo_%s", h->users[i]);
            const int rows = count_rows(db, table);
            if (rows > best) {
                best = rows;
                copy_str(h->user, sizeof(h->user), h->users[i]);
            }
        }
    }
    char icon[64], concept[64], group[64];
    snprintf(icon, sizeof(icon), "tbl_iconinfo_%s", h->user);
    if (h->user[0] == 0 || !appdb_has_column(db, icon, "titleId") || !appdb_has_column(db, icon, "dispLocation") ||
        !appdb_has_column(db, icon, "visible")) {
        snprintf(err, errlen, "This console's home-screen database keeps its layout in a way Art Swap does not know");
        return false;
    }
    snprintf(concept, sizeof(concept), "tbl_concepticoninfo_%s", h->user);
    snprintf(group, sizeof(group), "tbl_group_%s", h->user);
    const bool concepts = table_exists(db, concept) && appdb_has_column(db, concept, "conceptName") &&
                          appdb_has_column(db, icon, "localConceptId");
    const bool groups = table_exists(db, group);
    if ((h->has_version = table_exists(db, "tbl_version"))) {
        h->access_index = version(db, "access_index");
        h->played_index = version(db, "played_index");
    }

    /* the columns a firmware may not have read as their plainest value */
    char sql[1024];
    size_t used = (size_t)snprintf(sql, sizeof(sql),
                                   "SELECT i.titleId, %s, %s, i.dispLocation, i.visible, %s, %s, %s, %s FROM \"%s\" i ",
                                   appdb_has_column(db, icon, "titleName") ? "i.titleName" : "''",
                                   appdb_has_column(db, icon, "localConceptId") ? "i.localConceptId" : "''",
                                   appdb_has_column(db, icon, "lastAccessIndex") ? "i.lastAccessIndex" : "0",
                                   appdb_has_column(db, icon, "installStatus") ? "i.installStatus" : "2",
                                   concepts ? "c.conceptName" : "''", groups ? "g.idx" : "NULL", icon);
    if (concepts)
        used += (size_t)snprintf(sql + used, sizeof(sql) - used,
                                 "LEFT JOIN \"%s\" c ON c.localConceptId = i.localConceptId ", concept);
    if (groups)
        snprintf(sql + used, sizeof(sql) - used, "LEFT JOIN \"%s\" g ON g.itemId = i.titleId AND g.groupName = 'MyHome'",
                 group);
    h->tiles = calloc(TILES_MAX, sizeof(tile_t));
    if (h->tiles == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "The home screen's database could not be read (%s)", sqlite3_errmsg(db));
        return false;
    }
    while (sqlite3_step(st) == SQLITE_ROW && h->n < TILES_MAX) {
        const char *id = (const char *)sqlite3_column_text(st, 0);
        if (id == NULL || !plain_id(id) || internal(id) || find_tile(h, id) != NULL)
            continue;
        tile_t *t = &h->tiles[h->n];
        memset(t, 0, sizeof(*t));
        copy_str(t->id, sizeof(t->id), id);
        const char *title_name = (const char *)sqlite3_column_text(st, 1);
        const char *local = (const char *)sqlite3_column_text(st, 2);
        const char *concept_name = (const char *)sqlite3_column_text(st, 7);
        copy_str(t->name, sizeof(t->name), concept_name && concept_name[0] ? concept_name : title_name ? title_name : "");
        copy_str(t->concept, sizeof(t->concept), local ? local : "");
        t->disp = sqlite3_column_int64(st, 3);
        t->visible = sqlite3_column_int64(st, 4) == 1;
        t->access = sqlite3_column_int64(st, 5);
        t->install = sqlite3_column_int64(st, 6);
        t->system = strncmp(id, "NPXS", 4) == 0;
        /* the console honours MyHome only for its own tiles */
        t->home = t->system && sqlite3_column_type(st, 8) != SQLITE_NULL;
        t->home_idx = t->home ? sqlite3_column_int64(st, 8) : 0;
        if (t->home)
            t->now = "games";
        else if (!t->visible)
            t->now = "hidden";
        else if (t->disp & BIT_GAMES)
            t->now = "games";
        else if (t->disp & BIT_MEDIA)
            t->now = "media";
        else
            t->now = "hidden";
        if (strcmp(t->now, "hidden") == 0 && t->install != 2)
            continue; /* a leftover of something uninstalled */
        h->n++;
    }
    sqlite3_finalize(st);
    return true;
}

static tile_t *find_tile(home_t *h, const char *id)
{
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->tiles[i].id, id) == 0)
            return &h->tiles[i];
    return NULL;
}

static place_t *find_place(state_t *s, const char *id)
{
    for (int i = 0; i < s->nplaces; i++)
        if (strcmp(s->places[i].id, id) == 0)
            return &s->places[i];
    return NULL;
}

/* Where the person wants the tile: their wish, or where it is */
static const char *wished(state_t *s, const tile_t *t)
{
    const place_t *p = find_place(s, t->id);
    return p ? p->where : t->now;
}

static int by_recent(const void *a, const void *b)
{
    const tile_t *x = *(tile_t *const *)a, *y = *(tile_t *const *)b;
    if (x->access != y->access)
        return x->access > y->access ? -1 : 1;
    return strcmp(x->id, y->id);
}

static int by_home(const void *a, const void *b)
{
    const tile_t *x = *(tile_t *const *)a, *y = *(tile_t *const *)b;
    if (x->home != y->home)
        return x->home ? -1 : 1;
    if (x->home && x->home_idx != y->home_idx)
        return x->home_idx < y->home_idx ? -1 : 1;
    return by_recent(a, b);
}

/* The tiles that are not the system's: first those in the order wished, then the rest, the most recent first.
 * games_only: those the person wants on the Games row (the order written). Returns how many. */
static int game_order(state_t *s, home_t *h, bool games_only, tile_t **out)
{
    int n = 0;
    for (int i = 0; i < s->nseq; i++) {
        tile_t *t = find_tile(h, s->seq[i]);
        bool seen = false;
        for (int j = 0; j < n && !seen; j++)
            seen = out[j] == t;
        if (t && !t->system && !seen && (!games_only || strcmp(wished(s, t), "games") == 0))
            out[n++] = t;
    }
    const int named = n;
    for (int i = 0; i < h->n; i++) {
        tile_t *t = &h->tiles[i];
        bool seen = false;
        for (int j = 0; j < named && !seen; j++)
            seen = out[j] == t;
        if (!t->system && !seen && (!games_only || strcmp(wished(s, t), "games") == 0))
            out[n++] = t;
    }
    qsort(out + named, (size_t)(n - named), sizeof(out[0]), by_recent);
    return n;
}

static void tile_line(buf_t *out, state_t *s, const tile_t *t)
{
    buf_printf(out, "T\t%s\t%s\t%s\t%s\t", t->id, wished(s, t), t->now, t->system ? "system" : "game");
    buf_field(out, t->name);
    buf_printf(out, "\n");
}

/* The database opened and read; NULL with a sentence in err */
static sqlite3 *open_home(int flags, home_t *h, char *err, size_t errlen)
{
    sqlite3 *db = appdb_open(flags, err, errlen);
    if (db != NULL && !home_read(db, h, err, errlen)) {
        sqlite3_close(db);
        home_free(h);
        return NULL;
    }
    return db;
}

bool layout_list(buf_t *out, char *err, size_t errlen)
{
    state_t *s = malloc(sizeof(*s));
    tile_t **order = calloc(TILES_MAX, sizeof(tile_t *));
    home_t h;
    sqlite3 *db = s && order ? open_home(SQLITE_OPEN_READONLY, &h, err, errlen) : NULL;
    if (db == NULL) {
        if (!s || !order)
            snprintf(err, errlen, "out of memory");
        free(s);
        free(order);
        return false;
    }
    sqlite3_close(db);
    pthread_mutex_lock(&g_lock);
    state_load(s);
    pthread_mutex_unlock(&g_lock);
    buf_printf(out, "order\t%s\n", order_shown(s));
    const int games = game_order(s, &h, false, order);
    for (int i = 0; i < games; i++)
        tile_line(out, s, order[i]);
    int systems = 0;
    for (int i = 0; i < h.n; i++)
        if (h.tiles[i].system)
            order[systems++] = &h.tiles[i];
    qsort(order, (size_t)systems, sizeof(order[0]), by_home);
    for (int i = 0; i < systems; i++)
        tile_line(out, s, order[i]);
    home_free(&h);
    free(order);
    free(s);
    return true;
}

bool layout_place(const char *id, const char *where, buf_t *out, char *err, size_t errlen)
{
    if (!known_place(where)) {
        snprintf(err, errlen, "a tile goes on games, media or hidden");
        return false;
    }
    home_t h;
    sqlite3 *db = open_home(SQLITE_OPEN_READONLY, &h, err, errlen);
    if (db == NULL)
        return false;
    sqlite3_close(db);
    const tile_t *t = find_tile(&h, id);
    state_t *s = t ? malloc(sizeof(*s)) : NULL;
    if (s == NULL) {
        snprintf(err, errlen, t ? "out of memory" : "no such tile");
        home_free(&h);
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    place_t *p = find_place(s, id);
    if (p == NULL && s->nplaces < PLACES_MAX) {
        p = &s->places[s->nplaces++];
        copy_str(p->id, sizeof(p->id), id);
        copy_str(p->original, sizeof(p->original), t->now);
    }
    bool ok = p != NULL;
    if (ok) {
        copy_str(p->where, sizeof(p->where), where);
        /* back where it was, and the home screen shows it there: nothing left to keep */
        if (strcmp(p->where, p->original) == 0 && strcmp(t->now, where) == 0)
            *p = s->places[--s->nplaces];
        ok = state_save(s);
    }
    pthread_mutex_unlock(&g_lock);
    if (ok) {
        log_line("layout: %s (%s) wished on %s", id, t->name, where);
        tile_line(out, s, t);
    } else {
        snprintf(err, errlen, "The choice could not be kept");
    }
    free(s);
    home_free(&h);
    return ok;
}

bool layout_order(const char *mode, char *const *ids, int nids, buf_t *out, char *err, size_t errlen)
{
    const bool locked = strcmp(mode, "locked") == 0, free_order = strcmp(mode, "free") == 0;
    if (!locked && !free_order && strcmp(mode, "none") != 0) {
        snprintf(err, errlen, "the order is locked, free or none");
        return false;
    }
    state_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    const bool was_locked = strcmp(s->mode, "locked") == 0;
    if (nids > 0 && (locked || free_order || was_locked)) {
        s->nseq = 0;
        for (int i = 0; i < nids && s->nseq < SEQ_MAX; i++)
            if (plain_id(ids[i]))
                copy_str(s->seq[s->nseq++], sizeof(s->seq[0]), ids[i]);
    }
    if (locked || free_order) {
        copy_str(s->mode, sizeof(s->mode), mode);
        s->dirty = true;
    } else if (was_locked) {
        /* unlocked: written once more in the order it has, as now, so a game played next comes first again */
        copy_str(s->mode, sizeof(s->mode), "free");
        s->dirty = true;
    } else {
        copy_str(s->mode, sizeof(s->mode), "none");
        s->dirty = false;
        s->nseq = 0;
    }
    const bool ok = state_save(s);
    pthread_mutex_unlock(&g_lock);
    if (ok) {
        log_line("layout: order %s (%d named)", s->mode, s->nseq);
        buf_printf(out, "order\t%s\n", order_shown(s));
    } else {
        snprintf(err, errlen, "The choice could not be kept");
    }
    free(s);
    return ok;
}

bool layout_waiting(void)
{
    state_t *s = malloc(sizeof(*s));
    if (s == NULL)
        return false;
    pthread_mutex_lock(&g_lock);
    state_load(s);
    pthread_mutex_unlock(&g_lock);
    const bool any = waiting(s);
    free(s);
    return any;
}

/* The order the home screen shows is the one locked: each tile at its locked place */
static bool order_shown_locked(tile_t **order, int n)
{
    for (int i = 0; i < n; i++)
        if (order[i]->access != LOCK_INDEX - i)
            return false;
    return true;
}

void layout_pending(buf_t *out)
{
    state_t *s = malloc(sizeof(*s));
    if (s == NULL)
        return;
    pthread_mutex_lock(&g_lock);
    state_load(s);
    pthread_mutex_unlock(&g_lock);
    home_t h;
    char err[300];
    tile_t **order = waiting(s) ? calloc(TILES_MAX, sizeof(tile_t *)) : NULL;
    sqlite3 *db = order ? open_home(SQLITE_OPEN_READONLY, &h, err, sizeof(err)) : NULL;
    if (db != NULL) {
        sqlite3_close(db);
        for (int i = 0; i < s->nplaces; i++) {
            const tile_t *t = find_tile(&h, s->places[i].id);
            if (t && strcmp(t->now, s->places[i].where) != 0)
                buf_printf(out, "P\t%s\tlayout\n", t->id);
        }
        const int n = game_order(s, &h, true, order);
        if (s->dirty || (strcmp(s->mode, "locked") == 0 && !order_shown_locked(order, n)))
            buf_printf(out, "P\torder\tlayout\n");
        home_free(&h);
    }
    free(order);
    free(s);
}

/* ---- writing ---- */

static bool run(sqlite3 *db, const char *sql, const char *text, long long a, long long b, char *err, size_t errlen)
{
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        /* ?1 and ?2 numbers, ?3 the key, as far as the statement uses them */
        const int params = sqlite3_bind_parameter_count(st);
        if (params >= 1)
            sqlite3_bind_int64(st, 1, a);
        if (params >= 2)
            sqlite3_bind_int64(st, 2, b);
        if (params >= 3)
            sqlite3_bind_text(st, 3, text, -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(st);
        ok = rc == SQLITE_DONE || rc == SQLITE_ROW;
    }
    if (!ok)
        snprintf(err, errlen, "The home screen's database could not be changed (%s)", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return ok;
}

/* The tables the layout is written in, each with its key: the title, or the concept */
typedef struct {
    char table[64];
    bool by_concept;
    bool user_icon; /* tbl_iconinfo_<user>: visible too */
} target_t;

static int targets(sqlite3 *db, const home_t *h, target_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < h->nusers && n + 2 <= max; i++) {
        snprintf(out[n].table, sizeof(out[n].table), "tbl_iconinfo_%s", h->users[i]);
        out[n].by_concept = false;
        out[n].user_icon = true;
        n++;
        snprintf(out[n].table, sizeof(out[n].table), "tbl_concepticoninfo_%s", h->users[i]);
        out[n].by_concept = true;
        out[n].user_icon = false;
        if (table_exists(db, out[n].table) && appdb_has_column(db, out[n].table, "localConceptId"))
            n++;
    }
    if (n < max && table_exists(db, "tbl_contentinfo") && appdb_has_column(db, "tbl_contentinfo", "titleId")) {
        copy_str(out[n].table, sizeof(out[n].table), "tbl_contentinfo");
        out[n].by_concept = false;
        out[n].user_icon = false;
        n++;
    }
    if (n < max && table_exists(db, "tbl_conceptmetadata") &&
        appdb_has_column(db, "tbl_conceptmetadata", "localConceptId")) {
        copy_str(out[n].table, sizeof(out[n].table), "tbl_conceptmetadata");
        out[n].by_concept = true;
        out[n].user_icon = false;
        n++;
    }
    return n;
}

/* A user's MyHome group, in idx order, changed for the system's tiles moved: in on the Games row (before Game
 * Library), out otherwise. Rows of titles no longer on the console are kept where they are. */
static bool arrange_group(sqlite3 *db, const char *user, state_t *s, home_t *h, char *err, size_t errlen)
{
    char table[64];
    snprintf(table, sizeof(table), "tbl_group_%s", user);
    if (!table_exists(db, table))
        return true;
    char sql[200];
    snprintf(sql, sizeof(sql), "SELECT itemId, type FROM \"%s\" WHERE groupName = 'MyHome' ORDER BY idx", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "The home screen's database could not be read (%s)", sqlite3_errmsg(db));
        return false;
    }
    char items[64][16], types[64][16];
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < 64) {
        const char *id = (const char *)sqlite3_column_text(st, 0), *type = (const char *)sqlite3_column_text(st, 1);
        copy_str(items[n], sizeof(items[0]), id ? id : "");
        copy_str(types[n], sizeof(types[0]), type ? type : "title");
        n++;
    }
    sqlite3_finalize(st);
    bool changed = false;
    for (int i = 0; i < s->nplaces; i++) {
        const tile_t *t = find_tile(h, s->places[i].id);
        if (t == NULL || !t->system)
            continue;
        int at = -1;
        for (int j = 0; j < n && at < 0; j++)
            if (strcmp(items[j], t->id) == 0)
                at = j;
        const bool in = strcmp(s->places[i].where, "games") == 0;
        if (in && at < 0 && n < 64) {
            int library = n;
            for (int j = 0; j < n; j++)
                if (strcmp(items[j], LIBRARY) == 0)
                    library = j;
            memmove(items[library + 1], items[library], (size_t)(n - library) * sizeof(items[0]));
            memmove(types[library + 1], types[library], (size_t)(n - library) * sizeof(types[0]));
            copy_str(items[library], sizeof(items[0]), t->id);
            copy_str(types[library], sizeof(types[0]), "title");
            n++;
            changed = true;
        } else if (!in && at >= 0) {
            memmove(items[at], items[at + 1], (size_t)(n - at - 1) * sizeof(items[0]));
            memmove(types[at], types[at + 1], (size_t)(n - at - 1) * sizeof(types[0]));
            n--;
            changed = true;
        }
    }
    if (!changed)
        return true;
    snprintf(sql, sizeof(sql), "DELETE FROM \"%s\" WHERE groupName = 'MyHome'", table);
    bool ok = run(db, sql, NULL, 0, 0, err, errlen);
    const bool dated = appdb_has_column(db, table, "lastModifiedDate");
    snprintf(sql, sizeof(sql),
             dated ? "INSERT INTO \"%s\" (itemId, groupName, type, idx, lastModifiedDate) VALUES (?1, 'MyHome', ?2, ?3, NULL)"
                   : "INSERT INTO \"%s\" (itemId, groupName, type, idx) VALUES (?1, 'MyHome', ?2, ?3)",
             table);
    for (int i = 0; ok && i < n; i++) {
        st = NULL;
        ok = sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(st, 1, items[i], -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, types[i], -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 3, i + 1);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        if (!ok)
            snprintf(err, errlen, "The home screen's database could not be changed (%s)", sqlite3_errmsg(db));
        sqlite3_finalize(st);
    }
    return ok;
}

/* A tile onto the row wished, in every table that keeps where it is */
static bool arrange_tile(sqlite3 *db, const target_t *tg, int ntg, const tile_t *t, const char *where, char *err,
                         size_t errlen)
{
    const bool hidden = strcmp(where, "hidden") == 0;
    const long long set = strcmp(where, "media") == 0 ? BIT_MEDIA : BIT_GAMES;
    const long long clear = set == BIT_MEDIA ? BIT_GAMES : BIT_MEDIA;
    bool ok = true;
    for (int i = 0; ok && i < ntg; i++) {
        const char *key = tg[i].by_concept ? t->concept : t->id;
        if (key[0] == 0)
            continue;
        char sql[300];
        if (!hidden && appdb_has_column(db, tg[i].table, "dispLocation")) {
            snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET dispLocation = ((dispLocation | ?1) & ~?2) WHERE %s = ?3",
                     tg[i].table, tg[i].by_concept ? "localConceptId" : "titleId");
            ok = run(db, sql, key, set, clear, err, errlen);
        }
        if (ok && tg[i].user_icon) {
            snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET visible = ?1 WHERE titleId = ?3", tg[i].table);
            ok = run(db, sql, key, hidden ? 0 : 1, 0, err, errlen);
        }
    }
    return ok;
}

static void sql_time(long long when, char *out, size_t size)
{
    const time_t t = (time_t)when;
    struct tm g;
    gmtime_r(&t, &g);
    strftime(out, size, "%Y-%m-%d %H:%M:%S.000", &g);
}

/* The Games row in this order: the recency of each tile, as PSGFX writes it */
static bool arrange_order(sqlite3 *db, const target_t *tg, int ntg, home_t *h, tile_t **order, int n, bool locked,
                          char *err, size_t errlen)
{
    static const char *always[] = {"lastAccessTime", "recentActivityDatePlayedOrInstalled", "lastInteractedTime"};
    static const char *played_only[] = {"lastPlayedDate", "lastPlayedDateOnConsole",
                                        "recentActivityDatePlayedOrPurchased"};
    const long long base = locked ? LOCK_TIME : (long long)time(NULL);
    const long long access = locked ? LOCK_INDEX : h->access_index + n;
    const long long played = locked ? LOCK_INDEX : h->played_index + n;
    bool ok = true;
    for (int k = 0; ok && k < ntg; k++) {
        /* the statement for this table: ?1 the index, ?2 the played index, ?3 the key, ?4 the time */
        char sql[1024];
        size_t used = (size_t)snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET ", tg[k].table);
        bool any = false;
        if (appdb_has_column(db, tg[k].table, "lastAccessIndex")) {
            used += (size_t)snprintf(sql + used, sizeof(sql) - used, "lastAccessIndex = ?1");
            any = true;
        }
        if (appdb_has_column(db, tg[k].table, "lastPlayedIndex")) {
            used += (size_t)snprintf(sql + used, sizeof(sql) - used,
                                     "%slastPlayedIndex = CASE WHEN lastPlayedIndex IS NULL THEN NULL ELSE ?2 END",
                                     any ? ", " : "");
            any = true;
        }
        for (size_t c = 0; c < sizeof(always) / sizeof(always[0]); c++)
            if (appdb_has_column(db, tg[k].table, always[c])) {
                used += (size_t)snprintf(sql + used, sizeof(sql) - used, "%s%s = ?4", any ? ", " : "", always[c]);
                any = true;
            }
        for (size_t c = 0; c < sizeof(played_only) / sizeof(played_only[0]); c++)
            if (appdb_has_column(db, tg[k].table, played_only[c])) {
                used += (size_t)snprintf(sql + used, sizeof(sql) - used, "%s%s = CASE WHEN %s IS NULL THEN NULL ELSE ?4 END",
                                         any ? ", " : "", played_only[c], played_only[c]);
                any = true;
            }
        if (!any)
            continue;
        snprintf(sql + used, sizeof(sql) - used, " WHERE %s = ?3", tg[k].by_concept ? "localConceptId" : "titleId");
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
            snprintf(err, errlen, "The home screen's database could not be changed (%s)", sqlite3_errmsg(db));
            return false;
        }
        for (int i = 0; ok && i < n; i++) {
            const char *key = tg[k].by_concept ? order[i]->concept : order[i]->id;
            if (key[0] == 0)
                continue;
            char when[40];
            sql_time(base - i * 60LL, when, sizeof(when));
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            const int params = sqlite3_bind_parameter_count(st);
            for (int p = 1; p <= params; p++) {
                const char *name = sqlite3_bind_parameter_name(st, p);
                if (name == NULL)
                    continue;
                if (strcmp(name, "?1") == 0)
                    sqlite3_bind_int64(st, p, access - i);
                else if (strcmp(name, "?2") == 0)
                    sqlite3_bind_int64(st, p, played - i);
                else if (strcmp(name, "?3") == 0)
                    sqlite3_bind_text(st, p, key, -1, SQLITE_TRANSIENT);
                else if (strcmp(name, "?4") == 0)
                    sqlite3_bind_text(st, p, when, -1, SQLITE_TRANSIENT);
            }
            ok = sqlite3_step(st) == SQLITE_DONE;
            if (!ok)
                snprintf(err, errlen, "The home screen's database could not be changed (%s)", sqlite3_errmsg(db));
        }
        sqlite3_finalize(st);
    }
    /* unlocked: the console's own counters go on from the new order */
    if (ok && !locked && h->has_version) {
        ok = run(db, "UPDATE tbl_version SET status = ?1 WHERE category = 'access_index'", NULL, access, 0, err, errlen) &&
             run(db, "UPDATE tbl_version SET status = ?1 WHERE category = 'played_index'", NULL, played, 0, err, errlen);
    }
    return ok;
}

bool layout_arrange(char *err, size_t errlen)
{
    state_t *s = malloc(sizeof(*s));
    tile_t **order = calloc(TILES_MAX, sizeof(tile_t *));
    if (s == NULL || order == NULL) {
        free(s);
        free(order);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    if (!waiting(s)) {
        pthread_mutex_unlock(&g_lock);
        free(s);
        free(order);
        return true;
    }
    home_t h;
    sqlite3 *db = open_home(SQLITE_OPEN_READWRITE, &h, err, errlen);
    bool ok = db != NULL && appdb_backup(db, err, errlen);
    ok = ok && run(db, "BEGIN IMMEDIATE", NULL, 0, 0, err, errlen);
    const bool began = ok;
    target_t tg[2 * USERS_MAX + 2];
    const int ntg = ok ? targets(db, &h, tg, (int)(sizeof(tg) / sizeof(tg[0]))) : 0;
    int moved = 0;
    for (int i = 0; ok && i < s->nplaces; i++) {
        const tile_t *t = find_tile(&h, s->places[i].id);
        if (t == NULL)
            continue; /* not on the console now: its wish waits for it */
        ok = arrange_tile(db, tg, ntg, t, s->places[i].where, err, errlen);
        moved += ok;
    }
    for (int i = 0; ok && i < h.nusers; i++)
        ok = arrange_group(db, h.users[i], s, &h, err, errlen);
    const bool locked = strcmp(s->mode, "locked") == 0;
    int n = 0;
    if (ok && (locked || s->dirty)) {
        n = game_order(s, &h, true, order);
        ok = arrange_order(db, tg, ntg, &h, order, n, locked, err, errlen);
    }
    /* checked before it is kept: a database the home screen cannot read is never left behind */
    if (ok) {
        sqlite3_stmt *st = NULL;
        const char *verdict = NULL;
        if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            verdict = (const char *)sqlite3_column_text(st, 0);
        ok = verdict != NULL && strcmp(verdict, "ok") == 0;
        if (!ok)
            snprintf(err, errlen, "The home screen's database did not pass its check after the change (%s), so nothing "
                                  "was changed",
                     verdict ? verdict : sqlite3_errmsg(db));
        sqlite3_finalize(st);
    }
    if (ok)
        ok = run(db, "COMMIT", NULL, 0, 0, err, errlen);
    if (!ok && began)
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (ok) {
        log_line("layout: %d tile(s) placed%s%s", moved, locked || s->dirty ? ", the order written" : "",
                 locked ? " (locked)" : "");
        /* written once: free goes back to none; a tile back where it began needs no wish kept */
        if (strcmp(s->mode, "free") == 0) {
            copy_str(s->mode, sizeof(s->mode), "none");
            s->nseq = 0;
        }
        s->dirty = false;
        for (int i = 0; i < s->nplaces;) {
            if (strcmp(s->places[i].where, s->places[i].original) == 0)
                s->places[i] = s->places[--s->nplaces];
            else
                i++;
        }
        state_save(s);
    } else {
        log_line("layout: not arranged: %s", err);
    }
    pthread_mutex_unlock(&g_lock);
    if (db != NULL) {
        sqlite3_close(db);
        home_free(&h);
    }
    free(s);
    free(order);
    return ok;
}
