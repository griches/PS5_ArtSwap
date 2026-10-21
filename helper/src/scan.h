/*
 * Art Swap helper - the titles on the console and where their art is.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "image.h"

#define APP_MAX_FILES 16

typedef struct {
    char path[400]; /* as the console names it (cpath() before opening) */
    char slot[8];   /* icon0, pic0 or pic1 */
    art_format_t fmt;
    bool has_old;   /* the original is kept beside it as <path>.old */
} art_file_t;

typedef struct {
    char id[16];
    char name[200];
    char kind[12]; /* ps5, ps4 or homebrew */
    art_file_t files[APP_MAX_FILES];
    int nfiles;
    long sgdb_id; /* the SteamGridDB game chosen for it, 0 for none yet */
    char sgdb_name[200];
} app_t;

/* Every title with art; the list is sorted by name. Free with free(). */
bool scan_apps(app_t **apps, int *count);
/* The titles' SteamGridDB games (/data/artswap/matches.tsv) */
void matches_apply(app_t *apps, int count);
bool matches_set(const char *id, long game, const char *name);

/* The files of one kind: "icon" (icon0) or "pic" (pic0 and pic1) */
bool slot_matches(const art_file_t *file, const char *slot);
