/*
 * Art Swap helper - Apply changes: the home screen restarted, to show every change at once.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "scan.h"

#include <sys/types.h>

/* A title whose art changed since the home screen last restarted (/data/artswap/pending.txt) */
void home_note(const char *id);
/* A title renamed since, or with a background or music added or taken away: Apply changes writes its new name
 * into the home screen's database, or installs it again for the added art, first */
void home_note_name(const char *id);
/* Those titles waiting: up to max ids, how many */
int home_renamed(char (*ids)[16], int max);
/* It shows already (ShadowMountPlus installed it again) */
void home_forget(const char *id);
/* "P id kind" a line, each title changed since the last restart: kind art (a restart shows it), name (its new
 * name is written into the home screen's database first) or install (installed again first, for added art) */
void home_pending(buf_t *out);

/* Restarts the home screen in the background, once the process title_pid (Art Swap, closing) has ended:
 * first the renamed titles get their new names (appdb.c), or are installed again for added art (smp.c), then
 * the system starts SceShellUI again, and the new one reads every game's art. False, with a sentence in err, when one is already under way. */
bool home_restart_start(pid_t title_pid, const app_t *renamed, int nrenamed, char *err, size_t errlen);
bool home_running(void);
/* "home <phase> <unseen> <message>": phase none, waiting, renaming, installing, restarting, done or failed. The end is kept in
 * /data/artswap/home.txt, unseen until home_seen(): Art Swap has closed by then. */
void home_line(buf_t *out);
void home_seen(void);
