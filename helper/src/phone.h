/*
 * Art Swap helper - the phone page, for adding a SteamGridDB key or sending music from a phone (phone.c).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

/* Opens the phone page (port 9118) for ten minutes, for this four-digit code: the key page, or with music the
 * page that sends music files to /data/artswap/music */
bool phone_start(const char *code, bool music, char *err, size_t errlen);
/* Closes it */
void phone_stop(void);
int phone_port(void);
/* The console's address on its network ("" when it has none) */
const char *phone_lan_address(char *out, size_t size);
