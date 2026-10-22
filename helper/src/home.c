/*
 * Art Swap helper - Apply changes: the home screen restarted (home.h).
 *
 * The home screen keeps what it drew of each game's art, so new files do not
 * show by themselves. Its process, SceShellUI, can be ended: the system starts
 * it again within seconds, and the new one reads every game's art afresh,
 * whatever installed the game (a folder, a disk image, a package). Nothing else
 * restarts: a game open in the background, saved data and the payloads carry
 * on (measured on 13.60; OnionHEN's shutdown restarts it the same way).
 *
 * The title asks, then closes, so the helper first waits for its process to
 * end: the home screen it restarts is the one the person is looking at.
 *
 * A name is different: the home screen takes it only from an install. So a
 * renamed title is installed again first, through ShadowMountPlus (smp.c, with
 * its saved data copied aside and checked), and the restart follows.
 *
 * pending.txt keeps the titles changed since the last restart, one a line: the
 * id, then a tab and "name" for a rename waiting for its install.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "home.h"
#include "smp.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef ARTSWAP_HOST
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_phase[12] = "none";
static char g_message[300];
static bool g_running;
static bool g_unseen; /* a finished restart the title has not shown yet */
static bool g_loaded; /* the last one read from home.txt */

/* ---- the titles changed since the last restart (g_lock held) ---- */

#define PENDING_MAX 256

typedef struct {
    char id[16];
    bool name; /* renamed: it needs installing again */
} pending_t;

static int pending_read(pending_t *list)
{
    char path[512];
    buf_t text = {0};
    int n = 0;
    if (file_read(data_path("pending.txt", path, sizeof(path)), &text) && text.len > 0 && buf_append(&text, "", 1)) {
        char *save = NULL;
        for (char *line = strtok_r((char *)text.data, "\n", &save); line && n < PENDING_MAX;
             line = strtok_r(NULL, "\n", &save)) {
            char *tab = strchr(line, '\t');
            if (tab)
                *tab = 0;
            if (line[0] == 0)
                continue;
            copy_str(list[n].id, sizeof(list[n].id), line);
            list[n].name = tab && strcmp(tab + 1, "name") == 0;
            n++;
        }
    }
    buf_free(&text);
    return n;
}

static void pending_write(const pending_t *list, int n)
{
    char path[512];
    buf_t text = {0};
    for (int i = 0; i < n; i++)
        buf_printf(&text, "%s%s\n", list[i].id, list[i].name ? "\tname" : "");
    file_write_replace(data_path("pending.txt", path, sizeof(path)), text.data ? text.data : (uint8_t *)"", text.len,
                       NULL);
    buf_free(&text);
}

static void note(const char *id, bool name)
{
    static pending_t list[PENDING_MAX];
    pthread_mutex_lock(&g_lock);
    int n = pending_read(list), at = -1;
    for (int i = 0; i < n && at < 0; i++)
        if (strcmp(list[i].id, id) == 0)
            at = i;
    if (at < 0 && n < PENDING_MAX) {
        at = n++;
        copy_str(list[at].id, sizeof(list[at].id), id);
        list[at].name = false;
    }
    if (at >= 0) {
        list[at].name |= name;
        pending_write(list, n);
    }
    pthread_mutex_unlock(&g_lock);
}

void home_note(const char *id)
{
    note(id, false);
}

void home_note_name(const char *id)
{
    note(id, true);
}

void home_forget(const char *id)
{
    static pending_t list[PENDING_MAX];
    pthread_mutex_lock(&g_lock);
    const int n = pending_read(list);
    int kept = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].id, id) != 0)
            list[kept++] = list[i];
    if (kept != n)
        pending_write(list, kept);
    pthread_mutex_unlock(&g_lock);
}

void home_pending(buf_t *out)
{
    static pending_t list[PENDING_MAX];
    pthread_mutex_lock(&g_lock);
    const int n = pending_read(list);
    for (int i = 0; i < n; i++)
        buf_printf(out, "P\t%s\t%s\n", list[i].id, list[i].name ? "name" : "art");
    pthread_mutex_unlock(&g_lock);
}

int home_renamed(char (*ids)[16], int max)
{
    static pending_t list[PENDING_MAX];
    pthread_mutex_lock(&g_lock);
    const int n = pending_read(list);
    int found = 0;
    for (int i = 0; i < n && found < max; i++)
        if (list[i].name)
            copy_str(ids[found++], sizeof(ids[0]), list[i].id);
    pthread_mutex_unlock(&g_lock);
    return found;
}

/* ---- how the last restart went ---- */

/* home.txt: phase, unseen (1/0), message, tab separated. g_lock held. */
static void save_result(void)
{
    char path[512];
    buf_t text = {0};
    buf_printf(&text, "%s\t%d\t", g_phase, g_unseen);
    buf_field(&text, g_message);
    buf_printf(&text, "\n");
    file_write_replace(data_path("home.txt", path, sizeof(path)), text.data, text.len, NULL);
    buf_free(&text);
}

static void load_result(void)
{
    if (g_loaded)
        return;
    g_loaded = true;
    char path[512];
    buf_t text = {0};
    if (file_read(data_path("home.txt", path, sizeof(path)), &text) && text.len > 0 && buf_append(&text, "", 1)) {
        char *line = (char *)text.data;
        line[strcspn(line, "\n")] = 0;
        char *phase = strtok(line, "\t"), *unseen = strtok(NULL, "\t"), *message = strtok(NULL, "\t");
        if (phase && unseen) {
            copy_str(g_phase, sizeof(g_phase), phase);
            g_unseen = strcmp(unseen, "1") == 0;
            copy_str(g_message, sizeof(g_message), message ? message : "");
        }
    }
    buf_free(&text);
}

static void set_phase(const char *phase, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void set_phase(const char *phase, const char *format, ...)
{
    pthread_mutex_lock(&g_lock);
    copy_str(g_phase, sizeof(g_phase), phase);
    va_list ap;
    va_start(ap, format);
    vsnprintf(g_message, sizeof(g_message), format, ap);
    va_end(ap);
    const bool end = strcmp(phase, "done") == 0 || strcmp(phase, "failed") == 0;
    if (end) {
        g_unseen = true;
        g_running = false;
        save_result();
    }
    log_line("home: %s: %s", phase, g_message);
    pthread_mutex_unlock(&g_lock);
}

/* ---- the restart ---- */

#ifndef ARTSWAP_HOST
static pid_t shellui_pid(void)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    pid_t found = -1;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
        return -1;
    size += size / 4; /* room for processes started meanwhile */
    char *procs = malloc(size);
    if (procs != NULL && sysctl(mib, 4, procs, &size, NULL, 0) == 0) {
        for (char *p = procs; p < procs + size;) {
            const struct kinfo_proc *ki = (const struct kinfo_proc *)p;
            if (ki->ki_structsize <= 0)
                break;
            p += ki->ki_structsize;
            if (strcmp(ki->ki_comm, "SceShellUI") == 0)
                found = ki->ki_pid;
        }
    }
    free(procs);
    return found;
}
#endif

static bool alive(pid_t pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

typedef struct {
    pid_t title;
    app_t *renamed; /* installed again before the restart */
    int nrenamed;
} restart_t;

static void *restart_job(void *argument)
{
    restart_t *job = argument;
    const pid_t title = job->title;
    /* Art Swap closes first, so the restart happens on the home screen it goes back to */
    for (int i = 0; i < 300 && alive(title); i++)
        usleep(100000);
    if (alive(title))
        log_line("home: Art Swap (pid %d) is still open after 30 s; restarting the home screen anyway", (int)title);
    usleep(1500000);

    /* renamed titles: installed again, so the home screen takes their new names */
    char failed_names[300] = "", why[480] = "";
    int failures = 0;
    for (int i = 0; i < job->nrenamed; i++) {
        const app_t *app = &job->renamed[i];
        set_phase("installing", "Installing %s again for its new name", app->name);
        char message[480];
        if (!smp_refresh_run(app, 60, message, sizeof(message))) {
            failures++;
            if (failures == 1)
                copy_str(why, sizeof(why), message);
            size_t used = strlen(failed_names);
            snprintf(failed_names + used, sizeof(failed_names) - used, "%s%s", used ? ", " : "", app->name);
        }
    }
    free(job->renamed);
    free(job);

    set_phase("restarting", "Restarting the home screen");
#ifdef ARTSWAP_HOST
    /* the tests' console has no home screen: a file says it would have restarted */
    char path[512];
    file_write_replace(data_path("home-restarted", path, sizeof(path)), "1\n", 2, NULL);
    const bool ok = true;
#else
    bool ok = false;
    const pid_t before = shellui_pid();
    if (before <= 0) {
        set_phase("failed", "The home screen's process (SceShellUI) was not found, so nothing was restarted");
        return NULL;
    }
    if (kill(before, SIGKILL) != 0) {
        set_phase("failed", "The home screen could not be restarted: %s", strerror(errno));
        return NULL;
    }
    pid_t after = -1;
    for (int i = 0; i < 300 && !ok; i++) {
        usleep(100000);
        after = shellui_pid();
        ok = after > 0 && after != before;
    }
    if (!ok) {
        set_phase("failed", "The home screen did not come back within 30 seconds");
        return NULL;
    }
    log_line("home: SceShellUI %d ended, %d started", (int)before, (int)after);
#endif
    /* what is left waiting: the renames whose install did not finish */
    pthread_mutex_lock(&g_lock);
    static pending_t list[PENDING_MAX];
    const int n = pending_read(list);
    int kept = 0;
    for (int i = 0; i < n; i++)
        if (list[i].name) {
            list[kept] = list[i];
            list[kept++].name = true;
        }
    pending_write(list, kept);
    pthread_mutex_unlock(&g_lock);
    if (failures > 0)
        set_phase("failed", "The home screen restarted, but %s %s not installed again, so %s new name does not show yet. %s",
                  failed_names, failures == 1 ? "was" : "were", failures == 1 ? "its" : "their", why);
    else
        set_phase("done", ok ? "The home screen restarted with every change" : "");
    return NULL;
}

bool home_restart_start(pid_t title_pid, const app_t *renamed, int nrenamed, char *err, size_t errlen)
{
    pthread_mutex_lock(&g_lock);
    load_result();
    if (g_running) {
        pthread_mutex_unlock(&g_lock);
        snprintf(err, errlen, "The home screen is restarting already");
        return false;
    }
    g_running = true;
    g_unseen = false;
    copy_str(g_phase, sizeof(g_phase), "waiting");
    snprintf(g_message, sizeof(g_message), "Waiting for Art Swap to close");
    pthread_mutex_unlock(&g_lock);
    log_line("home: restart asked, once pid %d has ended", (int)title_pid);

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    restart_t *job = calloc(1, sizeof(*job));
    app_t *copies = nrenamed > 0 ? calloc((size_t)nrenamed, sizeof(app_t)) : NULL;
    int rc = job == NULL || (nrenamed > 0 && copies == NULL) ? ENOMEM : 0;
    if (rc == 0) {
        job->title = title_pid;
        job->renamed = copies;
        job->nrenamed = nrenamed;
        if (nrenamed > 0)
            memcpy(copies, renamed, (size_t)nrenamed * sizeof(app_t));
        rc = pthread_create(&thread, &attr, restart_job, job);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(job);
        free(copies);
        pthread_mutex_lock(&g_lock);
        g_running = false;
        copy_str(g_phase, sizeof(g_phase), "none");
        pthread_mutex_unlock(&g_lock);
        snprintf(err, errlen, "The helper could not start the restart (%s)", strerror(rc));
        return false;
    }
    return true;
}

bool home_running(void)
{
    pthread_mutex_lock(&g_lock);
    const bool running = g_running;
    pthread_mutex_unlock(&g_lock);
    return running;
}

void home_line(buf_t *out)
{
    pthread_mutex_lock(&g_lock);
    load_result();
    buf_printf(out, "home\t%s\t%d\t", g_phase, g_unseen);
    buf_field(out, g_message);
    buf_printf(out, "\n");
    pthread_mutex_unlock(&g_lock);
}

void home_seen(void)
{
    pthread_mutex_lock(&g_lock);
    load_result();
    if (g_unseen) {
        g_unseen = false;
        save_result();
    }
    pthread_mutex_unlock(&g_lock);
}
