/*
 * Art Swap helper - the home screen's tiles in its databases: moved between the Games and Media tabs, hidden on a
 * profile, ordered and pinned in a row, the system's own tiles shown, and kept that way (tiles.c).
 *
 * Every write here runs inside a transaction the caller opened on app.db, with appinfo.db attached as info
 * (tiles_attach), but tiles_startup, which opens its own.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

#include "../third_party/sqlite3.h"

#define TILES_MAX 512
#define TILES_USERS 16
#define GAMES_TAB 138
#define MEDIA_TAB 188

/* What can be done to a tile (tile_t.caps) */
#define TILE_MOVE 1u  /* between the Games and Media tabs, on every profile */
#define TILE_HIDE 2u  /* hidden on this profile, and brought back */
#define TILE_SHOW 4u  /* a tile the system hides, shown on every profile (a test), and hidden again */
#define TILE_ORDER 8u /* ordered and pinned in its row */

typedef struct {
    char id[16];
    char name[256];
    int tab;             /* GAMES_TAB, MEDIA_TAB, or 0 on neither */
    bool visible;        /* on this profile */
    bool system;         /* the system's own (NPXS) */
    bool shown;          /* one the system hides, shown by Art Swap */
    bool hidden_by_us;   /* hidden by Art Swap on this profile */
    bool row;            /* on its tab's row, ordered with the apps */
    int pin;             /* its place pinned in the row, or -1 */
    int priority;        /* sortPriority: the system's hubs before the apps, the libraries after */
    long long index;     /* lastAccessIndex: the row's order, largest first */
    unsigned caps;
} tile_t;

typedef struct {
    char user[16]; /* the tables' suffix, ten digits */
    char name[64];
} tiles_profile_t;

/* The profiles of tbl_iconinfo_<user>, or -1 */
int tiles_users(sqlite3 *db, char users[][16], int max);
/* The profiles with their names (/user/home/<hex>/username.dat), or -1 */
int tiles_profiles(sqlite3 *db, tiles_profile_t *out, int max);
/* Whether `user` is ten digits and has its tables */
bool tiles_user_known(sqlite3 *db, const char *user);
/* Lineup's tables or triggers are in app.db or appinfo.db: Art Swap leaves the tiles alone, or they would fight */
bool tiles_conflict(sqlite3 *db);
#define TILES_CONFLICT                                                                                                \
    "Lineup's changes are in the home screen's database, and they would undo Art Swap's. Open Lineup, then Options, " \
    "More options, Remove pins from every profile, and come back."

/* appinfo.db attached as info, checked; false with a sentence in err */
bool tiles_attach(sqlite3 *db, char *err, size_t errlen);
/* Both databases copied aside before the first change of this run (appdb_backup, and appinfo.db beside it) */
bool tiles_backup(sqlite3 *db, char *err, size_t errlen);

/* The tiles of `user`, as the home screen orders them: the Games tab, then Media, then those hidden; -1 with err */
int tiles_read(sqlite3 *db, const char *user, tile_t *out, int max, char *err, size_t errlen);

/* ---- writes, inside the caller's transaction: 0 done, 1 skipped (why in msg), -1 failed (msg) ---- */
int tiles_move(sqlite3 *db, const char *id, int tab, char *msg, size_t msglen);
int tiles_hide(sqlite3 *db, const char *user, const char *id, bool hide, char *msg, size_t msglen);
int tiles_show(sqlite3 *db, const char *id, bool show, char *msg, size_t msglen);
/* ids in the order wished, pins[i] to pin it at its place; tiles not named follow, the most recent first */
int tiles_order(sqlite3 *db, const char *user, int tab, char ids[][16], const bool *pins, int n, char *msg,
                size_t msglen);
/* TabShifter's TBSH00001 tile, from both databases: the rows removed in *removed */
int tiles_tabshifter(sqlite3 *db, int *removed, char *msg, size_t msglen);
/* After the transaction: TabShifter's folders */
void tiles_tabshifter_folders(void);
/* The home screen put back as the system has it, whatever moved, hid or showed a tile (Art Swap, Lineup, Art Swap
 * 1.4): Art Swap's tables and triggers removed, installed homebrew shown, the system's tiles on their own tab and
 * shown or hidden as the system has them, each app on the tab its param.json names */
int tiles_reset(sqlite3 *db, char *msg, size_t msglen);
/* The apps on a tab other than the one their param.json names that Art Swap does not keep yet (another tool moved
 * them): counted (create false), or kept there as Art Swap's own moves (create true). How many, or -1. */
int tiles_adopt(sqlite3 *db, bool create);
/* What Art Swap 1.4 and older left: an order locked far in the future, and the 140 and 186 it wrote for an app it
 * moved. *changed counts what was put right. */
int tiles_normalize(sqlite3 *db, int *changed, char *msg, size_t msglen);

/* When the helper starts: puts back, after a backup, what the triggers could not keep (a profile made since, the
 * JSON, a database rebuilt without them). Opens its own transaction. */
void tiles_startup(void);
