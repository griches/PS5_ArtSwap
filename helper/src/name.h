/*
 * Art Swap helper - a title's name, in its param.json.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "scan.h"

/* Writes name as the title's titleName, in every language, into each param.json
 * beside its art (the home screen's /user/appmeta/<id> and the folder's
 * sce_sys), keeping each original once as param.json.old. The home screen shows
 * it once the title is installed again (smp.c). report gets "O" lines for
 * originals kept and "W" lines for files written. */
bool name_change(const app_t *app, const char *name, buf_t *report, char *err, size_t errlen);
/* The kept param.json.old files back; report gets "R" lines. The number put back, -1 when one failed. */
int name_restore(const app_t *app, buf_t *report);
/* An original param.json is kept */
bool name_kept(const app_t *app);

/* The console's languages, numbered as the home screen's database numbers them (TITLE_01 is US English) */
#define NAME_LANGUAGES 31
/* A title's names as its param.json gives them: its default language's, and each language's it names ("" else) */
typedef struct {
    char def[256];
    char lang[NAME_LANGUAGES][256];
} names_t;
/* The title's names, read from its param.json beside its art */
bool name_read(const app_t *app, names_t *out);
