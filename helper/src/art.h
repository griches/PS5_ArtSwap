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

/* A background for a title that has none: pic0.dds (3840x2160 BC7) in its folder's sce_sys (folder: the game's
 * folder, as ShadowMountPlus names it), which an install then copies for the home screen. report gets a "W" line. */
bool art_add_background(const app_t *app, const char *folder, const char *url, const char *mode, float pan_x,
                        float pan_y, float zoom, buf_t *report, char *err, size_t errlen);
/* Art Swap added the title's background ("pic0") or music ("snd0") (/data/artswap/added.txt): there is no
 * original to keep */
bool art_added(const char *id, const char *what);
/* One more file of something Art Swap added, listed once */
void art_added_note(const char *id, const char *file);
/* The files of the background or music Art Swap added, deleted, and the install's copies of them; how many, -1
 * when one could not be. report gets "R" lines. */
int art_remove_added(const char *id, const char *what, buf_t *report);
