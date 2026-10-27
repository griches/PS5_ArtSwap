/*
 * Art Swap helper - the person's own pictures, sent from a phone or a computer (pictures.c).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

/* The most a picture of the person's may be */
#define PICTURE_MAX_BYTES ((size_t)40 << 20)
/* A picture URL that names one of them: mine:<name> */
#define PICTURE_SCHEME "mine:"

/* Where they are: /data/artswap/pictures, made if need be */
const char *pictures_folder(char *out, size_t size);
/* "F name bytes w h alpha" a line: the PNG and JPEG pictures in the folder, newest first */
void pictures_list(buf_t *out);
/* A name the folder may hold: 1-200 bytes, no folders, not hidden */
bool pictures_name_ok(const char *name);
/* The picture's bytes; false, with a sentence in err, for a name that is not one */
bool pictures_read(const char *name, buf_t *out, char *err, size_t errlen);
bool pictures_delete(const char *name, char *err, size_t errlen);
/* A file name for an upload, safe to keep in the folder (.png, .jpg or .jpeg), or false */
bool pictures_upload_name(const char *wanted, char *out, size_t size);
/* Whether the file (a path as opened) begins as a PNG or a JPEG does: its size, and whether a PNG can be
 * transparent (colour type 4 or 6, or a tRNS chunk) */
bool pictures_probe(const char *path, int *w, int *h, bool *alpha);
