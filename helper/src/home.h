/*
 * Art Swap helper - Apply changes: the home screen restarted, to show every change at once.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

#include <sys/types.h>

/* A title whose art or name changed since the home screen last restarted (/data/artswap/pending.txt) */
void home_note(const char *id);
/* It shows already (ShadowMountPlus installed it again) */
void home_forget(const char *id);
/* "P id" a line, each title changed since the last restart */
void home_pending(buf_t *out);

/* Restarts the home screen in the background, once the process title_pid (Art Swap, closing) has ended:
 * the system starts SceShellUI again, and the new one reads every game's art. False, with a sentence in
 * err, when one is already under way. */
bool home_restart_start(pid_t title_pid, char *err, size_t errlen);
bool home_running(void);
/* "home <phase> <unseen> <message>": phase none, waiting, restarting, done or failed. The end is kept in
 * /data/artswap/home.txt, unseen until home_seen(): Art Swap has closed by then. */
void home_line(buf_t *out);
void home_seen(void);
