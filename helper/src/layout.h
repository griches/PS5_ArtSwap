/*
 * Art Swap helper - the home screen's layout: the row each tile is on (Games, Media, or hidden), and a locked
 * order of the Games row (layout.c).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

/* "order mode", then "T id where now kind name" a tile (protocol.md, LAYOUT) */
bool layout_list(buf_t *out, char *err, size_t errlen);
/* Keeps the person's wish for a tile (games, media or hidden), for Apply changes to write; out gets its T line */
bool layout_place(const char *id, const char *where, buf_t *out, char *err, size_t errlen);
/* The order of the Games row: locked (kept at every Apply changes), free (written once) or none (left alone);
 * ids: the order wished, those not named following in their present order. out gets the order line. */
bool layout_order(const char *mode, char *const *ids, int nids, buf_t *out, char *err, size_t errlen);
/* "P id layout" for each tile whose wish the home screen does not show yet, and "P order layout" while an order
 * waits */
void layout_pending(buf_t *out);
/* Something for Apply changes to write */
bool layout_waiting(void);
/* Apply changes: every wish written into the home screen's database, in one transaction, before the home screen
 * restarts. Called with g_apply_lock held. False, with a sentence in err, when nothing could be written. */
bool layout_arrange(char *err, size_t errlen);
