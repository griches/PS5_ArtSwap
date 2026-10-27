/*
 * Art Swap helper - the phone page, for adding a SteamGridDB key, or sending music or pictures from a phone or a
 * computer (phone.c).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

/* What the page is for */
typedef enum {
    PHONE_KEY,      /* the SteamGridDB key */
    PHONE_MUSIC,    /* music files, to /data/artswap/music */
    PHONE_PICTURES, /* pictures, to /data/artswap/pictures */
} phone_mode_t;

/* Opens the phone page (port 9118) for ten minutes, for this four-digit code */
bool phone_start(const char *code, phone_mode_t mode, char *err, size_t errlen);
/* Closes it */
void phone_stop(void);
int phone_port(void);
/* The console's address on its network ("" when it has none) */
const char *phone_lan_address(char *out, size_t size);
