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
 * pending.txt keeps the titles changed since the last restart, one id a line,
 * for the title to say what is waiting.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "home.h"

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

static void pending_read(buf_t *text)
{
    char path[512];
    file_read(data_path("pending.txt", path, sizeof(path)), text);
}

static void pending_write(const buf_t *text)
{
    char path[512];
    file_write_replace(data_path("pending.txt", path, sizeof(path)), text->data ? text->data : (uint8_t *)"",
                       text->len, NULL);
}

/* Calls each(line, arg) for each id in text */
static void pending_each(const buf_t *text, void (*each)(const char *id, void *arg), void *arg)
{
    size_t at = 0;
    while (at < text->len) {
        size_t end = at;
        while (end < text->len && text->data[end] != '\n')
            end++;
        char id[16];
        const size_t n = end - at < sizeof(id) - 1 ? end - at : sizeof(id) - 1;
        memcpy(id, text->data + at, n);
        id[n] = 0;
        if (id[0])
            each(id, arg);
        at = end + 1;
    }
}

typedef struct {
    const char *id;
    bool found;
    buf_t *kept;
} pending_scan_t;

static void find_id(const char *id, void *arg)
{
    pending_scan_t *scan = arg;
    if (strcmp(id, scan->id) == 0)
        scan->found = true;
    else if (scan->kept)
        buf_printf(scan->kept, "%s\n", id);
}

void home_note(const char *id)
{
    pthread_mutex_lock(&g_lock);
    buf_t text = {0};
    pending_read(&text);
    pending_scan_t scan = {id, false, NULL};
    pending_each(&text, find_id, &scan);
    if (!scan.found) {
        buf_printf(&text, "%s\n", id);
        pending_write(&text);
    }
    buf_free(&text);
    pthread_mutex_unlock(&g_lock);
}

void home_forget(const char *id)
{
    pthread_mutex_lock(&g_lock);
    buf_t text = {0}, kept = {0};
    pending_read(&text);
    pending_scan_t scan = {id, false, &kept};
    pending_each(&text, find_id, &scan);
    if (scan.found)
        pending_write(&kept);
    buf_free(&text);
    buf_free(&kept);
    pthread_mutex_unlock(&g_lock);
}

static void print_id(const char *id, void *arg)
{
    buf_printf(arg, "P\t%s\n", id);
}

void home_pending(buf_t *out)
{
    pthread_mutex_lock(&g_lock);
    buf_t text = {0};
    pending_read(&text);
    pending_each(&text, print_id, out);
    buf_free(&text);
    pthread_mutex_unlock(&g_lock);
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

static void *restart_job(void *argument)
{
    const pid_t title = (pid_t)(intptr_t)argument;
    /* Art Swap closes first, so the restart happens on the home screen it goes back to */
    for (int i = 0; i < 300 && alive(title); i++)
        usleep(100000);
    if (alive(title))
        log_line("home: Art Swap (pid %d) is still open after 30 s; restarting the home screen anyway", (int)title);
    usleep(1500000);
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
    pthread_mutex_lock(&g_lock);
    const buf_t none = {0};
    pending_write(&none);
    pthread_mutex_unlock(&g_lock);
    set_phase("done", ok ? "The home screen restarted with every change" : "");
    return NULL;
}

bool home_restart_start(pid_t title_pid, char *err, size_t errlen)
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
    const int rc = pthread_create(&thread, &attr, restart_job, (void *)(intptr_t)title_pid);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
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
