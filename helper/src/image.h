/*
 * Art Swap helper - pictures: reading the console's art and SteamGridDB's,
 * scaling, framing, and writing in the form the file being replaced has.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "artswap.h"

typedef struct {
    int w, h;
    uint8_t *rgba; /* w * h * 4, straight alpha, top row first */
} image_t;

typedef enum { CONTAINER_UNKNOWN, CONTAINER_PNG, CONTAINER_JPEG, CONTAINER_DDS } container_t;
typedef enum { DDS_NONE, DDS_BC1, DDS_BC3, DDS_BC7, DDS_RGBA8, DDS_BGRA8 } dds_kind_t;

/* What a picture file is: enough to write a new one the console reads the same way */
typedef struct {
    container_t container;
    int w, h;
    int channels;      /* PNG: 3 or 4 (what the file had) */
    dds_kind_t dds;    /* DDS: the block or pixel format */
    int mips;          /* DDS: levels stored */
    size_t header_len; /* DDS: 128, or 148 with the DX10 header */
} art_format_t;

const char *format_name(const art_format_t *f); /* "png", "jpeg", "dds-bc7"... */

bool image_alloc(image_t *img, int w, int h);
void image_free(image_t *img);

/* What the bytes are; false when they are not a picture this helper knows */
bool image_probe(const uint8_t *data, size_t len, art_format_t *out);
/* PNG, JPEG (and the rest stb_image reads) or DDS (BC1, BC3, BC7, RGBA8, BGRA8; the first level) */
bool image_decode(const uint8_t *data, size_t len, image_t *out, char *err, size_t errlen);

/* The region (x, y, w, h, in source pixels) scaled to out_w x out_h (Lanczos 3) */
bool image_resample(const image_t *src, float x, float y, float w, float h, int out_w, int out_h,
                    image_t *out);
/* Scaled to fill w x h, the overflow cut; pan -1..1 slides the cut (0 centres it) */
bool image_cover(const image_t *src, int w, int h, float pan_x, float pan_y, image_t *out);
/* All of src inside w x h, over a blurred, darkened fill made from itself
 * (transparent pictures, such as icons, over a dark plate of their own colour) */
bool image_fit_blur(const image_t *src, int w, int h, image_t *out);
/* Scaled down (never up) to fit max_w x max_h, aspect kept */
bool image_shrink(const image_t *src, int max_w, int max_h, image_t *out);
bool image_has_alpha(const image_t *img);

/* img (already the right size) written as like is: the container, the
 * channels, the DDS format and levels, the DDS header copied from like_data */
bool image_encode_like(const image_t *img, const art_format_t *like, const uint8_t *like_data,
                       size_t like_len, buf_t *out, char *err, size_t errlen);

/* BC7 mode 6 blocks for a picture whose sides are multiples of 4 (bc7enc.c) */
void bc7_encode_image(const image_t *img, uint8_t *blocks);
