/*
 * Art Swap helper - the home screen's layout: the tab each tile is on (Games or Media), the tiles hidden on a
 * profile, a row's order and the tiles pinned in it, and the system's tiles shown (layout.c, written into the
 * databases by tiles.c).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

/* "U user name" each profile (protocol.md, PROFILES) */
bool layout_profiles(buf_t *out, char *err, size_t errlen);
/* The layout of a profile (empty: the first), with the changes waiting: protocol.md, LAYOUT */
bool layout_list(const char *user, buf_t *out, char *err, size_t errlen);
/* Changes kept for Apply changes to write (protocol.md): TAB, HIDE, SHOW, ROW */
bool layout_tab(const char *user, const char *id, const char *where, char *err, size_t errlen);
bool layout_hide(const char *user, const char *id, bool hide, char *err, size_t errlen);
bool layout_show(const char *user, const char *id, bool show, char *err, size_t errlen);
bool layout_row(const char *user, const char *tab, char *const *ids, int nids, char *err, size_t errlen);
/* TABSHIFTER and RESET: queued for Apply changes (on), or not */
bool layout_queue(const char *what, bool on, char *err, size_t errlen);
/* "P id layout" for each tile with a change waiting, "P order layout" while an order waits, "P tabshifter layout"
 * and "P reset layout" */
void layout_pending(buf_t *out);
/* Something for Apply changes to write */
bool layout_waiting(void);
/* Apply changes: every change written into the home screen's databases, in one transaction, before the home
 * screen restarts. Called with g_apply_lock held. False, with a sentence in err, when nothing could be written. */
bool layout_arrange(char *err, size_t errlen);
/* When the helper starts: Art Swap 1.4's layout.txt taken over, and what the console undid put back (tiles.c) */
void layout_startup(void);
