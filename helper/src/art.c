/*
 * Art Swap helper - changing a title's art, and putting the original back.
 *
 * Before a file is replaced the first time, the original is copied beside it
 * as <file>.old (icon0.png.old, pic0.dds.old); an .old that exists is never
 * written again, so it is always the art the title came with. A copy without
 * one (the home screen's, made again from the folder) takes another copy's.
 * Restoring moves each .old back over its file. New art is written in the
 * form of the file it replaces (container, size, channels, DDS format, levels
 * and header), so the home screen reads it as it read the original.
 *
 * A title with no background has nothing to replace: Art Swap adds pic0.dds
 * to the game's own sce_sys, and the home screen takes it in when the game is
 * installed again (Apply changes). It registers only a pic0.dds: a title with
 * a pic0.png alone shows none. Those files have no original; they are
 * listed in /data/artswap/added.txt, and Restore deletes them.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "art.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void added_note(const char *id, const char *file);

/* The file to show for a slot: pic0 before pic1, the home screen's copy first */
static const art_file_t *preview_file(const app_t *app, const char *slot)
{
    const art_file_t *best = NULL;
    for (int i = 0; i < app->nfiles; i++) {
        const art_file_t *f = &app->files[i];
        if (!slot_matches(f, slot))
            continue;
        const bool appmeta = strncmp(f->path, "/user/appmeta/", 14) == 0;
        if (best == NULL || (strcmp(f->slot, "pic0") == 0 && strcmp(best->slot, "pic1") == 0) ||
            (strcmp(f->slot, best->slot) == 0 && appmeta && strncmp(best->path, "/user/appmeta/", 14) != 0))
            best = f;
    }
    return best;
}

bool art_current(const app_t *app, const char *slot, int max_w, int max_h, image_t *out, char *err,
                 size_t errlen)
{
    memset(out, 0, sizeof(*out));
    const art_file_t *file = preview_file(app, slot);
    if (file == NULL) {
        snprintf(err, errlen, "%s has no %s", app->name, strcmp(slot, "icon") == 0 ? "icon" : "background");
        return false;
    }
    char real[512];
    buf_t data = {0};
    if (!file_read(cpath(file->path, real, sizeof(real)), &data)) {
        snprintf(err, errlen, "%s cannot be read", file->path);
        return false;
    }
    image_t full = {0};
    bool ok = image_decode(data.data, data.len, &full, err, errlen);
    buf_free(&data);
    if (ok) {
        ok = image_shrink(&full, max_w, max_h, out);
        if (!ok)
            snprintf(err, errlen, "out of memory");
    }
    image_free(&full);
    return ok;
}

/* The original another copy of this picture keeps (the folder's sce_sys/icon0.png.old for the home
 * screen's icon0.png): ShadowMountPlus makes the home screen's copies again from the folder, without
 * the .old beside them, so a copy can lose its own while the folder still has the art it came with */
static bool other_old(const app_t *app, const art_file_t *file, buf_t *out)
{
    const char *name = strrchr(file->path, '/');
    for (int i = 0; name != NULL && i < app->nfiles; i++) {
        const art_file_t *other = &app->files[i];
        const char *other_name = strrchr(other->path, '/');
        if (other == file || other_name == NULL || strcasecmp(name, other_name) != 0)
            continue;
        char old[420], real_old[512];
        art_format_t fmt;
        snprintf(old, sizeof(old), "%s.old", other->path);
        if (file_exists(cpath(old, real_old, sizeof(real_old))) && file_read(real_old, out) &&
            image_probe(out->data, out->len, &fmt) && fmt.w == file->fmt.w && fmt.h == file->fmt.h)
            return true;
        buf_free(out);
    }
    return false;
}

/* ---- backgrounds Art Swap added ---- */

static const char *added_path(char *out, size_t size)
{
    return data_path("added.txt", out, size);
}

bool art_added(const char *id)
{
    char path[512], line[24];
    buf_t text = {0};
    snprintf(line, sizeof(line), "\n%s\t", id); /* a line of its own: the text read starts after a newline */
    bool found = false;
    if (file_read(added_path(path, sizeof(path)), &text) && text.len > 0) {
        buf_t lines = {0};
        buf_append(&lines, "\n", 1);
        buf_append(&lines, text.data, text.len);
        found = strstr((const char *)lines.data, line) != NULL;
        buf_free(&lines);
    }
    buf_free(&text);
    return found;
}

/* "id<TAB>path", once */
static void added_note(const char *id, const char *file)
{
    char path[512], line[460];
    buf_t text = {0};
    snprintf(line, sizeof(line), "%s\t%s\n", id, file);
    file_read(added_path(path, sizeof(path)), &text);
    if (text.len == 0 || strstr((const char *)text.data, line) == NULL) {
        buf_append(&text, line, strlen(line));
        file_write_replace(path, text.data, text.len, NULL);
    }
    buf_free(&text);
}

int art_remove_added(const char *id, buf_t *report)
{
    char path[512];
    buf_t text = {0}, kept = {0};
    if (!file_read(added_path(path, sizeof(path)), &text) || text.len == 0) {
        buf_free(&text);
        return 0;
    }
    const size_t idlen = strlen(id);
    int removed = 0;
    bool failed = false;
    char *save = NULL;
    for (char *line = strtok_r((char *)text.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, id, idlen) != 0 || line[idlen] != '\t') {
            buf_printf(&kept, "%s\n", line);
            continue;
        }
        char real[512];
        const char *file = line + idlen + 1;
        if (unlink(cpath(file, real, sizeof(real))) == 0 || !file_exists(real)) {
            buf_printf(report, "R\t%s\n", file);
            removed++;
        } else {
            buf_printf(&kept, "%s\n", line); /* tried again next time */
            failed = true;
        }
    }
    file_write_replace(path, kept.data ? kept.data : (uint8_t *)"", kept.len, NULL);
    buf_free(&text);
    buf_free(&kept);
    /* the copies an install made of it for the home screen, which the list may not name yet */
    static const char *copies[] = {"/user/appmeta/%s/pic0.dds", "/user/app/%s/sce_sys/pic0.dds",
                                   "/user/appmeta/%s/pic0.png", "/user/app/%s/sce_sys/pic0.png"};
    for (size_t i = 0; i < sizeof(copies) / sizeof(copies[0]) && removed > 0; i++) {
        char copy[200], real[512], old[220], real_old[512];
        snprintf(copy, sizeof(copy), copies[i], id);
        snprintf(old, sizeof(old), "%s.old", copy);
        if (file_exists(cpath(copy, real, sizeof(real))) && !file_exists(cpath(old, real_old, sizeof(real_old))) &&
            unlink(real) == 0) {
            buf_printf(report, "R\t%s\n", copy);
            removed++;
        }
    }
    log_line("restore %s: %d added background files deleted%s", id, removed, failed ? ", one could not be" : "");
    return failed ? -1 : removed;
}

bool art_add_background(const app_t *app, const char *folder, const char *url, const char *mode, float pan_x,
                        float pan_y, float zoom, buf_t *report, char *err, size_t errlen)
{
    char dest[440], real[512], icon[440], like[512];
    snprintf(dest, sizeof(dest), "%s/sce_sys/pic0.dds", folder);
    snprintf(icon, sizeof(icon), "%s/sce_sys/icon0.png", folder);
    cpath(icon, like, sizeof(like));
    if (file_exists(cpath(dest, real, sizeof(real)))) {
        snprintf(err, errlen, "%s has a background already: %s", app->name, dest);
        return false;
    }
    buf_t download = {0}, dds = {0};
    image_t src = {0}, made = {0};
    bool ok = sgdb_fetch(url, &download, err, errlen) && image_decode(download.data, download.len, &src, err, errlen);
    buf_free(&download);
    /* the home screen registers a background only as pic0.dds (a pic0.png alone is left out): 3840x2160 BC7,
     * as the console writes its own */
    if (ok && !art_compose(&src, "pic", mode, pan_x, pan_y, zoom, 3840, 2160, &made)) {
        snprintf(err, errlen, "the picture could not be made at 3840x2160");
        ok = false;
    }
    ok = ok && image_encode_bc7_dds(&made, &dds, err, errlen);
    if (ok && !file_write_replace(real, dds.data, dds.len, file_exists(like) ? like : NULL)) {
        snprintf(err, errlen, "%s could not be written", dest);
        ok = false;
    }
    if (ok) {
        added_note(app->id, dest);
        buf_printf(report, "W\t%s\tdds-bc7 3840x2160\n", dest);
    }
    log_line("add %s background: %s, %s%s%s", app->id, url, dest, ok ? "" : ": ", ok ? "" : err);
    image_free(&src);
    image_free(&made);
    buf_free(&dds);
    return ok;
}

bool art_compose(const image_t *src, const char *slot, const char *mode, float pan_x, float pan_y, float zoom, int w,
                 int h, image_t *out)
{
    if (strcmp(mode, "fit") == 0)
        return image_fit_blur(src, w, h, out);
    const float aspect_src = (float)src->w / src->h, aspect_dst = (float)w / h;
    if (strcmp(slot, "icon") == 0 && fabsf(aspect_src / aspect_dst - 1.0f) < 0.02f && zoom <= 1.001f)
        return image_resample(src, 0, 0, (float)src->w, (float)src->h, w, h, out);
    return image_cover(src, w, h, pan_x, pan_y, zoom, out);
}

bool art_apply(const app_t *app, const char *slot, const char *url, const char *mode, float pan_x, float pan_y,
               float zoom, buf_t *report, char *err, size_t errlen)
{
    const uint64_t start = now_ms();
    int targets = 0;
    for (int i = 0; i < app->nfiles; i++)
        targets += slot_matches(&app->files[i], slot);
    if (targets == 0) {
        snprintf(err, errlen, "%s has no %s file to replace", app->name,
                 strcmp(slot, "icon") == 0 ? "icon" : "background");
        return false;
    }
    buf_t download = {0};
    if (!sgdb_fetch(url, &download, err, errlen)) {
        buf_free(&download);
        return false;
    }
    image_t src = {0};
    const bool decoded = image_decode(download.data, download.len, &src, err, errlen);
    buf_free(&download);
    if (!decoded)
        return false;
    log_line("apply %s %s: %dx%d from %s, %s, pan %.2f %.2f, zoom %.2f", app->id, slot, src.w, src.h, url, mode,
             pan_x, pan_y, zoom);

    /* one picture per target size; files of the same size share it */
    image_t made = {0};
    int made_w = 0, made_h = 0;
    int written = 0;
    bool ok = true;
    for (int i = 0; ok && i < app->nfiles; i++) {
        const art_file_t *file = &app->files[i];
        if (!slot_matches(file, slot))
            continue;
        char real[512], real_old[512], old[420];
        cpath(file->path, real, sizeof(real));
        snprintf(old, sizeof(old), "%s.old", file->path);
        cpath(old, real_old, sizeof(real_old));
        buf_t original = {0}, encoded = {0};
        art_format_t fmt;
        if (!file_read(real, &original) || !image_probe(original.data, original.len, &fmt)) {
            snprintf(err, errlen, "%s cannot be read", file->path);
            ok = false;
        }
        if (ok && (made.rgba == NULL || made_w != fmt.w || made_h != fmt.h)) {
            image_free(&made);
            ok = art_compose(&src, slot, mode, pan_x, pan_y, zoom, fmt.w, fmt.h, &made);
            made_w = fmt.w;
            made_h = fmt.h;
            if (!ok)
                snprintf(err, errlen, "the picture could not be made at %dx%d", fmt.w, fmt.h);
        }
        if (ok)
            ok = image_encode_like(&made, &fmt, original.data, original.len, &encoded, err, errlen);
        /* the original, kept once: another copy's, if it keeps one, rather than the art shown now; a background
         * Art Swap added had none, so its copies are listed with it instead */
        const bool added = strcmp(slot, "pic") == 0 && art_added(app->id);
        if (ok && added)
            added_note(app->id, file->path);
        else if (ok && !file_exists(real_old)) {
            buf_t kept = {0};
            const buf_t *keep = other_old(app, file, &kept) ? &kept : &original;
            ok = file_write_replace(real_old, keep->data, keep->len, real);
            if (!ok)
                snprintf(err, errlen, "the original could not be kept as %s", old);
            else
                buf_printf(report, "O\t%s\n", old);
            buf_free(&kept);
        }
        if (ok) {
            ok = file_write_replace(real, encoded.data, encoded.len, real);
            if (ok) {
                buf_printf(report, "W\t%s\t%s %dx%d\n", file->path, format_name(&fmt), fmt.w, fmt.h);
                written++;
            } else {
                snprintf(err, errlen, "%s could not be written", file->path);
            }
        }
        buf_free(&original);
        buf_free(&encoded);
    }
    image_free(&made);
    image_free(&src);
    log_line("apply %s %s: %d of %d files in %llu ms%s%s", app->id, slot, written, targets,
             (unsigned long long)(now_ms() - start), ok ? "" : ": ", ok ? "" : err);
    return ok;
}

bool art_restore(const app_t *app, const char *slot, buf_t *report, char *err, size_t errlen)
{
    int restored = 0, failed = 0;
    /* first the copies without their .old: another copy's original, before the moves below take it */
    for (int i = 0; i < app->nfiles; i++) {
        const art_file_t *file = &app->files[i];
        if (strcmp(slot, "all") != 0 && !slot_matches(file, slot))
            continue;
        char real[512], real_old[512], old[420];
        cpath(file->path, real, sizeof(real));
        snprintf(old, sizeof(old), "%s.old", file->path);
        buf_t kept = {0};
        if (file_exists(cpath(old, real_old, sizeof(real_old))) || !other_old(app, file, &kept))
            continue;
        if (file_write_replace(real, kept.data, kept.len, real)) {
            buf_printf(report, "R\t%s\n", file->path);
            restored++;
        } else {
            failed++;
        }
        buf_free(&kept);
    }
    for (int i = 0; i < app->nfiles; i++) {
        const art_file_t *file = &app->files[i];
        if (strcmp(slot, "all") != 0 && !slot_matches(file, slot))
            continue;
        char real[512], real_old[512], old[420];
        cpath(file->path, real, sizeof(real));
        snprintf(old, sizeof(old), "%s.old", file->path);
        cpath(old, real_old, sizeof(real_old));
        if (!file_exists(real_old))
            continue;
        if (rename(real_old, real) == 0) {
            buf_printf(report, "R\t%s\n", file->path);
            restored++;
        } else {
            failed++;
        }
    }
    log_line("restore %s %s: %d files, %d failed", app->id, slot, restored, failed);
    if (failed) {
        snprintf(err, errlen, "%d file%s could not be put back", failed, failed == 1 ? "" : "s");
        return false;
    }
    if (restored == 0) {
        snprintf(err, errlen, "%s has no original art kept", app->name);
        return false;
    }
    return true;
}
