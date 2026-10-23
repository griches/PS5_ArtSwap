/*
 * Art Swap helper - the home screen's database, /system_data/priv/mms/app.db.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "name.h"

#include "../third_party/sqlite3.h"

/* The database, opened with SQLite's flags, waiting up to 10 s for the home screen's locks */
sqlite3 *appdb_open(int flags, char *err, size_t errlen);
/* Before this run's first change: a copy of the database as it was (app.db.backup), and the first copy ever
 * (app.db.first), kept for good */
bool appdb_backup(sqlite3 *db, char *err, size_t errlen);

/* What the home screen has of a title: the name it shows, and the background and music its entry names
 * (each "" for none; art_known is false where the firmware's table has no such columns) */
typedef struct {
    char name[256];
    char pic0[400];
    char snd0[400];
    bool art_known;
} appdb_title_t;
/* 1 with the title's entry in out, 0 when the home screen has none, -1 on an error (err) */
int appdb_title(const char *id, appdb_title_t *out, char *err, size_t errlen);
/* Writes the names as the title's name everywhere the home screen keeps it, at once, so they show after a
 * restart of the home screen with no install. Called with g_apply_lock held. */
bool appdb_rename(const char *id, const names_t *names, char *err, size_t errlen);
