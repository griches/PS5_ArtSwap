/*
 * Art Swap helper - the network on the console, step by step (a test payload).
 *
 *   make -C helper nettest   -> build/helper/nettest.elf; send it to the ELF loader
 *
 * Makes the helper's own requests (http_ps5.c), each on a thread of its own
 * with a clock that logs every five seconds it is still waiting, to
 * /data/artswap/nettest.log: SteamGridDB's API with a key that is not one (a
 * quick 401 means HTTPS works), a picture from its CDN, and plain HTTP.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/artswap.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *url;
    const char *bearer;
    volatile bool done;
} job_t;

static void *fetch(void *argument)
{
    job_t *job = argument;
    buf_t body = {0};
    int status = 0;
    char err[300] = "";
    const uint64_t start = now_ms();
    const bool ok = http_get(job->url, job->bearer, &body, &status, err, sizeof(err));
    log_line("RESULT %s: %s, status %d, %zu bytes, %llu ms %s", job->url, ok ? "ok" : "failed", status, body.len,
             (unsigned long long)(now_ms() - start), err);
    if (body.len)
        log_line("BODY %.200s", (const char *)body.data);
    buf_free(&body);
    job->done = true;
    return NULL;
}

static void run(const char *url, const char *bearer, int seconds)
{
    job_t job = {url, bearer, false};
    pthread_t thread;
    log_line("TEST %s", url);
    pthread_create(&thread, NULL, fetch, &job);
    for (int waited = 0; !job.done && waited < seconds; waited++) {
        sleep(1);
        if (!job.done && waited % 5 == 4)
            log_line("... still waiting after %d s", waited + 1);
    }
    if (!job.done) {
        log_line("GAVE UP after %d s: %s", seconds, url);
        pthread_detach(thread);
    } else {
        pthread_join(thread, NULL);
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char folder[512];
    mkdirs(data_path("cache", folder, sizeof(folder)));
    log_open_named("nettest.log");
    log_quiet(true);
    log_line("nettest starting");
    char err[300] = "";
    if (!http_init(err, sizeof(err))) {
        log_line("http_init failed: %s", err);
        return 1;
    }
    run("https://www.steamgriddb.com/api/v2/search/autocomplete/portal", "0123456789abcdef0123456789abcdef", 60);
    run("https://cdn2.steamgriddb.com/thumb/dce5eb1f56dd8dd4e1b9a1e22c0e1be0.jpg", NULL, 40);
    run("http://example.com/", NULL, 30);
    log_line("nettest done");
    return 0;
}
