/*
 * Art Swap helper - the home-screen refresh, through ShadowMountPlus's API.
 *
 * The home screen keeps its own copy of a title's art, found through app.db
 * (icon0Info "/user/app/<id>/sce_sys/icon0.png?ts=<install time>"), so new
 * files do not show until the title is installed again. For a title that
 * ShadowMountPlus installed from a folder, its HTTP/JSON API (127.0.0.1:10101,
 * docs/api.md of 1.7) does exactly that: games/uninstall removes the title's
 * registration through AppInstUtil and keeps its folder, and a scan installs it
 * again, copying the art from the folder. Saved data is not part of the
 * install, so it stays, as when a game is deleted from the home screen.
 *
 * Only such titles are refreshed: uninstalling a package deletes the game, and
 * an image's art is inside the image, which Art Swap does not change.
 *
 * Saved data must survive, so the refresh does not take that on trust: before
 * anything is removed it copies the system's saved data of the title (every
 * user's savedata_prospero/<id> and savedata/<id>) to /data/artswap/saves/,
 * and stops if it cannot; it takes a fingerprint (every file's path and size)
 * of the title's folder, where homebrew keeps its own saves; and afterwards it
 * checks both, copying back any saved file that went missing.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "smp.h"

#include "../third_party/cJSON.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifndef ARTSWAP_HOST
#include <ps5/kernel.h>
#endif

#define SMP_PORT 10101
#define EBUSY_STATUS 16

/* ---- the API ---- */

/* api_port in /data/shadowmount/config.ini, else the default */
static int smp_port(void)
{
#ifdef ARTSWAP_HOST
    if (getenv("ARTSWAP_SMP_PORT"))
        return atoi(getenv("ARTSWAP_SMP_PORT"));
#endif
    char path[512];
    buf_t ini = {0};
    int port = SMP_PORT;
    if (file_read(cpath("/data/shadowmount/config.ini", path, sizeof(path)), &ini) && ini.len > 0) {
        size_t at = 0;
        while (at < ini.len) {
            size_t end = at;
            while (end < ini.len && ini.data[end] != '\n')
                end++;
            char line[128];
            const size_t n = end - at < sizeof(line) - 1 ? end - at : sizeof(line) - 1;
            memcpy(line, ini.data + at, n);
            line[n] = 0;
            trim(line);
            if (strncmp(line, "api_port", 8) == 0) {
                const char *eq = strchr(line, '=');
                const int value = eq ? atoi(eq + 1) : 0;
                if (value > 0 && value < 65536)
                    port = value;
            }
            at = end + 1;
        }
    }
    buf_free(&ini);
    return port;
}

/* POSTs body to route; the answer's JSON object, which has a numeric status, or NULL and err */
static cJSON *smp_post(const char *route, const char *body, int timeout_s, char *err, size_t errlen)
{
    const int port = smp_port();
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return NULL;
    }
    struct timeval send_timeout = {5, 0}, recv_timeout = {timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        snprintf(err, errlen, "nothing answers on port %d (%s)", port, strerror(errno));
        close(fd);
        return NULL;
    }
    buf_t io = {0};
    buf_printf(&io,
               "POST %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
               "Connection: close\r\n\r\n%s",
               route, strlen(body), body);
    bool sent = io.data != NULL;
    for (size_t at = 0; sent && at < io.len;) {
        const ssize_t put = send(fd, io.data + at, io.len - at, 0);
        if (put < 0 && errno == EINTR)
            continue;
        if (put <= 0)
            sent = false;
        else
            at += (size_t)put;
    }
    io.len = 0;
    while (sent && io.len < (4u << 20) && buf_reserve(&io, 16384)) {
        const ssize_t got = recv(fd, io.data + io.len, 16384, 0);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        io.len += (size_t)got;
    }
    close(fd);
    cJSON *json = NULL;
    if (!sent)
        snprintf(err, errlen, "the request was not sent (%s)", strerror(errno));
    else if (io.len == 0)
        snprintf(err, errlen, "no answer from port %d", port);
    else {
        io.data[io.len] = 0;
        const char *head_end = strstr((const char *)io.data, "\r\n\r\n");
        if (strncmp((const char *)io.data, "HTTP/", 5) == 0 && head_end != NULL)
            json = cJSON_Parse(head_end + 4);
        if (json != NULL && !cJSON_IsNumber(cJSON_GetObjectItem(json, "status"))) {
            cJSON_Delete(json);
            json = NULL;
        }
        if (json == NULL)
            snprintf(err, errlen, "port %d did not answer as ShadowMountPlus does", port);
    }
    buf_free(&io);
    return json;
}

static int status_of(const cJSON *answer)
{
    return answer ? cJSON_GetObjectItem(answer, "status")->valueint : -1;
}

static const char *text_of(const cJSON *object, const char *name)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(object, name));
    return value ? value : "";
}

static bool flag_of(const cJSON *object, const char *name)
{
    return cJSON_IsTrue(cJSON_GetObjectItem(object, name));
}

/* "error (error_reason)" of a failed answer */
static void failure_of(const cJSON *answer, char *out, size_t size)
{
    const char *reason = text_of(answer, "error_reason");
    snprintf(out, size, "%s%s%s%s", text_of(answer, "error"), reason[0] ? " (" : "", reason, reason[0] ? ")" : "");
}

/* The title in games' list, or NULL */
static const cJSON *game_in(const cJSON *games, const char *id)
{
    const cJSON *game;
    cJSON_ArrayForEach(game, cJSON_GetObjectItem(games, "games"))
    {
        if (strcmp(text_of(game, "title_id"), id) == 0)
            return game;
    }
    return NULL;
}

/* 1 installed, 0 not, -1 unknown (ShadowMountPlus did not answer) */
static int installed_now(const char *id)
{
    char err[200];
    cJSON *games = smp_post("/api/v1/games", "{\"include_size\":false}", 20, err, sizeof(err));
    int state = -1;
    if (status_of(games) == 0) {
        const cJSON *game = game_in(games, id);
        state = game && flag_of(game, "installed") ? 1 : 0;
    }
    cJSON_Delete(games);
    return state;
}

/* The home screen's copy of the title was made at or after since: ShadowMountPlus's tracker,
 * and the install's copies of the title's param file. The game list says "installed" while
 * the install is still copying, so it alone is not enough. */
static bool installed_since(const char *id, time_t since)
{
    static const char *made[] = {"/user/app/%s/mount.lnk", "/user/appmeta/%s/param.json|/user/appmeta/%s/param.sfo",
                                 "/user/app/%s/sce_sys/param.json|/user/app/%s/sce_sys/param.sfo"};
    for (size_t i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
        char forms[160];
        copy_str(forms, sizeof(forms), made[i]);
        bool found = false;
        char *rest = NULL;
        for (char *form = strtok_r(forms, "|", &rest); form && !found; form = strtok_r(NULL, "|", &rest)) {
            char rel[128], path[512];
            struct stat st;
            snprintf(rel, sizeof(rel), form, id);
            /* the change time: a copy may carry its source's modification time, never its change time */
            found = stat(cpath(rel, path, sizeof(path)), &st) == 0 && st.st_ctime >= since;
        }
        if (!found)
            return false;
    }
    return installed_now(id) == 1;
}

/* ---- what it knows of a title ---- */

static bool can(const cJSON *version, const char *capability)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(version, "capabilities"))
    {
        if (cJSON_IsString(item) && strcmp(item->valuestring, capability) == 0)
            return true;
    }
    return false;
}

void smp_info(const app_t *app, smp_info_t *out)
{
    memset(out, 0, sizeof(*out));
    char err[200] = "";
    cJSON *version = smp_post("/api/v1/version", "{}", 5, err, sizeof(err));
    if (status_of(version) != 0) {
        log_line("ShadowMountPlus: %s", err[0] ? err : "version failed");
        snprintf(out->why, sizeof(out->why),
                 "ShadowMountPlus is not answering. Refreshing needs ShadowMountPlus 1.7 or later running, with its "
                 "API on (it is on unless config.ini says api_enabled=0).");
        cJSON_Delete(version);
        return;
    }
    out->answers = true;
    copy_str(out->version, sizeof(out->version), text_of(version, "shadowmount_version"));
    const bool able = can(version, "list_games") && can(version, "uninstall_game") && can(version, "rescan");
    cJSON_Delete(version);
    if (!able) {
        snprintf(out->why, sizeof(out->why),
                 "This ShadowMountPlus (%s) cannot uninstall and rescan through its API. Refreshing needs 1.7 or "
                 "later.",
                 out->version);
        return;
    }
    cJSON *games = smp_post("/api/v1/games", "{\"include_size\":false}", 20, err, sizeof(err));
    const cJSON *game = status_of(games) == 0 ? game_in(games, app->id) : NULL;
    if (status_of(games) != 0) {
        log_line("ShadowMountPlus games: %s", err);
        snprintf(out->why, sizeof(out->why), "ShadowMountPlus did not list its games. Try again in a moment.");
    } else if (game == NULL) {
        snprintf(out->why, sizeof(out->why),
                 "ShadowMountPlus did not install %s, so it cannot put it back on the home screen.", app->name);
    } else {
        out->found = true;
        out->installed = flag_of(game, "installed");
        copy_str(out->source_type, sizeof(out->source_type), text_of(game, "source_type"));
        copy_str(out->path, sizeof(out->path), text_of(game, "path"));
        char folder[512];
        const size_t plen = strlen(out->path);
        bool art_inside = false;
        for (int i = 0; i < app->nfiles && plen > 1; i++)
            if (strncmp(app->files[i].path, out->path, plen) == 0 && app->files[i].path[plen] == '/')
                art_inside = true;
        if (flag_of(game, "installed_pkg") || strcmp(out->source_type, "pkg") == 0)
            snprintf(out->why, sizeof(out->why),
                     "%s is installed from a package. Taking its tile away would delete the game, so Art Swap does "
                     "not refresh it.",
                     app->name);
        else if (strcmp(out->source_type, "folder") != 0)
            snprintf(out->why, sizeof(out->why),
                     "%s runs from a disk image. Its art is inside the image, which Art Swap does not change, so a "
                     "refresh would bring the old art back.",
                     app->name);
        else if (!flag_of(game, "managed") || !flag_of(game, "source_available") ||
                 !dir_exists(cpath(out->path, folder, sizeof(folder))))
            snprintf(out->why, sizeof(out->why),
                     "ShadowMountPlus cannot reach %s's folder right now, so it could not put the game back.",
                     app->name);
        else if (!art_inside)
            snprintf(out->why, sizeof(out->why),
                     "Art Swap has not changed the art in %s's own folder, so the refresh would bring the old art "
                     "back.",
                     app->name);
        else
            out->can_refresh = true;
    }
    cJSON_Delete(games);
}

/* ---- what must survive ---- */

/* A game's own files (its saves among them) belong to the user it ran as, in
 * folders only that user may open: the console refuses even root there. Such
 * a folder is read and written as its owner. The effective uid is the whole
 * process's, so this happens only under g_apply_lock, which art writes hold. */
static bool act_as(uid_t uid)
{
#ifdef ARTSWAP_HOST
    return seteuid(uid) == 0;
#else
    return kernel_set_ucred_uid(getpid(), uid) == 0;
#endif
}

/* opendir, or, when refused, opendir as the folder's owner: *as is then the uid to go back to */
static DIR *open_dir(const char *path, int *as)
{
    *as = -1;
    DIR *d = opendir(path);
    if (d != NULL || errno != EACCES)
        return d;
    struct stat st;
    const uid_t self = geteuid();
    if (stat(path, &st) != 0 || st.st_uid == self || !act_as(st.st_uid))
        return NULL;
    d = opendir(path);
    if (d == NULL)
        act_as(self);
    else
        *as = (int)self;
    return d;
}

static void close_dir(DIR *d, int as)
{
    closedir(d);
    if (as >= 0)
        act_as((uid_t)as);
}

/* mkdir -p, every new folder open to all, so whoever wrote into it, the helper can clear it later */
static bool make_dirs(const char *path)
{
    char temp[1024];
    copy_str(temp, sizeof(temp), path);
    for (char *p = temp + 1;; p++) {
        if (*p == '/' || *p == 0) {
            const char c = *p;
            *p = 0;
            if (mkdir(temp, 0777) == 0)
                chmod(temp, 0777);
            else if (errno != EEXIST)
                return false;
            *p = c;
            if (c == 0)
                break;
        }
    }
    return true;
}

/* A folder's fingerprint: its files, their bytes, and a sum over each file's path and size */
typedef struct {
    uint64_t files, bytes, mix;
    int unreadable; /* folders not even their owner could open */
} tree_sum_t;

static void tree_walk(const char *dir, size_t root_len, tree_sum_t *sum, int depth)
{
    int as;
    DIR *d = depth < 32 ? open_dir(dir, &as) : NULL;
    if (d == NULL) {
        sum->unreadable++;
        log_line("refresh: cannot read %s (%s)", dir, strerror(errno));
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        struct stat st;
        if (lstat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            tree_walk(path, root_len, sum, depth + 1);
            continue;
        }
        char key[1100];
        snprintf(key, sizeof(key), "%s\t%lld", path + root_len, (long long)st.st_size);
        sum->files++;
        sum->bytes += (uint64_t)st.st_size;
        sum->mix += fnv1a64(key);
    }
    close_dir(d, as);
}

static tree_sum_t tree_sum(const char *folder)
{
    tree_sum_t sum = {0};
    tree_walk(folder, strlen(folder), &sum, 0);
    return sum;
}

/* Copies one file in pieces, open to all; true when the copy has every byte */
static bool copy_file(const char *from, const char *to)
{
    const int in = open(from, O_RDONLY);
    if (in < 0)
        return false;
    struct stat st;
    const int out = fstat(in, &st) == 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    char *piece = malloc(1 << 20);
    bool ok = out >= 0 && fchmod(out, 0666) == 0 && piece != NULL;
    off_t total = 0;
    while (ok) {
        const ssize_t got = read(in, piece, 1 << 20);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            ok = got == 0;
            break;
        }
        for (ssize_t at = 0; ok && at < got;) {
            const ssize_t put = write(out, piece + at, (size_t)(got - at));
            if (put < 0 && errno == EINTR)
                continue;
            ok = put > 0;
            at += put > 0 ? put : 0;
        }
        total += got;
    }
    free(piece);
    if (out >= 0 && fsync(out) != 0)
        ok = false;
    if (out >= 0)
        close(out);
    close(in);
    return ok && total == st.st_size;
}

/* What a copy takes: only names ending in suffix (NULL: all), and no more than budget bytes in all */
typedef struct {
    const char *suffix;
    uint64_t budget, used;
    int files;
    bool failed, over;
} copy_t;

static bool wanted(const copy_t *c, const char *name)
{
    const size_t n = strlen(name), s = c->suffix ? strlen(c->suffix) : 0;
    return s == 0 || (n > s && strcmp(name + n - s, c->suffix) == 0);
}

static void copy_tree(const char *from, const char *to, copy_t *c, int depth)
{
    int as;
    DIR *d = depth < 32 ? open_dir(from, &as) : NULL;
    if (d == NULL) {
        if (depth > 0 || errno != ENOENT) {
            log_line("refresh: cannot read %s (%s)", from, strerror(errno));
            c->failed = true;
        }
        return;
    }
    struct dirent *entry;
    while (!c->failed && !c->over && (entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char src[1024], dst[1024];
        snprintf(src, sizeof(src), "%s/%s", from, entry->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", to, entry->d_name);
        struct stat st;
        if (lstat(src, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            copy_tree(src, dst, c, depth + 1);
        else if (S_ISREG(st.st_mode) && wanted(c, entry->d_name)) {
            if (c->used + (uint64_t)st.st_size > c->budget) {
                c->over = true;
                break;
            }
            if (!make_dirs(to) || !copy_file(src, dst)) {
                log_line("refresh: could not copy %s (%s)", src, strerror(errno));
                c->failed = true;
                break;
            }
            c->used += (uint64_t)st.st_size;
            c->files++;
        }
    }
    close_dir(d, as);
}

/* Each file under kept must be at its place under original: a missing one is copied back,
 * one of another size is counted in *changed. The original is walked level by level as the
 * fingerprint is, so a folder only its owner may open is entered as the owner, and what lies
 * inside is looked at, and put back, as that owner. The number copied back, or -1 when one
 * could not be. */
static int restore_missing(const char *kept, const char *original, int *changed, int depth)
{
    DIR *d = depth < 32 ? opendir(kept) : NULL;
    if (d == NULL)
        return 0;
    int as = -1;
    DIR *there = open_dir(original, &as);
    if (there == NULL && errno == ENOENT && make_dirs(original))
        there = open_dir(original, &as);
    if (there == NULL) {
        log_line("refresh: cannot open %s to check it (%s)", original, strerror(errno));
        closedir(d);
        return -1;
    }
    int restored = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && restored >= 0) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char src[1024], dst[1024];
        snprintf(src, sizeof(src), "%s/%s", kept, entry->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", original, entry->d_name);
        struct stat was, now;
        if (lstat(src, &was) != 0)
            continue;
        if (S_ISDIR(was.st_mode)) {
            const int inside = restore_missing(src, dst, changed, depth + 1);
            restored = inside < 0 ? -1 : restored + inside;
        } else if (lstat(dst, &now) == 0) {
            if (now.st_size != was.st_size) {
                (*changed)++;
                log_line("refresh: %s is %lld bytes, was %lld", dst, (long long)now.st_size, (long long)was.st_size);
            }
        } else if (errno == ENOENT) {
            const bool back = copy_file(src, dst);
            log_line("refresh: %s was missing; %s", dst, back ? "copied back" : "COULD NOT COPY IT BACK");
            restored = back ? restored + 1 : -1;
        } else {
            log_line("refresh: cannot look at %s (%s)", dst, strerror(errno));
            restored = -1;
        }
    }
    close_dir(there, as);
    closedir(d);
    return restored;
}

static void remove_tree(const char *path, int depth)
{
    DIR *d = depth < 32 ? opendir(path) : NULL;
    if (d != NULL) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char inside[1024];
            snprintf(inside, sizeof(inside), "%s/%s", path, entry->d_name);
            struct stat st;
            if (lstat(inside, &st) == 0 && S_ISDIR(st.st_mode))
                remove_tree(inside, depth + 1);
            else
                unlink(inside);
        }
        closedir(d);
    }
    rmdir(path);
}

/* The places a title's things are kept aside under its backup folder: the
 * system's saved data of every user, Art Swap's originals (.old) in the home
 * screen's copy (/user/appmeta/<id>; the install's /user/app/<id>/sce_sys is a
 * copy of the folder's, not Art Swap's). The folder itself is not copied: the
 * uninstall keeps it, and its fingerprint shows whether it did. */
typedef struct {
    char from[512];
    char to[600];
    copy_t rule;
    bool goes; /* the uninstall takes these away (the home screen's copies): putting them back is expected */
} keep_t;

#define KEEP_MAX 12
#define KEEP_BUDGET (1024ull << 20)

static int keep_list(const char *id, const char *backup, keep_t *keep)
{
    int n = 0;
    const copy_t olds = {".old", KEEP_BUDGET, 0, 0, false, false};
    const copy_t saves = {NULL, KEEP_BUDGET, 0, 0, false, false};
    char meta[512];
    snprintf(meta, sizeof(meta), "/user/appmeta/%s", id);
    cpath(meta, keep[n].from, sizeof(keep[n].from));
    snprintf(keep[n].to, sizeof(keep[n].to), "%s/appmeta", backup);
    keep[n].goes = true;
    keep[n++].rule = olds;
    char home[512];
    DIR *users = opendir(cpath("/user/home", home, sizeof(home)));
    struct dirent *user;
    while (users != NULL && (user = readdir(users)) != NULL && n + 2 <= KEEP_MAX) {
        if (user->d_name[0] == '.')
            continue;
        static const char *kinds[] = {"savedata_prospero", "savedata"};
        for (int k = 0; k < 2; k++) {
            snprintf(keep[n].from, sizeof(keep[n].from), "%s/%s/%s/%s", home, user->d_name, kinds[k], id);
            snprintf(keep[n].to, sizeof(keep[n].to), "%s/home/%s/%s", backup, user->d_name, kinds[k]);
            keep[n].goes = false;
            keep[n++].rule = saves;
        }
    }
    if (users != NULL)
        closedir(users);
    return n;
}

/* ---- the refresh ---- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_phase[12] = "none";
static char g_id[16];
static char g_message[480];
static bool g_running;
static bool g_unseen;  /* a finished refresh the title has not shown yet */
static bool g_loaded;  /* the last one read from refresh.txt */

/* The last refresh's end, kept for the title's next start: Art Swap has closed by then.
 * refresh.txt: id, phase, unseen (1/0), message, tab separated. g_lock held. */
static void save_result(void)
{
    char path[512];
    buf_t line = {0};
    buf_printf(&line, "%s\t%s\t%d\t", g_id, g_phase, g_unseen);
    buf_field(&line, g_message);
    buf_printf(&line, "\n");
    if (line.data)
        file_write_replace(data_path("refresh.txt", path, sizeof(path)), line.data, line.len, NULL);
    buf_free(&line);
}

static void load_result(void)
{
    if (g_loaded || g_running)
        return;
    g_loaded = true;
    char path[512];
    buf_t text = {0};
    if (file_read(data_path("refresh.txt", path, sizeof(path)), &text) && text.len > 0 && buf_reserve(&text, 1)) {
        text.data[text.len] = 0;
        char *rest = NULL;
        const char *id = strtok_r((char *)text.data, "\t", &rest);
        const char *phase = strtok_r(NULL, "\t", &rest);
        const char *unseen = strtok_r(NULL, "\t", &rest);
        const char *message = strtok_r(NULL, "\n", &rest);
        if (id && phase && unseen) {
            copy_str(g_id, sizeof(g_id), id);
            copy_str(g_phase, sizeof(g_phase), phase);
            g_unseen = unseen[0] == '1';
            copy_str(g_message, sizeof(g_message), message ? message : "");
        }
    }
    buf_free(&text);
}

typedef struct {
    char id[16];
    char name[200];
    char path[400]; /* its folder */
    bool installed;
    int wait_s;
} job_t;

static void set_phase(const char *phase, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void set_phase(const char *phase, const char *format, ...)
{
    pthread_mutex_lock(&g_lock);
    copy_str(g_phase, sizeof(g_phase), phase);
    va_list args;
    va_start(args, format);
    vsnprintf(g_message, sizeof(g_message), format, args);
    va_end(args);
    log_line("refresh %s: %s %s", g_id, g_phase, g_message);
    if (strcmp(phase, "done") == 0 || strcmp(phase, "failed") == 0) {
        g_unseen = true;
        save_result();
    }
    pthread_mutex_unlock(&g_lock);
}

/* The busy reasons that end by themselves: a game closing, a launch, an install */
static bool will_pass(const char *reason)
{
    return strcmp(reason, "game_active") == 0 || strcmp(reason, "runtime_prepared") == 0 ||
           strcmp(reason, "install_pending") == 0 || strcmp(reason, "scanner_busy") == 0 ||
           strcmp(reason, "runtime_busy") == 0 || strcmp(reason, "title_unmount_busy") == 0 || reason[0] == 0;
}

static void *refresh_job(void *argument)
{
    job_t *job = argument;
    char err[300], body[64];
    snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", job->id);
    bool ok = true;
    const time_t started = time(NULL) - 1;

    /* 0. what must survive: the folder's fingerprint, and the rest kept aside first; nothing is removed unless
     * every copy is whole */
    char folder[512], saves[512], backup[512];
    cpath(job->path, folder, sizeof(folder));
    data_path("saves", saves, sizeof(saves));
    snprintf(backup, sizeof(backup), "%s/%s-%llu", saves, job->id, (unsigned long long)time(NULL));
    static keep_t keep[KEEP_MAX];
    pthread_mutex_lock(&g_apply_lock);
    const tree_sum_t before = tree_sum(folder);
    const int nkeep = keep_list(job->id, backup, keep);
    uint64_t used = 0;
    int kept = 0;
    bool whole = make_dirs(backup), over = false;
    for (int i = 0; i < nkeep && whole; i++) {
        keep[i].rule.budget = KEEP_BUDGET - used;
        copy_tree(keep[i].from, keep[i].to, &keep[i].rule, 0);
        used += keep[i].rule.used;
        kept += keep[i].rule.files;
        over = keep[i].rule.over;
        whole = !keep[i].rule.failed && !over;
    }
    pthread_mutex_unlock(&g_apply_lock);
    log_line("refresh %s: folder %s: %llu files, %llu bytes, %d unreadable folders; %d files (%llu bytes) kept in %s%s",
             job->id, folder, (unsigned long long)before.files, (unsigned long long)before.bytes, before.unreadable,
             kept, (unsigned long long)used, backup, whole ? "" : " (INCOMPLETE)");
    if (before.files == 0 || before.unreadable > 0) {
        set_phase("failed", "Art Swap could not read all of %s's folder, so it changed nothing.", job->name);
        ok = false;
    } else if (!whole) {
        if (over)
            set_phase("failed", "%s's saved data is more than Art Swap can copy aside (1 GB), so it changed nothing.",
                      job->name);
        else
            set_phase("failed", "Art Swap could not copy %s's saved data aside, so it changed nothing.", job->name);
        ok = false;
    }
    bool removed = false;

    /* 1. away from the home screen: ShadowMountPlus refuses while a game (Art Swap) is open */
    if (ok && job->installed) {
        set_phase("waiting", "Waiting for the console to be free");
        const uint64_t until = now_ms() + (uint64_t)job->wait_s * 1000u;
        for (;;) {
            cJSON *answer = smp_post("/api/v1/games/uninstall", body, 20, err, sizeof(err));
            const int status = status_of(answer);
            const char *reason = answer ? text_of(answer, "error_reason") : "";
            if (status == 0) {
                cJSON_Delete(answer);
                removed = true;
                break;
            }
            const bool again = status == EBUSY_STATUS && will_pass(reason) && now_ms() < until;
            if (answer)
                failure_of(answer, err, sizeof(err));
            cJSON_Delete(answer);
            if (!again) {
                set_phase("failed", "ShadowMountPlus did not take %s off the home screen: %s", job->name, err);
                ok = false;
                break;
            }
            usleep(1000000);
        }
        if (ok)
            set_phase("removing", "Taking %s off the home screen", job->name);
    }

    /* 2. a scan installs it again; one more scan if it is slow */
    if (ok) {
        set_phase("adding", "ShadowMountPlus is adding %s back", job->name);
        bool back = false;
        for (int round = 0; round < 2 && !back; round++) {
            cJSON *answer = smp_post("/api/v1/scan", "{\"reset_attempts\":true}", 20, err, sizeof(err));
            if (status_of(answer) != 0 && answer)
                failure_of(answer, err, sizeof(err));
            if (status_of(answer) != 0)
                log_line("refresh %s: scan: %s", job->id, err);
            cJSON_Delete(answer);
            for (int i = 0; i < 60 && !back; i++) {
                back = installed_since(job->id, started);
                if (!back)
                    usleep(1000000);
            }
        }
        /* the last of the install's copies */
        if (back)
            usleep(2000000);
        if (!back)
            set_phase("failed",
                      "ShadowMountPlus has not added %s back yet. It will on its next scan, or after a restart of the "
                      "console.",
                      job->name);
        else
            set_phase("checking", "Checking %s's files", job->name);
        ok = back;
    }

    /* 3. everything still in place; anything missing is copied back */
    if (whole) {
        pthread_mutex_lock(&g_apply_lock);
        int restored = 0, expected = 0, changed = 0;
        for (int i = 0; i < nkeep; i++) {
            /* the home screen's copies come back only with the title: until then the originals stay in the copy */
            if (keep[i].goes && !dir_exists(keep[i].from))
                continue;
            const int back = restore_missing(keep[i].to, keep[i].from, &changed, 0);
            if (keep[i].goes && back > 0)
                expected += back;
            else
                restored = back < 0 || restored < 0 ? -1 : restored + back;
        }
        const tree_sum_t after = tree_sum(folder);
        pthread_mutex_unlock(&g_apply_lock);
        const bool same = after.files == before.files && after.bytes == before.bytes && after.mix == before.mix;
        log_line("refresh %s: after: folder %llu files, %llu bytes, %s; %d copied back (%d originals of Art Swap's "
                 "expected), %d changed",
                 job->id, (unsigned long long)after.files, (unsigned long long)after.bytes,
                 same ? "unchanged" : "CHANGED", restored + expected, expected, changed);
        if (restored < 0)
            set_phase("failed", "Some of %s's saved data went missing and Art Swap could not put it all back. The "
                      "copy is in %s.", job->name, backup);
        else if (!same)
            set_phase("failed", "%s's folder changed during the refresh: it held %llu files (%llu bytes) and now "
                      "holds %llu (%llu bytes). Art Swap keeps no copy of the folder, so check the game's saves.",
                      job->name, (unsigned long long)before.files, (unsigned long long)before.bytes,
                      (unsigned long long)after.files, (unsigned long long)after.bytes);
        else if (restored > 0 || changed > 0)
            set_phase("failed", "%s's saved data changed during the refresh. Art Swap put back %d that went missing; "
                      "the copy is in %s.", job->name, restored, backup);
        else if (!removed)
            remove_tree(backup, 0); /* nothing was touched */
        else if (ok) {
            set_phase("done", "%s is back on the home screen with its new art", job->name);
            /* all well: this copy is the one kept, older ones of the title go */
            DIR *d = opendir(saves);
            struct dirent *entry;
            const size_t idlen = strlen(job->id);
            char old[1024];
            while (d != NULL && (entry = readdir(d)) != NULL) {
                snprintf(old, sizeof(old), "%s/%s", saves, entry->d_name);
                if (strncmp(entry->d_name, job->id, idlen) == 0 && entry->d_name[idlen] == '-' &&
                    strcmp(old, backup) != 0)
                    remove_tree(old, 0);
            }
            if (d != NULL)
                closedir(d);
        }
    }
    pthread_mutex_lock(&g_lock);
    g_running = false;
    pthread_mutex_unlock(&g_lock);
    free(job);
    return NULL;
}

bool smp_refresh_start(const app_t *app, int wait_s, char *err, size_t errlen)
{
    smp_info_t info;
    smp_info(app, &info);
    if (!info.can_refresh) {
        snprintf(err, errlen, "%s", info.why);
        return false;
    }
    pthread_mutex_lock(&g_lock);
    const bool busy = g_running;
    if (!busy) {
        g_running = true;
        g_loaded = true;
        g_unseen = false;
        copy_str(g_id, sizeof(g_id), app->id);
        copy_str(g_phase, sizeof(g_phase), "copying");
        snprintf(g_message, sizeof(g_message), "Checking %s's files", app->name);
    }
    pthread_mutex_unlock(&g_lock);
    if (busy) {
        snprintf(err, errlen, "Art Swap is already refreshing a game. Try again when it has finished.");
        return false;
    }
    job_t *job = calloc(1, sizeof(*job));
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    bool started = false;
    if (job != NULL) {
        copy_str(job->id, sizeof(job->id), app->id);
        copy_str(job->name, sizeof(job->name), app->name);
        copy_str(job->path, sizeof(job->path), info.path);
        job->installed = info.installed;
        job->wait_s = wait_s < 0 ? 0 : wait_s > 600 ? 600 : wait_s;
        started = pthread_create(&thread, &attr, refresh_job, job) == 0;
    }
    pthread_attr_destroy(&attr);
    if (!started) {
        free(job);
        pthread_mutex_lock(&g_lock);
        g_running = false;
        pthread_mutex_unlock(&g_lock);
        snprintf(err, errlen, "the refresh could not start");
        return false;
    }
    log_line("refresh %s: started (ShadowMountPlus %s, %s, %s, wait %d s)", app->id, info.version, info.path,
             info.installed ? "installed" : "not installed", wait_s);
    return true;
}

void smp_refresh_line(buf_t *out)
{
    pthread_mutex_lock(&g_lock);
    load_result();
    buf_printf(out, "refresh\t%s\t%s\t%d\t", g_phase, g_id[0] ? g_id : "-", g_unseen);
    buf_field(out, g_message);
    buf_printf(out, "\n");
    pthread_mutex_unlock(&g_lock);
}

void smp_refresh_seen(void)
{
    pthread_mutex_lock(&g_lock);
    load_result();
    if (g_unseen && !g_running) {
        g_unseen = false;
        save_result();
    }
    pthread_mutex_unlock(&g_lock);
}

bool smp_refresh_running(void)
{
    pthread_mutex_lock(&g_lock);
    const bool running = g_running;
    pthread_mutex_unlock(&g_lock);
    return running;
}
