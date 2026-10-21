/*
 * Art Swap helper (artswapd) - the server the Art Swap title talks to.
 *
 * Started by the title through the console's ELF loader (127.0.0.1:9021), it
 * listens on 127.0.0.1:9117 and answers the requests of protocol.md, one
 * thread per connection. It leaves by itself 90 seconds after its last
 * connection closed (or 10 minutes after starting with none), and on BYE.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "art.h"
#include "name.h"
#include "phone.h"
#include "smp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_FIELDS 10

static pthread_mutex_t g_apps_lock = PTHREAD_MUTEX_INITIALIZER;
static app_t *g_apps;
static int g_count;
static bool g_scanned;

pthread_mutex_t g_apply_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_clients;
static uint64_t g_last_client_ms;
static bool g_ever_connected;
static volatile bool g_leaving;
static char g_http_error[200];
static bool g_http_ok;

/* ---- sending ---- */

static bool send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        const ssize_t put = send(fd, p, len, 0);
        if (put < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += put;
        len -= (size_t)put;
    }
    return true;
}

static bool reply_text(int fd, const buf_t *text)
{
    char head[64];
    snprintf(head, sizeof(head), "OK %zu 0 0\n", text->len);
    return send_all(fd, head, strlen(head)) && (text->len == 0 || send_all(fd, text->data, text->len));
}

static bool reply_image(int fd, const image_t *img)
{
    const size_t len = (size_t)img->w * img->h * 4;
    char head[64];
    snprintf(head, sizeof(head), "OK %zu %d %d\n", len, img->w, img->h);
    return send_all(fd, head, strlen(head)) && send_all(fd, img->rgba, len);
}

static bool reply_error(int fd, const char *message)
{
    char line[400];
    snprintf(line, sizeof(line), "ERR %s\n", message);
    for (char *p = line + 4; *p && p[1]; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    return send_all(fd, line, strlen(line));
}

/* "key user 1bb9" or "key none" */
static void key_line(buf_t *text)
{
    char hint[8];
    if (config_key_hint(hint, sizeof(hint)))
        buf_printf(text, "key\tuser\t%s\n", hint);
    else
        buf_printf(text, "key\tnone\n");
}

/* ---- the titles ---- */

static void rescan(void)
{
    app_t *apps = NULL;
    int count = 0;
    scan_apps(&apps, &count);
    pthread_mutex_lock(&g_apps_lock);
    free(g_apps);
    g_apps = apps;
    g_count = count;
    g_scanned = true;
    pthread_mutex_unlock(&g_apps_lock);
}

/* A copy of the title with this id */
static bool find_app(const char *id, app_t *out)
{
    pthread_mutex_lock(&g_apps_lock);
    bool found = false;
    for (int i = 0; i < g_count && !found; i++)
        if (strcmp(g_apps[i].id, id) == 0) {
            *out = g_apps[i];
            found = true;
        }
    pthread_mutex_unlock(&g_apps_lock);
    return found;
}

/* After APPLY, RESTORE or RENAME: which files now have an original kept, the match, the name */
static void refresh_app(const char *id, long game, const char *game_name, const char *name)
{
    pthread_mutex_lock(&g_apps_lock);
    for (int i = 0; i < g_count; i++) {
        app_t *app = &g_apps[i];
        if (strcmp(app->id, id) != 0)
            continue;
        for (int j = 0; j < app->nfiles; j++) {
            char old[420], real[512];
            snprintf(old, sizeof(old), "%s.old", app->files[j].path);
            app->files[j].has_old = file_exists(cpath(old, real, sizeof(real)));
        }
        if (game >= 0) {
            app->sgdb_id = game;
            copy_str(app->sgdb_name, sizeof(app->sgdb_name), game_name);
        }
        if (name != NULL)
            copy_str(app->name, sizeof(app->name), name);
    }
    pthread_mutex_unlock(&g_apps_lock);
}

static void describe_slot(buf_t *out, const app_t *app, const char *slot)
{
    const art_file_t *first = NULL;
    int count = 0;
    for (int i = 0; i < app->nfiles; i++)
        if (slot_matches(&app->files[i], slot)) {
            if (first == NULL || (strcmp(app->files[i].slot, "pic0") == 0 && strcmp(first->slot, "pic1") == 0))
                first = &app->files[i];
            count++;
        }
    if (first == NULL)
        buf_printf(out, "-");
    else
        buf_printf(out, "%s %dx%d x%d", format_name(&first->fmt), first->fmt.w, first->fmt.h, count);
}

static void list_apps(buf_t *out)
{
    pthread_mutex_lock(&g_apps_lock);
    for (int i = 0; i < g_count; i++) {
        const app_t *app = &g_apps[i];
        bool kept = false;
        for (int j = 0; j < app->nfiles; j++)
            kept |= app->files[j].has_old;
        buf_printf(out, "A\t%s\t", app->id);
        buf_field(out, app->name);
        buf_printf(out, "\t%s\t", app->kind);
        describe_slot(out, app, "icon");
        buf_printf(out, "\t");
        describe_slot(out, app, "pic");
        buf_printf(out, "\t%d\t%ld\t", kept, app->sgdb_id);
        buf_field(out, app->sgdb_name);
        buf_printf(out, "\t%d\n", name_kept(app));
    }
    pthread_mutex_unlock(&g_apps_lock);
}

/* ---- requests ---- */

static int clamp_size(const char *text, int fallback)
{
    const int value = text ? atoi(text) : 0;
    return value < 16 ? fallback : value > 4096 ? 4096 : value;
}

static bool want(int fd, int n, int need)
{
    if (n >= need)
        return true;
    reply_error(fd, "not enough fields");
    return false;
}

/* One request; false ends the connection */
static bool handle(int fd, char **field, int n)
{
    const char *cmd = field[0];
    char err[300] = "";
    buf_t text = {0};
    bool ok = true;

    if (strcmp(cmd, "HELLO") == 0) {
        char lan[64];
        if (phone_lan_address(lan, sizeof(lan))[0])
            buf_printf(&text, "lan\t%s\n", lan);
        buf_printf(&text, "artswapd\t%s\n", ARTSWAP_VERSION);
        key_line(&text);
        buf_printf(&text, "http\t%s\t", g_http_ok ? http_backend() : "unavailable");
        buf_field(&text, g_http_ok ? "" : g_http_error);
        buf_printf(&text, "\n");
        ok = reply_text(fd, &text);
    } else if (strcmp(cmd, "APPS") == 0) {
        if (!g_scanned || (n > 1 && strcmp(field[1], "rescan") == 0))
            rescan();
        list_apps(&text);
        ok = reply_text(fd, &text);
    } else if (strcmp(cmd, "ART") == 0) {
        if (!want(fd, n, 3))
            return true;
        app_t app;
        image_t img = {0};
        if (!find_app(field[1], &app))
            ok = reply_error(fd, "no such title");
        else if (!art_current(&app, field[2], clamp_size(n > 3 ? field[3] : NULL, 512),
                              clamp_size(n > 4 ? field[4] : NULL, 512), &img, err, sizeof(err)))
            ok = reply_error(fd, err);
        else
            ok = reply_image(fd, &img);
        image_free(&img);
    } else if (strcmp(cmd, "KEY") == 0) {
        const char *key = n > 1 ? field[1] : "";
        if (key[0] && !sgdb_check_key(key, err, sizeof(err)))
            ok = reply_error(fd, err);
        else if (!config_set_key(key))
            ok = reply_error(fd, "that does not look like an API key");
        else {
            key_line(&text);
            ok = reply_text(fd, &text);
        }
    } else if (strcmp(cmd, "KEYSTATE") == 0) {
        key_line(&text);
        ok = reply_text(fd, &text);
    } else if (strcmp(cmd, "PHONE") == 0) {
        /* PHONE code: open the key page for that code; PHONE alone closes it */
        if (n < 2 || field[1][0] == 0) {
            phone_stop();
            ok = reply_text(fd, &text);
        } else if (!phone_start(field[1], err, sizeof(err))) {
            ok = reply_error(fd, err);
        } else {
            char lan[64];
            buf_printf(&text, "lan\t%s\nport\t%d\n", phone_lan_address(lan, sizeof(lan)), phone_port());
            ok = reply_text(fd, &text);
        }
    } else if (strcmp(cmd, "SEARCH") == 0) {
        if (!want(fd, n, 2))
            return true;
        ok = sgdb_search(field[1], &text, err, sizeof(err)) ? reply_text(fd, &text) : reply_error(fd, err);
    } else if (strcmp(cmd, "MATCH") == 0) {
        if (!want(fd, n, 3))
            return true;
        const long game = strtol(field[2], NULL, 10);
        const char *name = n > 3 ? field[3] : "";
        if (!matches_set(field[1], game, name))
            ok = reply_error(fd, "the choice could not be saved");
        else {
            refresh_app(field[1], game, name, NULL);
            ok = reply_text(fd, &text);
        }
    } else if (strcmp(cmd, "LIST") == 0) {
        if (!want(fd, n, 3))
            return true;
        ok = sgdb_list(strtol(field[1], NULL, 10), field[2], n > 3 ? atoi(field[3]) : 0, &text, err, sizeof(err))
                 ? reply_text(fd, &text)
                 : reply_error(fd, err);
    } else if (strcmp(cmd, "THUMB") == 0 || strcmp(cmd, "PREVIEW") == 0) {
        /* THUMB url w h: the picture scaled down to fit
         * PREVIEW url slot mode panx pany w h [zoom]: what APPLY would make, at w x h */
        const bool preview = cmd[0] == 'P';
        if (!want(fd, n, preview ? 8 : 2))
            return true;
        buf_t data = {0};
        image_t src = {0}, img = {0};
        bool made = sgdb_fetch(field[1], &data, err, sizeof(err)) &&
                    image_decode(data.data, data.len, &src, err, sizeof(err));
        buf_free(&data);
        if (made && !preview)
            made = image_shrink(&src, clamp_size(n > 2 ? field[2] : NULL, 256), clamp_size(n > 3 ? field[3] : NULL, 256),
                                &img);
        else if (made) {
            const int w = clamp_size(field[6], 960), h = clamp_size(field[7], 540);
            made = art_compose(&src, field[2], field[3], (float)atof(field[4]), (float)atof(field[5]),
                               n > 8 ? (float)atof(field[8]) : 1.0f, w, h, &img);
        }
        if (made && img.rgba == NULL)
            snprintf(err, sizeof(err), "out of memory");
        ok = made && img.rgba ? reply_image(fd, &img) : reply_error(fd, err[0] ? err : "out of memory");
        image_free(&src);
        image_free(&img);
    } else if (strcmp(cmd, "APPLY") == 0) {
        if (!want(fd, n, 4))
            return true;
        app_t app;
        if (!find_app(field[1], &app)) {
            ok = reply_error(fd, "no such title");
        } else {
            pthread_mutex_lock(&g_apply_lock);
            const bool done = art_apply(&app, field[2], field[3], n > 4 ? field[4] : "cover",
                                        n > 5 ? (float)atof(field[5]) : 0.0f, n > 6 ? (float)atof(field[6]) : 0.0f,
                                        n > 7 ? (float)atof(field[7]) : 1.0f, &text, err, sizeof(err));
            pthread_mutex_unlock(&g_apply_lock);
            refresh_app(app.id, -1, "", NULL);
            ok = done ? reply_text(fd, &text) : reply_error(fd, err);
        }
    } else if (strcmp(cmd, "RESTORE") == 0) {
        if (!want(fd, n, 2))
            return true;
        app_t app;
        if (!find_app(field[1], &app)) {
            ok = reply_error(fd, "no such title");
        } else {
            pthread_mutex_lock(&g_apply_lock);
            /* all: the name too */
            const char *slot = n > 2 ? field[2] : "all";
            const bool all = strcmp(slot, "all") == 0;
            const bool name = all && name_kept(&app);
            bool art = !name;
            for (int i = 0; i < app.nfiles && !art; i++)
                art = app.files[i].has_old;
            bool done = !art || art_restore(&app, slot, &text, err, sizeof(err));
            if (done && name && name_restore(&app, &text) < 0) {
                snprintf(err, sizeof(err), "The original name could not be put back");
                done = false;
            }
            pthread_mutex_unlock(&g_apply_lock);
            refresh_app(app.id, -1, "", NULL);
            if (name)
                rescan(); /* the name, read again */
            ok = done ? reply_text(fd, &text) : reply_error(fd, err);
        }
    } else if (strcmp(cmd, "RENAME") == 0) {
        /* RENAME id name: the title's name, for a title ShadowMountPlus installed from a folder; the home
         * screen shows it after a refresh */
        if (!want(fd, n, 3))
            return true;
        app_t app;
        smp_info_t info;
        if (!find_app(field[1], &app)) {
            ok = reply_error(fd, "no such title");
        } else if (smp_info(&app, &info), !info.can_refresh) {
            snprintf(err, sizeof(err), "%s", info.why);
            ok = reply_error(fd, err);
        } else {
            pthread_mutex_lock(&g_apply_lock);
            const bool done = name_change(&app, field[2], &text, err, sizeof(err));
            pthread_mutex_unlock(&g_apply_lock);
            if (done)
                refresh_app(app.id, -1, "", field[2]);
            ok = done ? reply_text(fd, &text) : reply_error(fd, err);
        }
    } else if (strcmp(cmd, "SMP") == 0) {
        /* SMP id: what ShadowMountPlus knows of the title, and whether its tile can be refreshed */
        if (!want(fd, n, 2))
            return true;
        app_t app;
        if (!find_app(field[1], &app))
            return reply_error(fd, "no such title");
        smp_info_t info;
        smp_info(&app, &info);
        buf_printf(&text, "smp\t%s\n", info.answers ? (info.version[0] ? info.version : "?") : "-");
        if (info.found)
            buf_printf(&text, "game\t%s\t%d\t%s\n", info.source_type, info.installed, info.path);
        buf_printf(&text, "can\t%d\t", info.can_refresh);
        buf_field(&text, info.why);
        buf_printf(&text, "\n");
        smp_refresh_line(&text);
        ok = reply_text(fd, &text);
    } else if (strcmp(cmd, "REFRESH") == 0) {
        /* REFRESH id [wait]: take the title off the home screen and have ShadowMountPlus add it back,
         * asking for up to wait seconds while a game (Art Swap, until it closes) keeps the console busy */
        if (!want(fd, n, 2))
            return true;
        app_t app;
        if (!find_app(field[1], &app))
            ok = reply_error(fd, "no such title");
        else if (!smp_refresh_start(&app, n > 2 ? atoi(field[2]) : 0, err, sizeof(err)))
            ok = reply_error(fd, err);
        else {
            smp_refresh_line(&text);
            ok = reply_text(fd, &text);
        }
    } else if (strcmp(cmd, "REFRESHSTATE") == 0) {
        /* REFRESHSTATE [seen]: how the refresh goes, or went; seen: the title has shown its end */
        if (n > 1 && strcmp(field[1], "seen") == 0)
            smp_refresh_seen();
        smp_refresh_line(&text);
        ok = reply_text(fd, &text);
    } else if (strcmp(cmd, "LOG") == 0) {
        char path[512];
        buf_t log = {0};
        file_read(data_path("helper.log", path, sizeof(path)), &log);
        /* the last 8 KiB */
        const size_t keep = log.len > 8192 ? 8192 : log.len;
        buf_append(&text, log.data + log.len - keep, keep);
        buf_free(&log);
        ok = reply_text(fd, &text);
    } else if (strcmp(cmd, "BYE") == 0) {
        /* not in the middle of a refresh: it leaves when that has finished and nobody is connected */
        if (!smp_refresh_running())
            g_leaving = true;
        reply_text(fd, &text);
        ok = false;
    } else {
        ok = reply_error(fd, "unknown request");
    }
    buf_free(&text);
    return ok;
}

/* ---- connections ---- */

static void *serve(void *argument)
{
    const int fd = (int)(intptr_t)argument;
    char line[8192];
    size_t used = 0;
    bool open = true;
    while (open && !g_leaving) {
        const ssize_t got = recv(fd, line + used, sizeof(line) - 1 - used, 0);
        if (got <= 0) {
            if (got < 0 && errno == EINTR)
                continue;
            break;
        }
        used += (size_t)got;
        line[used] = 0;
        char *newline;
        while (open && (newline = memchr(line, '\n', used)) != NULL) {
            *newline = 0;
            if (newline > line && newline[-1] == '\r')
                newline[-1] = 0;
            char *field[MAX_FIELDS];
            int n = 0;
            char *save = NULL;
            /* tabs separate fields; empty fields are kept */
            for (char *p = line; n < MAX_FIELDS;) {
                field[n++] = p;
                char *tab = strchr(p, '\t');
                if (tab == NULL)
                    break;
                *tab = 0;
                p = tab + 1;
            }
            (void)save;
            if (field[0][0])
                open = handle(fd, field, n);
            const size_t consumed = (size_t)(newline - line) + 1;
            memmove(line, newline + 1, used - consumed);
            used -= consumed;
        }
        if (used == sizeof(line) - 1) {
            reply_error(fd, "request too long");
            break;
        }
    }
    close(fd);
    pthread_mutex_lock(&g_clients_lock);
    g_clients--;
    g_last_client_ms = now_ms();
    pthread_mutex_unlock(&g_clients_lock);
    return NULL;
}

/* Nobody has needed the helper for a while */
static bool idle(uint64_t started)
{
    pthread_mutex_lock(&g_clients_lock);
    /* a refresh carries on after the title has closed */
    const bool quiet = g_clients == 0 && !smp_refresh_running() &&
                       ((g_ever_connected && now_ms() - g_last_client_ms > 90000) ||
                        (!g_ever_connected && now_ms() - started > 600000));
    pthread_mutex_unlock(&g_clients_lock);
    return quiet;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char folder[512];
    mkdirs(data_path("cache", folder, sizeof(folder)));
    log_open();
    log_line("artswapd %s starting, pid %d", ARTSWAP_VERSION, (int)getpid());
    config_load();
    g_http_ok = http_init(g_http_error, sizeof(g_http_error));
    log_line("network: %s %s, key: %s", g_http_ok ? "ready" : "unavailable", g_http_error, config_key_source());
    sgdb_cache_trim();

    int port = ARTSWAP_PORT;
#ifdef ARTSWAP_HOST
    if (getenv("ARTSWAP_PORT"))
        port = atoi(getenv("ARTSWAP_PORT"));
#endif
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        log_line("socket: %s", strerror(errno));
        return 1;
    }
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    /* the title is on the same console; /data/artswap/listen-lan opens it to the network, for tests */
    char lan[512];
    const bool open_lan = file_exists(data_path("listen-lan", lan, sizeof(lan)));
    address.sin_addr.s_addr = htonl(open_lan ? INADDR_ANY : INADDR_LOOPBACK);
    /* a helper that crashed is stopped, not ended, and keeps its port: the next of three */
    int bound = -1;
    for (int i = 0; i < 3 && bound < 0; i++) {
        address.sin_port = htons((uint16_t)(port + 10 * i));
        if (bind(server, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(server, 8) == 0)
            bound = port + 10 * i;
        else
            log_line("port %d: %s", port + 10 * i, strerror(errno));
    }
    if (bound < 0) {
        log_line("no port free: leaving");
        close(server);
        return 1;
    }
    port = bound;
    log_line("listening on %s:%d", open_lan ? "0.0.0.0" : "127.0.0.1", port);
    const uint64_t started = now_ms();
    while (!g_leaving) {
        struct pollfd waiting = {server, POLLIN, 0};
        const int ready = poll(&waiting, 1, 1000);
        if (ready < 0 && errno != EINTR)
            break;
        if (ready <= 0) {
            if (idle(started)) {
                log_line("no one has asked for a while: leaving");
                break;
            }
            continue;
        }
        const int client = accept(server, NULL, NULL);
        if (client < 0)
            continue;
        pthread_mutex_lock(&g_clients_lock);
        g_clients++;
        g_ever_connected = true;
        pthread_mutex_unlock(&g_clients_lock);
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        if (pthread_create(&thread, &attr, serve, (void *)(intptr_t)client) != 0) {
            pthread_mutex_lock(&g_clients_lock);
            g_clients--;
            pthread_mutex_unlock(&g_clients_lock);
            close(client);
        }
        pthread_attr_destroy(&attr);
    }
    g_leaving = true;
    close(server);
    /* let the threads that are answering finish */
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&g_clients_lock);
        const int left = g_clients;
        pthread_mutex_unlock(&g_clients_lock);
        if (left == 0)
            break;
        usleep(100000);
    }
    log_line("artswapd leaving");
    return 0;
}
