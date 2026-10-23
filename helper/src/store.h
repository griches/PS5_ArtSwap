/*
 * Art Swap helper - a store game's own logo, from PlayStation Store's public catalog.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "scan.h"

/* An address on Sony's image server, where PlayStation Store's pictures are */
bool store_picture(const char *url);
/* The title's logo on PlayStation Store, as LIST's lines: P, then an I line, or none when the store has none */
bool store_logo(const app_t *app, buf_t *out, char *err, size_t errlen);
