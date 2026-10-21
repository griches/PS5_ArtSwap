/*
 * Art Swap helper - pictures (image.h).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "image.h"

#include "../third_party/bcdec.h"
#include "../third_party/stb_dxt.h"
#include "../third_party/stb_image.h"
#include "../third_party/stb_image_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- basics ---- */

bool image_alloc(image_t *img, int w, int h)
{
    img->w = w;
    img->h = h;
    img->rgba = (w > 0 && h > 0) ? calloc((size_t)w * (size_t)h, 4) : NULL;
    return img->rgba != NULL;
}

void image_free(image_t *img)
{
    free(img->rgba);
    img->rgba = NULL;
    img->w = img->h = 0;
}

const char *format_name(const art_format_t *f)
{
    switch (f->container) {
    case CONTAINER_PNG:
        return "png";
    case CONTAINER_JPEG:
        return "jpeg";
    case CONTAINER_DDS:
        switch (f->dds) {
        case DDS_BC1:
            return "dds-bc1";
        case DDS_BC3:
            return "dds-bc3";
        case DDS_BC7:
            return "dds-bc7";
        case DDS_RGBA8:
            return "dds-rgba8";
        case DDS_BGRA8:
            return "dds-bgra8";
        default:
            return "dds";
        }
    default:
        return "unknown";
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* ---- what a file is ---- */

static bool probe_dds(const uint8_t *data, size_t len, art_format_t *out)
{
    if (len < 128 || memcmp(data, "DDS ", 4) != 0 || le32(data + 4) != 124)
        return false;
    out->container = CONTAINER_DDS;
    out->h = (int)le32(data + 12);
    out->w = (int)le32(data + 16);
    const uint32_t flags = le32(data + 8);
    const uint32_t mips = le32(data + 28);
    out->mips = (flags & 0x20000) && mips > 1 ? (int)mips : 1;
    out->header_len = 128;
    const uint32_t pf_flags = le32(data + 80);
    const uint8_t *fourcc = data + 84;
    out->dds = DDS_NONE;
    if (pf_flags & 0x4) {
        if (memcmp(fourcc, "DXT1", 4) == 0)
            out->dds = DDS_BC1;
        else if (memcmp(fourcc, "DXT5", 4) == 0)
            out->dds = DDS_BC3;
        else if (memcmp(fourcc, "DX10", 4) == 0) {
            if (len < 148)
                return false;
            out->header_len = 148;
            const uint32_t array_size = le32(data + 140);
            if (array_size > 1)
                return false; /* arrays are not home-screen art */
            switch (le32(data + 128)) {
            case 71:
            case 72:
                out->dds = DDS_BC1;
                break;
            case 77:
            case 78:
                out->dds = DDS_BC3;
                break;
            case 98:
            case 99:
                out->dds = DDS_BC7;
                break;
            case 28:
            case 29:
                out->dds = DDS_RGBA8;
                break;
            case 87:
            case 91:
                out->dds = DDS_BGRA8;
                break;
            default:
                break;
            }
        }
    } else if ((pf_flags & 0x40) && le32(data + 88) == 32) {
        out->dds = le32(data + 92) == 0x000000ff ? DDS_RGBA8 : DDS_BGRA8;
    }
    return out->dds != DDS_NONE && out->w > 0 && out->h > 0;
}

bool image_probe(const uint8_t *data, size_t len, art_format_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len >= 33 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(data + 12, "IHDR", 4) == 0) {
        out->container = CONTAINER_PNG;
        out->w = (int)be32(data + 16);
        out->h = (int)be32(data + 20);
        const uint8_t colour = data[25];
        out->channels = (colour == 2 || colour == 0) ? 3 : 4;
        return out->w > 0 && out->h > 0;
    }
    if (len >= 4 && data[0] == 0xff && data[1] == 0xd8) {
        int w, h, n;
        if (!stbi_info_from_memory(data, (int)len, &w, &h, &n))
            return false;
        out->container = CONTAINER_JPEG;
        out->w = w;
        out->h = h;
        out->channels = 3;
        return true;
    }
    return probe_dds(data, len, out);
}

/* ---- reading ---- */

static size_t dds_level_size(dds_kind_t kind, int w, int h)
{
    const size_t bw = (size_t)((w + 3) / 4), bh = (size_t)((h + 3) / 4);
    switch (kind) {
    case DDS_BC1:
        return bw * bh * 8;
    case DDS_BC3:
    case DDS_BC7:
        return bw * bh * 16;
    case DDS_RGBA8:
    case DDS_BGRA8:
        return (size_t)w * (size_t)h * 4;
    default:
        return 0;
    }
}

static bool decode_dds(const uint8_t *data, size_t len, const art_format_t *f, image_t *out, char *err,
                       size_t errlen)
{
    const size_t need = dds_level_size(f->dds, f->w, f->h);
    if (len < f->header_len + need) {
        snprintf(err, errlen, "the DDS file is cut short");
        return false;
    }
    if (!image_alloc(out, f->w, f->h)) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    const uint8_t *src = data + f->header_len;
    if (f->dds == DDS_RGBA8 || f->dds == DDS_BGRA8) {
        memcpy(out->rgba, src, need);
        if (f->dds == DDS_BGRA8)
            for (size_t i = 0; i < (size_t)f->w * f->h; i++) {
                uint8_t *p = out->rgba + i * 4;
                const uint8_t t = p[0];
                p[0] = p[2];
                p[2] = t;
            }
        return true;
    }
    const int bw = (f->w + 3) / 4, bh = (f->h + 3) / 4;
    const size_t block = f->dds == DDS_BC1 ? 8 : 16;
    uint8_t texels[4 * 4 * 4];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + ((size_t)by * bw + bx) * block;
            if (f->dds == DDS_BC1)
                bcdec_bc1(b, texels, 16);
            else if (f->dds == DDS_BC3)
                bcdec_bc3(b, texels, 16);
            else
                bcdec_bc7(b, texels, 16);
            for (int y = 0; y < 4; y++) {
                const int py = by * 4 + y;
                if (py >= f->h)
                    break;
                for (int x = 0; x < 4; x++) {
                    const int px = bx * 4 + x;
                    if (px >= f->w)
                        break;
                    memcpy(out->rgba + ((size_t)py * f->w + px) * 4, texels + (y * 4 + x) * 4, 4);
                }
            }
        }
    return true;
}

bool image_decode(const uint8_t *data, size_t len, image_t *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    art_format_t f;
    if (image_probe(data, len, &f) && f.container == CONTAINER_DDS)
        return decode_dds(data, len, &f, out, err, errlen);
    int w, h, n;
    uint8_t *pixels = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    if (pixels == NULL) {
        snprintf(err, errlen, "not a picture this app reads (%s)", stbi_failure_reason());
        return false;
    }
    /* own copy, so that every image_t is freed with free() */
    if (!image_alloc(out, w, h)) {
        stbi_image_free(pixels);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    memcpy(out->rgba, pixels, (size_t)w * h * 4);
    stbi_image_free(pixels);
    return true;
}

/* ---- scaling: separable Lanczos 3, premultiplied alpha, rows streamed ---- */

typedef struct {
    int start;
    int count;
    int offset; /* into the weight pool */
} span_t;

static float lanczos3(float x)
{
    x = fabsf(x);
    if (x < 1e-6f)
        return 1.0f;
    if (x >= 3.0f)
        return 0.0f;
    const float pi_x = (float)M_PI * x;
    return 3.0f * sinf(pi_x) * sinf(pi_x / 3.0f) / (pi_x * pi_x);
}

/* For each of out_size outputs, the inputs (0..in_size-1) it takes and their weights */
static bool make_spans(int in_size, float start, float length, int out_size, span_t **spans_out,
                       float **weights_out, int *max_count)
{
    const float scale = (float)out_size / length; /* >1 enlarges */
    const float filter_scale = scale < 1.0f ? scale : 1.0f;
    const float support = 3.0f / filter_scale;
    const int width = (int)ceilf(support * 2.0f) + 2;
    span_t *spans = calloc((size_t)out_size, sizeof(*spans));
    float *weights = calloc((size_t)out_size * width, sizeof(*weights));
    if (spans == NULL || weights == NULL) {
        free(spans);
        free(weights);
        return false;
    }
    *max_count = 0;
    for (int i = 0; i < out_size; i++) {
        const float centre = start + ((float)i + 0.5f) / scale;
        int lo = (int)floorf(centre - support);
        int hi = (int)ceilf(centre + support);
        float *w = weights + (size_t)i * width;
        int first = -1, last = -1;
        float total = 0.0f;
        for (int j = lo; j <= hi; j++) {
            const float v = lanczos3(((float)j + 0.5f - centre) * filter_scale);
            if (v == 0.0f)
                continue;
            const int k = j < 0 ? 0 : (j >= in_size ? in_size - 1 : j);
            if (first < 0)
                first = k;
            if (k - first >= width)
                continue;
            w[k - first] += v;
            if (k > last)
                last = k;
            total += v;
        }
        if (first < 0) {
            first = last = (int)fminf(fmaxf(centre, 0.0f), (float)(in_size - 1));
            w[0] = 1.0f;
            total = 1.0f;
        }
        const int count = last - first + 1;
        for (int k = 0; k < count; k++)
            w[k] /= total;
        spans[i] = (span_t){first, count, i * width};
        if (count > *max_count)
            *max_count = count;
    }
    *spans_out = spans;
    *weights_out = weights;
    return true;
}

bool image_resample(const image_t *src, float x, float y, float w, float h, int out_w, int out_h,
                    image_t *out)
{
    memset(out, 0, sizeof(*out));
    if (src->w <= 0 || src->h <= 0 || w <= 0.0f || h <= 0.0f || out_w <= 0 || out_h <= 0)
        return false;
    span_t *hs = NULL, *vs = NULL;
    float *hw = NULL, *vw = NULL;
    int hmax = 0, vmax = 0;
    float *ring = NULL;
    int *ring_row = NULL;
    bool ok = make_spans(src->w, x, w, out_w, &hs, &hw, &hmax) &&
              make_spans(src->h, y, h, out_h, &vs, &vw, &vmax) && image_alloc(out, out_w, out_h);
    const int ring_n = vmax + 1;
    if (ok) {
        ring = malloc((size_t)ring_n * out_w * 4 * sizeof(float));
        ring_row = malloc((size_t)ring_n * sizeof(int));
        ok = ring != NULL && ring_row != NULL;
    }
    if (ok) {
        for (int i = 0; i < ring_n; i++)
            ring_row[i] = -1;
        float acc[4];
        for (int oy = 0; oy < out_h; oy++) {
            const span_t *v = &vs[oy];
            /* the source rows this output row takes, scaled across */
            for (int k = 0; k < v->count; k++) {
                const int row = v->start + k;
                const int slot = row % ring_n;
                if (ring_row[slot] == row)
                    continue;
                ring_row[slot] = row;
                float *dst = ring + (size_t)slot * out_w * 4;
                const uint8_t *line = src->rgba + (size_t)row * src->w * 4;
                for (int ox = 0; ox < out_w; ox++) {
                    const span_t *s = &hs[ox];
                    const float *wt = hw + s->offset;
                    acc[0] = acc[1] = acc[2] = acc[3] = 0.0f;
                    for (int j = 0; j < s->count; j++) {
                        const uint8_t *p = line + (size_t)(s->start + j) * 4;
                        const float a = (float)p[3] * (1.0f / 255.0f);
                        acc[0] += wt[j] * p[0] * a;
                        acc[1] += wt[j] * p[1] * a;
                        acc[2] += wt[j] * p[2] * a;
                        acc[3] += wt[j] * a;
                    }
                    memcpy(dst + (size_t)ox * 4, acc, sizeof(acc));
                }
            }
            const float *wt = vw + v->offset;
            uint8_t *line = out->rgba + (size_t)oy * out_w * 4;
            for (int ox = 0; ox < out_w; ox++) {
                acc[0] = acc[1] = acc[2] = acc[3] = 0.0f;
                for (int k = 0; k < v->count; k++) {
                    const float *p = ring + ((size_t)((v->start + k) % ring_n) * out_w + ox) * 4;
                    acc[0] += wt[k] * p[0];
                    acc[1] += wt[k] * p[1];
                    acc[2] += wt[k] * p[2];
                    acc[3] += wt[k] * p[3];
                }
                const float a = acc[3];
                uint8_t *o = line + (size_t)ox * 4;
                if (a <= 1e-4f) {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }
                for (int c = 0; c < 3; c++) {
                    const float value = acc[c] / a;
                    o[c] = (uint8_t)(value < 0.0f ? 0.0f : value > 255.0f ? 255.0f : value + 0.5f);
                }
                const float alpha = a * 255.0f;
                o[3] = (uint8_t)(alpha < 0.0f ? 0.0f : alpha > 255.0f ? 255.0f : alpha + 0.5f);
            }
        }
    }
    free(hs);
    free(vs);
    free(hw);
    free(vw);
    free(ring);
    free(ring_row);
    if (!ok)
        image_free(out);
    return ok;
}

bool image_cover(const image_t *src, int w, int h, float pan_x, float pan_y, image_t *out)
{
    const float scale = fmaxf((float)w / src->w, (float)h / src->h);
    const float rw = (float)w / scale, rh = (float)h / scale;
    pan_x = fminf(fmaxf(pan_x, -1.0f), 1.0f);
    pan_y = fminf(fmaxf(pan_y, -1.0f), 1.0f);
    const float x = ((float)src->w - rw) * (pan_x + 1.0f) * 0.5f;
    const float y = ((float)src->h - rh) * (pan_y + 1.0f) * 0.5f;
    return image_resample(src, x, y, rw, rh, w, h, out);
}

bool image_shrink(const image_t *src, int max_w, int max_h, image_t *out)
{
    float scale = fminf((float)max_w / src->w, (float)max_h / src->h);
    if (scale >= 1.0f) {
        if (!image_alloc(out, src->w, src->h))
            return false;
        memcpy(out->rgba, src->rgba, (size_t)src->w * src->h * 4);
        return true;
    }
    int w = (int)lroundf(src->w * scale), h = (int)lroundf(src->h * scale);
    return image_resample(src, 0, 0, (float)src->w, (float)src->h, w < 1 ? 1 : w, h < 1 ? 1 : h, out);
}

bool image_has_alpha(const image_t *img)
{
    const size_t n = (size_t)img->w * img->h;
    size_t clear = 0;
    for (size_t i = 0; i < n; i++)
        if (img->rgba[i * 4 + 3] < 250)
            clear++;
    return clear > n / 100; /* more than a stray edge */
}

/* Three passes of a box blur, in place */
static void blur(image_t *img, int radius)
{
    const int w = img->w, h = img->h;
    uint8_t *temp = malloc((size_t)w * h * 4);
    if (temp == NULL)
        return;
    for (int pass = 0; pass < 3; pass++) {
        for (int dir = 0; dir < 2; dir++) {
            const int lines = dir == 0 ? h : w, length = dir == 0 ? w : h;
            for (int l = 0; l < lines; l++)
                for (int i = 0; i < length; i++) {
                    int sum[4] = {0, 0, 0, 0}, count = 0;
                    for (int k = -radius; k <= radius; k++) {
                        int j = i + k;
                        j = j < 0 ? 0 : j >= length ? length - 1 : j;
                        const size_t at = dir == 0 ? ((size_t)l * w + j) : ((size_t)j * w + l);
                        for (int c = 0; c < 4; c++)
                            sum[c] += img->rgba[at * 4 + c];
                        count++;
                    }
                    const size_t at = dir == 0 ? ((size_t)l * w + i) : ((size_t)i * w + l);
                    for (int c = 0; c < 4; c++)
                        temp[at * 4 + c] = (uint8_t)(sum[c] / count);
                }
            memcpy(img->rgba, temp, (size_t)w * h * 4);
        }
    }
    free(temp);
}

/* fg over bg at (x, y); bg stays opaque */
static void composite(image_t *bg, const image_t *fg, int x, int y)
{
    for (int fy = 0; fy < fg->h; fy++) {
        const int by = y + fy;
        if (by < 0 || by >= bg->h)
            continue;
        for (int fx = 0; fx < fg->w; fx++) {
            const int bx = x + fx;
            if (bx < 0 || bx >= bg->w)
                continue;
            const uint8_t *s = fg->rgba + ((size_t)fy * fg->w + fx) * 4;
            uint8_t *d = bg->rgba + ((size_t)by * bg->w + bx) * 4;
            const int a = s[3];
            for (int c = 0; c < 3; c++)
                d[c] = (uint8_t)((s[c] * a + d[c] * (255 - a) + 127) / 255);
            d[3] = 255;
        }
    }
}

bool image_fit_blur(const image_t *src, int w, int h, image_t *out)
{
    memset(out, 0, sizeof(*out));
    const bool clear = image_has_alpha(src);
    float scale = fminf((float)w / src->w, (float)h / src->h);
    if (clear)
        scale *= 0.8f; /* an icon sits on its plate with room around it */
    image_t fg = {0};
    if (!image_resample(src, 0, 0, (float)src->w, (float)src->h, (int)lroundf(src->w * scale),
                        (int)lroundf(src->h * scale), &fg))
        return false;
    if (clear) {
        /* a dark plate in the picture's own colour */
        double sum[3] = {0, 0, 0}, weight = 0;
        for (size_t i = 0; i < (size_t)src->w * src->h; i++) {
            const uint8_t *p = src->rgba + i * 4;
            for (int c = 0; c < 3; c++)
                sum[c] += p[c] * (double)p[3];
            weight += p[3];
        }
        if (!image_alloc(out, w, h)) {
            image_free(&fg);
            return false;
        }
        uint8_t plate[4] = {24, 24, 28, 255};
        if (weight > 0)
            for (int c = 0; c < 3; c++)
                plate[c] = (uint8_t)fmin(255.0, sum[c] / weight * 0.3 + 10.0);
        for (size_t i = 0; i < (size_t)w * h; i++)
            memcpy(out->rgba + i * 4, plate, 4);
    } else {
        /* the picture itself, filled across, blurred and darkened */
        image_t small = {0};
        const int sw = w / 24 > 8 ? w / 24 : 8, sh = h / 24 > 8 ? h / 24 : 8;
        if (!image_cover(src, sw, sh, 0, 0, &small)) {
            image_free(&fg);
            return false;
        }
        blur(&small, 1);
        const bool ok = image_resample(&small, 0, 0, (float)sw, (float)sh, w, h, out);
        image_free(&small);
        if (!ok) {
            image_free(&fg);
            return false;
        }
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t *p = out->rgba + i * 4;
            for (int c = 0; c < 3; c++)
                p[c] = (uint8_t)(p[c] * 0.55f);
            p[3] = 255;
        }
    }
    composite(out, &fg, (w - fg.w) / 2, (h - fg.h) / 2);
    image_free(&fg);
    return true;
}

/* ---- writing ---- */

static void write_to_buf(void *context, void *data, int size)
{
    buf_append(context, data, (size_t)size);
}

/* The next level down: each pixel the average of up to four */
static bool half(const image_t *src, image_t *out)
{
    const int w = src->w > 1 ? src->w / 2 : 1, h = src->h > 1 ? src->h / 2 : 1;
    if (!image_alloc(out, w, h))
        return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sum[4] = {0, 0, 0, 0}, n = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const int sx = x * 2 + dx, sy = y * 2 + dy;
                    if (sx >= src->w || sy >= src->h)
                        continue;
                    const uint8_t *p = src->rgba + ((size_t)sy * src->w + sx) * 4;
                    for (int c = 0; c < 4; c++)
                        sum[c] += p[c];
                    n++;
                }
            for (int c = 0; c < 4; c++)
                out->rgba[((size_t)y * w + x) * 4 + c] = (uint8_t)((sum[c] + n / 2) / n);
        }
    return true;
}

/* A copy padded (edge pixels repeated) to whole 4 x 4 blocks */
static bool pad4(const image_t *src, image_t *out)
{
    const int w = (src->w + 3) & ~3, h = (src->h + 3) & ~3;
    if (!image_alloc(out, w, h))
        return false;
    for (int y = 0; y < h; y++) {
        const int sy = y < src->h ? y : src->h - 1;
        for (int x = 0; x < w; x++) {
            const int sx = x < src->w ? x : src->w - 1;
            memcpy(out->rgba + ((size_t)y * w + x) * 4, src->rgba + ((size_t)sy * src->w + sx) * 4, 4);
        }
    }
    return true;
}

static bool encode_level(const image_t *level, dds_kind_t kind, buf_t *out)
{
    const size_t size = dds_level_size(kind, level->w, level->h);
    if (!buf_reserve(out, size))
        return false;
    uint8_t *dst = out->data + out->len;
    if (kind == DDS_RGBA8 || kind == DDS_BGRA8) {
        memcpy(dst, level->rgba, size);
        if (kind == DDS_BGRA8)
            for (size_t i = 0; i < (size_t)level->w * level->h; i++) {
                const uint8_t t = dst[i * 4];
                dst[i * 4] = dst[i * 4 + 2];
                dst[i * 4 + 2] = t;
            }
        out->len += size;
        return true;
    }
    image_t padded = {0};
    if (!pad4(level, &padded))
        return false;
    if (kind == DDS_BC7) {
        bc7_encode_image(&padded, dst);
    } else {
        const int bw = padded.w / 4, bh = padded.h / 4;
        const size_t block = kind == DDS_BC1 ? 8 : 16;
        uint8_t texels[64];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                for (int y = 0; y < 4; y++)
                    memcpy(texels + y * 16, padded.rgba + ((size_t)(by * 4 + y) * padded.w + bx * 4) * 4, 16);
                stb_compress_dxt_block(dst + ((size_t)by * bw + bx) * block, texels, kind == DDS_BC3,
                                       STB_DXT_HIGHQUAL);
            }
    }
    image_free(&padded);
    out->len += size;
    return true;
}

bool image_encode_like(const image_t *img, const art_format_t *like, const uint8_t *like_data,
                       size_t like_len, buf_t *out, char *err, size_t errlen)
{
    out->len = 0;
    if (img->w != like->w || img->h != like->h) {
        snprintf(err, errlen, "internal: %dx%d is not %dx%d", img->w, img->h, like->w, like->h);
        return false;
    }
    switch (like->container) {
    case CONTAINER_PNG: {
        stbi_write_png_compression_level = 6;
        int ok;
        if (like->channels == 3) {
            uint8_t *rgb = malloc((size_t)img->w * img->h * 3);
            if (rgb == NULL) {
                snprintf(err, errlen, "out of memory");
                return false;
            }
            for (size_t i = 0; i < (size_t)img->w * img->h; i++)
                memcpy(rgb + i * 3, img->rgba + i * 4, 3);
            ok = stbi_write_png_to_func(write_to_buf, out, img->w, img->h, 3, rgb, img->w * 3);
            free(rgb);
        } else {
            ok = stbi_write_png_to_func(write_to_buf, out, img->w, img->h, 4, img->rgba, img->w * 4);
        }
        if (!ok)
            snprintf(err, errlen, "the PNG could not be written");
        return ok != 0;
    }
    case CONTAINER_JPEG:
        if (!stbi_write_jpg_to_func(write_to_buf, out, img->w, img->h, 4, img->rgba, 92)) {
            snprintf(err, errlen, "the JPEG could not be written");
            return false;
        }
        return true;
    case CONTAINER_DDS: {
        /* the same header, then each level in the same format */
        size_t expected = like->header_len;
        for (int level = 0, w = like->w, h = like->h; level < like->mips; level++) {
            expected += dds_level_size(like->dds, w, h);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        if (like_len != expected) {
            snprintf(err, errlen, "the DDS file holds more than one picture (%zu bytes, %zu expected)",
                     like_len, expected);
            return false;
        }
        if (!buf_append(out, like_data, like->header_len))
            return false;
        image_t level = {0}, next = {0};
        if (!image_alloc(&level, img->w, img->h))
            return false;
        memcpy(level.rgba, img->rgba, (size_t)img->w * img->h * 4);
        bool ok = true;
        for (int i = 0; ok && i < like->mips; i++) {
            ok = encode_level(&level, like->dds, out);
            if (ok && i + 1 < like->mips) {
                ok = half(&level, &next);
                image_free(&level);
                level = next;
                memset(&next, 0, sizeof(next));
            }
        }
        image_free(&level);
        if (!ok)
            snprintf(err, errlen, "out of memory");
        return ok;
    }
    default:
        snprintf(err, errlen, "this kind of file cannot be written");
        return false;
    }
}
