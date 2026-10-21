/*
 * Art Swap helper - the key page, for adding a SteamGridDB key from a phone (phone.c).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

/* Opens the key page (port 9118) for ten minutes, for this four-digit code */
bool phone_start(const char *code, char *err, size_t errlen);
/* Closes it */
void phone_stop(void);
int phone_port(void);
/* The console's address on its network ("" when it has none) */
const char *phone_lan_address(char *out, size_t size);
