/*
 * Art Swap helper - a logo on a title's home-screen page.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "scan.h"

/* The picture at url (SteamGridDB's, or PlayStation Store's) as the logo on the title's page, at once.
 * Text: W lines, as APPLY's. Called with g_apply_lock held. */
bool logo_set(const app_t *app, const char *url, buf_t *report, char *err, size_t errlen);
/* Art Swap's logo taken away, and what the page had before put back (most often nothing).
 * Text: R lines. Called with g_apply_lock held. */
bool logo_restore(const app_t *app, buf_t *report, char *err, size_t errlen);
/* Art Swap chose a logo for the title (/data/artswap/logos/<id>.png) */
bool logo_kept(const char *id);
/* Puts back each logo of Art Swap's the home screen has lost: an install writes a title's entry afresh,
 * without it. Only id's when id is not NULL, waiting up to wait_s seconds for its entry to come back.
 * How many were put back. Takes g_apply_lock. */
int logo_keep(const char *id, int wait_s);
/* Art Swap's own logo (helper/res/artswap-logo.png) on Art Swap's page, once: after that it is a logo of Art
 * Swap's like any other, kept through installs, and a Restore takes it away for good. Nothing when the page
 * has a logo already. Whether it was put there. Takes g_apply_lock. */
bool logo_own(void);
/* The logo the title's page shows now, scaled down to fit */
bool logo_current(const app_t *app, int max_w, int max_h, image_t *out, char *err, size_t errlen);
