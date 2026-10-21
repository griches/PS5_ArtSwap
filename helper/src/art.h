/*
 * Art Swap helper - changing a title's art, and putting the original back.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "scan.h"

/* The art a slot ("icon" or "pic") shows now, scaled down to fit max_w x max_h */
bool art_current(const app_t *app, const char *slot, int max_w, int max_h, image_t *out, char *err,
                 size_t errlen);
/* The picture made from src for a w x h file of the slot. mode "cover" fills
 * the frame: the part kept is the largest of the frame's shape, made zoom
 * (1..8) times smaller, and pan -1..1 places it across and down the rest;
 * "fit" shows it whole over a blurred fill. */
bool art_compose(const image_t *src, const char *slot, const char *mode, float pan_x, float pan_y, float zoom, int w,
                 int h, image_t *out);
/* The SteamGridDB picture at url into every file of the slot, made by
 * art_compose. report gets "O" lines for originals kept and "W" lines for
 * files written. */
bool art_apply(const app_t *app, const char *slot, const char *url, const char *mode, float pan_x, float pan_y,
               float zoom, buf_t *report, char *err, size_t errlen);
/* The kept originals back; slot "icon", "pic" or "all". report gets "R" lines. */
bool art_restore(const app_t *app, const char *slot, buf_t *report, char *err, size_t errlen);
