/*
 * Art Swap helper - the titles on the console and where their art is.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "image.h"

#define APP_MAX_FILES 24 /* /user/appmeta, the installed copy and the homebrew folder: up to 6 each */

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
    /* from the home screen's database (app.db): */
    bool listed;  /* it has an entry there, so a logo can be set */
    char home_name[200]; /* the name the home screen shows: an install's, which a rename waits for */
    long concept; /* its PlayStation Store concept; 0 for homebrew, whose concept is its own number */
    bool logo;    /* a logo shows on its page (logoImageInfo), Art Swap's or not */
    long recent;  /* its place in the home screen's own order (lastAccessIndex): higher was used more recently */
} app_t;

/* Every title with art; the list is sorted by name. Free with free(). */
bool scan_apps(app_t **apps, int *count);
/* The titles' SteamGridDB games (/data/artswap/matches.tsv) */
void matches_apply(app_t *apps, int count);
bool matches_set(const char *id, long game, const char *name);

/* The files of one kind: "icon" (icon0) or "pic" (pic0 and pic1) */
bool slot_matches(const art_file_t *file, const char *slot);
